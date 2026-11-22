#include "uos/kernel/attestation.hpp"
#include "uos/rt/memory.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

namespace uos::kernel::attestation {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `readonly CryptoKey[]`
    using KernelPublicKeys = std::vector<rt::crypto::CryptoKey>;

    // JavaScriptCore's rendering of the value named in "<expression> is not a function. (In
    // '<call>', '<expression>' is <value>)".
    std::string describeValue(const rt::KvValue& value) {
      if (value.isUndefined()) return "undefined";
      if (value.isNull()) return "null";
      if (value.isBool()) return value.asBool() ? "true" : "false";
      if (value.isNumber()) return rt::numberToString(value.asNumber());
      if (value.isString()) return "\"" + value.asString() + "\"";
      if (value.isBigInt()) return value.asBigInt().toString();
      if (value.is<rt::KvError>()) return "an instance of " + value.as<rt::KvError>().name;
      return "an instance of " + std::string(value.kind());
    }

    // Property `name` of a stored value, as JS reads it: an own member of a plain object;
    // nothing the other stored types expose (no stored value has a function member).
    const rt::KvValue& property(const rt::KvValue& value, std::string_view name) {
      static const rt::KvValue undefined;
      return value.isObject() ? value[name] : undefined;
    }

    std::string normalizePemValue(std::string_view raw) {
      static const rt::Regex ESCAPED_NEWLINE("\\\\n", "g");
      return rt::replace(rt::trim(raw), ESCAPED_NEWLINE, "\n");
    }

    std::vector<std::string> extractPemBlocks(std::string_view raw, std::string_view begin, std::string_view end) {
      std::vector<std::string> blocks;
      std::int64_t cursor = 0;
      while (cursor < rt::length(raw)) {
        const std::int64_t start = rt::indexOf(raw, begin, cursor);
        if (start == -1) break;
        const std::int64_t finish = rt::indexOf(raw, end, start);
        if (finish == -1) break;
        blocks.push_back(rt::trim(rt::slice(raw, start, finish + rt::length(end))));
        cursor = finish + rt::length(end);
      }
      return blocks;
    }

    const rt::KvKey UOS_KERNEL_PUBKEYS_KEY{"uos_ai", "kernel_pubkeys"};

    // `kvEntry.value.map((p) => p.pem)` over whatever the `{ pem: string }[]` row holds, then
    // spread: the elements a truthy stored value yields, or the TypeError JS throws.
    std::vector<rt::KvValue> storedPems(const rt::KvValue& value) {
      std::vector<rt::KvValue> mapped;
      if (value.isArray()) {
        for (const rt::KvValue& p : value.asArray()) {
          // map skips a hole; the spread reads it back as undefined.
          if (p.is<rt::KvHole>()) {
            mapped.emplace_back();
            continue;
          }
          if (p.isNull()) throw rt::TypeError("null is not an object (evaluating 'p.pem')");
          if (p.isUndefined()) throw rt::TypeError("undefined is not an object (evaluating 'p.pem')");
          mapped.push_back(property(p, "pem"));
        }
        return mapped;
      }
      // Typed arrays have a map too: every element is the callback's undefined converted to
      // the element type.
      std::size_t elements = 0;
      double element = 0;
      bool typedArray = false;
      bool bigIntElements = false;
      if (value.isBytes()) {
        typedArray = true;
        elements = value.asBytes().size();
      } else if (value.is<rt::KvArrayBufferView>()) {
        const rt::KvArrayBufferView& view = value.as<rt::KvArrayBufferView>();
        std::size_t elementSize = 1;
        switch (view.kind) {
          case rt::KvViewKind::DataView: break;
          case rt::KvViewKind::Int8Array:
          case rt::KvViewKind::Uint8Array:
          case rt::KvViewKind::Uint8ClampedArray: typedArray = true; break;
          case rt::KvViewKind::Int16Array:
          case rt::KvViewKind::Uint16Array:
            typedArray = true;
            elementSize = 2;
            break;
          case rt::KvViewKind::Int32Array:
          case rt::KvViewKind::Uint32Array:
            typedArray = true;
            elementSize = 4;
            break;
          case rt::KvViewKind::Float32Array:
            typedArray = true;
            elementSize = 4;
            element = rt::Number::NaN;
            break;
          case rt::KvViewKind::Float64Array:
            typedArray = true;
            elementSize = 8;
            element = rt::Number::NaN;
            break;
          case rt::KvViewKind::BigInt64Array:
          case rt::KvViewKind::BigUint64Array:
            typedArray = true;
            bigIntElements = true;
            elementSize = 8;
            break;
        }
        elements = static_cast<std::size_t>(view.byteLength / elementSize);
      }
      if (typedArray) {
        if (bigIntElements && elements > 0) throw rt::TypeError("Invalid argument type in ToBigInt operation");
        for (std::size_t index = 0; index < elements; index += 1) mapped.emplace_back(element);
        return mapped;
      }
      throw rt::TypeError("kvEntry.value.map is not a function. (In 'kvEntry.value.map((p) => p.pem)', 'kvEntry.value.map' is " +
                          describeValue(property(value, "map")) + ")");
    }

    rt::Task<std::vector<rt::KvValue>> getKernelPublicKeyPems() {
      const std::string envRaw = rt::trim(getEnv("UOS_AI_KERNEL_PUBLIC_KEY").value_or(""));
      std::vector<rt::KvValue> tokens;

      if (!envRaw.empty()) {
        const std::string normalized = normalizePemValue(envRaw);
        const std::vector<std::string> blocks = extractPemBlocks(normalized, "-----BEGIN PUBLIC KEY-----", "-----END PUBLIC KEY-----");
        if (!blocks.empty()) {
          for (const std::string& block : blocks) tokens.emplace_back(block);
        } else {
          tokens.emplace_back(normalized);
        }
      }

      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      const std::shared_ptr<rt::Kv> store = co_await opening.wait();
      if (store) {
        const rt::KvEntryMaybe<> kvEntry = co_await store->get(UOS_KERNEL_PUBKEYS_KEY);
        if (kvEntry.value.truthy()) {
          std::vector<rt::KvValue> pems = storedPems(kvEntry.value);
          for (rt::KvValue& pem : pems) tokens.push_back(std::move(pem));
        }
      }

      co_return tokens;
    }

    rt::Task<rt::crypto::CryptoKey> importRsaPublicKey(rt::KvValue publicKeyPem) {
      if (publicKeyPem.isNull()) throw rt::TypeError("null is not an object (evaluating 'publicKeyPem.replace')");
      if (publicKeyPem.isUndefined()) throw rt::TypeError("undefined is not an object (evaluating 'publicKeyPem.replace')");
      if (!publicKeyPem.isString()) {
        throw rt::TypeError(
          "publicKeyPem.replace is not a function. (In 'publicKeyPem.replace(\"-----BEGIN PUBLIC KEY-----\", \"\")', 'publicKeyPem.replace' is " +
          describeValue(property(publicKeyPem, "replace")) + ")");
      }
      static const rt::Regex WHITESPACE("\\s+", "g");
      const std::string pemContents =
        rt::replace(rt::trim(rt::replace(rt::replace(publicKeyPem.asString(), "-----BEGIN PUBLIC KEY-----", ""), "-----END PUBLIC KEY-----", "")), WHITESPACE, "");

      const std::vector<std::uint8_t> binaryDer = rt::atobBytes(pemContents);
      const rt::crypto::Algorithm algorithm{.name = "RSASSA-PKCS1-v1_5", .hash = "SHA-256"};
      const std::vector<std::string> usages{"verify"};
      rt::crypto::CryptoKey key = rt::crypto::subtle::importKey("spki", binaryDer, algorithm, true, usages);
      co_await rt::microtask();
      co_return key;
    }

    constexpr double KERNEL_PUBLIC_KEYS_REFRESH_MS = 60'000;

    // `let kernelPublicKeysPromise: Promise<readonly CryptoKey[]> | null` and `let kernelPublicKeysLoadedAtMs`.
    struct KernelPublicKeysState {
      std::optional<rt::Promise<KernelPublicKeys>> promise;
      double loadedAtMs = 0;
    };

    KernelPublicKeysState& kernelPublicKeysState() {
      static KernelPublicKeysState state;
      return state;
    }

    rt::Task<KernelPublicKeys> loadKernelPublicKeys() {
      const std::vector<rt::KvValue> pems = co_await getKernelPublicKeyPems();
      KernelPublicKeys keys;
      std::size_t bytes = 0;
      for (const rt::KvValue& pem : pems) {
        rt::Reason error;
        try {
          rt::Task<rt::crypto::CryptoKey> importing = importRsaPublicKey(pem);
          rt::crypto::CryptoKey key = co_await std::move(importing);
          keys.push_back(std::move(key));
          bytes += pem.isString() ? pem.asString().size() : 0;
        } catch (...) {
          error = std::current_exception();
        }
        if (error) rt::console::error("[ai.ubq.fi] Failed to import kernel public key:", error);
      }
      rt::memory::setCache(rt::memory::Cache::kernel_public_keys, keys.size(), bytes + keys.capacity() * sizeof(rt::crypto::CryptoKey));
      co_return keys;
    }

    // The async arrow `getKernelPublicKeys` stores: load, then stamp the load time.
    rt::Task<KernelPublicKeys> loadAndStampKernelPublicKeys() {
      KernelPublicKeys keys = co_await loadKernelPublicKeys();
      kernelPublicKeysState().loadedAtMs = rt::dateNow();
      co_return keys;
    }

    // `getKernelPublicKeys(forceReload = false)`
    rt::Promise<KernelPublicKeys> getKernelPublicKeys(bool forceReload) {
      KernelPublicKeysState& state = kernelPublicKeysState();
      const double now = rt::dateNow();
      if (!forceReload && state.promise && now - state.loadedAtMs < KERNEL_PUBLIC_KEYS_REFRESH_MS) {
        return *state.promise;
      }
      state.promise = rt::Promise<KernelPublicKeys>(loadAndStampKernelPublicKeys());
      return *state.promise;
    }

    std::vector<std::uint8_t> decodeBase64UrlToBytes(std::string_view raw) {
      static const rt::Regex DASH("-", "g");
      static const rt::Regex UNDERSCORE("_", "g");
      const std::string b64 = rt::replace(rt::replace(raw, DASH, "+"), UNDERSCORE, "/");
      const std::string padded = b64 + rt::repeat("=", (4 - (rt::length(b64) % 4)) % 4);
      return rt::atobBytes(padded);
    }

    json::value decodeBase64UrlToJson(std::string_view raw) {
      const std::vector<std::uint8_t> bytes = decodeBase64UrlToBytes(raw);
      const std::string text = rt::decodeUtf8(bytes);
      return rt::jsonParse(text);
    }

    // `typeof v === "number" && Number.isFinite(v) ? Math.trunc(v) : null`
    std::optional<double> finiteInteger(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (!number || !rt::Number::isFinite(*number)) return std::nullopt;
      return rt::Math::trunc(*number);
    }

    std::optional<KernelAttestationPayload> parseKernelAttestationPayload(const json::value& value) {
      if (!utils::isRecord(value)) return std::nullopt;

      const std::optional<std::string> iss = utils::getString(rt::get(value, "iss"));
      const std::optional<std::string> aud = utils::getString(rt::get(value, "aud"));
      const std::optional<std::string> jti = utils::getString(rt::get(value, "jti"));
      const std::optional<std::string> owner = utils::getString(rt::get(value, "owner"));
      const std::optional<std::string> repo = utils::getString(rt::get(value, "repo"));
      const std::optional<std::string> authTokenSha = utils::getString(rt::get(value, "auth_token_sha256"));
      const std::optional<std::string> stateId = utils::getString(rt::get(value, "state_id"));

      const std::optional<double> iat = finiteInteger(rt::get(value, "iat"));
      const std::optional<double> exp = finiteInteger(rt::get(value, "exp"));
      if (!iat || !exp) return std::nullopt;

      const json::value* installationIdValue = rt::get(value, "installation_id");
      // `installationIdValue !== null`: undefined (an absent member) is not null.
      const bool installationIdIsNull = installationIdValue != nullptr && installationIdValue->is_null();
      std::optional<double> installationId;
      if (!installationIdIsNull) installationId = finiteInteger(installationIdValue);
      if (!installationIdIsNull && !installationId) return std::nullopt;

      if (iss != "ubiquity-os-kernel") return std::nullopt;
      if (aud != "ai.ubq.fi") return std::nullopt;
      if (!rt::truthy(jti) || !rt::truthy(owner) || !rt::truthy(repo) || !rt::truthy(authTokenSha) || !rt::truthy(stateId)) return std::nullopt;

      return KernelAttestationPayload{
        .iss = "ubiquity-os-kernel",
        .aud = "ai.ubq.fi",
        .iat = *iat,
        .exp = *exp,
        .jti = *jti,
        .owner = *owner,
        .repo = *repo,
        .installation_id = installationId,
        .auth_token_sha256 = *authTokenSha,
        .state_id = *stateId,
      };
    }

    constexpr double KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS = 60;
    constexpr double KERNEL_ATTESTATION_MAX_TTL_SECONDS = 60 * 60;

    KernelAttestationFailure attestationFailure(double status, std::string message, std::string code) {
      return KernelAttestationFailure{.response = http::openaiError(status, std::move(message), std::move(code))};
    }

    std::optional<KernelAttestationFailure> kernelAttestationClockFailure(const KernelAttestationPayload& payload, double now) {
      if (payload.exp < payload.iat) {
        return attestationFailure(401,
                                  "Unauthorized: kernel attestation 'exp' (" + rt::numberToString(payload.exp) + ") is before 'iat' (" + rt::numberToString(payload.iat) + ")",
                                  "invalid_kernel_token");
      }
      if (payload.exp - payload.iat > KERNEL_ATTESTATION_MAX_TTL_SECONDS) {
        return attestationFailure(401,
                                  "Unauthorized: kernel attestation TTL is too long (" + rt::numberToString(payload.exp - payload.iat) + "s > " +
                                    rt::numberToString(KERNEL_ATTESTATION_MAX_TTL_SECONDS) + "s)",
                                  "invalid_kernel_token");
      }
      if (payload.iat > now + KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS) {
        return attestationFailure(
          401, "Unauthorized: kernel attestation 'iat' (" + rt::numberToString(payload.iat) + ") is in the future (server now: " + rt::numberToString(now) + ")",
          "invalid_kernel_token");
      }
      if (payload.exp < now - KERNEL_ATTESTATION_CLOCK_SKEW_SECONDS) {
        return attestationFailure(
          401, "Unauthorized: kernel attestation 'exp' (" + rt::numberToString(payload.exp) + ") is in the past (server now: " + rt::numberToString(now) + ")",
          "invalid_kernel_token");
      }
      return std::nullopt;
    }

    // `owner` / `repo`: `string | null | undefined`.
    std::optional<KernelAttestationFailure> kernelAttestationRepoFailure(const KernelAttestationPayload& payload, const rt::Nullable<std::string>& owner,
                                                                         const rt::Nullable<std::string>& repo) {
      const std::string expectedOwner = owner.isNullish() ? "" : rt::trim(*owner);
      const std::string expectedRepo = repo.isNullish() ? "" : rt::trim(*repo);
      if (!expectedOwner.empty() && !expectedRepo.empty() && (payload.owner != expectedOwner || payload.repo != expectedRepo)) {
        return attestationFailure(401,
                                  "Unauthorized: kernel attestation repo mismatch. Expected '" + expectedOwner + "/" + expectedRepo + "', got '" + payload.owner + "/" +
                                    payload.repo + "'",
                                  "invalid_kernel_token");
      }
      return std::nullopt;
    }

    // `number | null`
    std::optional<double> parseInstallationIdHeader(const rt::Request& req) {
      const std::string raw = rt::trim(req.headers().get("X-GitHub-Installation-Id").value_or(""));
      if (raw.empty()) return std::nullopt;
      const double num = rt::stringToNumber(raw);
      if (!rt::Number::isFinite(num)) return std::nullopt;
      return rt::Math::trunc(num);
    }

    std::optional<KernelAttestationFailure> kernelAttestationInstallationFailure(const rt::Request& req, const KernelAttestationPayload& payload) {
      if (!payload.installation_id) return std::nullopt;
      const std::optional<double> installationId = parseInstallationIdHeader(req);
      if (!installationId) {
        return attestationFailure(401, "Unauthorized: missing 'X-GitHub-Installation-Id' header, required for kernel attestation verification", "missing_installation_id");
      }
      if (*payload.installation_id != *installationId) {
        return attestationFailure(401,
                                  "Unauthorized: kernel attestation installation_id mismatch. Expected '" + rt::numberToString(*installationId) + "', got '" +
                                    rt::numberToString(*payload.installation_id) + "'",
                                  "invalid_kernel_token");
      }
      return std::nullopt;
    }

    // `{ ok: true; header: unknown; payload: KernelAttestationPayload | null }`
    struct KernelAttestationParts {
      json::value header;
      std::optional<KernelAttestationPayload> payload;
    };

    // `err instanceof Error ? err.message : String(err)`
    std::string errorText(const rt::Reason& err) { return rt::isError(err) ? rt::errorMessage(err).value_or("") : rt::reasonToString(err); }

    std::variant<KernelAttestationParts, KernelAttestationFailure> parseKernelAttestationParts(const std::vector<std::string>& parts) {
      rt::Reason err;
      try {
        json::value header = decodeBase64UrlToJson(parts[0]);
        const json::value payload = decodeBase64UrlToJson(parts[1]);
        return KernelAttestationParts{.header = std::move(header), .payload = parseKernelAttestationPayload(payload)};
      } catch (...) {
        err = std::current_exception();
      }
      return attestationFailure(401, "Unauthorized: failed to parse kernel attestation JWT parts: " + errorText(err), "invalid_kernel_token");
    }

    // The `verifySignature` closure: any key that verifies wins; a rejected verify tries the next key.
    rt::Task<bool> verifySignature(KernelPublicKeys candidateKeys, std::shared_ptr<const std::vector<std::uint8_t>> signatureBytes,
                                   std::shared_ptr<const std::vector<std::uint8_t>> dataArray) {
      for (const rt::crypto::CryptoKey& key : candidateKeys) {
        bool ok = false;
        try {
          ok = rt::crypto::subtle::verify("RSASSA-PKCS1-v1_5", key, *signatureBytes, *dataArray);
        } catch (...) {
          // ignore and try next key
          ok = false;
        }
        co_await rt::microtask();
        if (ok) co_return true;
      }
      co_return false;
    }

    rt::Task<std::optional<KernelAttestationFailure>> kernelAttestationSignatureFailure(std::vector<std::string> parts, KernelPublicKeys keys) {
      const std::string signingInput = parts[0] + "." + parts[1];
      const auto dataArray = std::make_shared<const std::vector<std::uint8_t>>(rt::encodeUtf8(signingInput));
      std::shared_ptr<const std::vector<std::uint8_t>> signatureBytes;
      rt::Reason err;
      try {
        signatureBytes = std::make_shared<const std::vector<std::uint8_t>>(decodeBase64UrlToBytes(parts[2]));
      } catch (...) {
        err = std::current_exception();
      }
      if (!signatureBytes) {
        co_return attestationFailure(401, "Unauthorized: failed to decode kernel attestation signature: " + errorText(err), "invalid_kernel_token");
      }

      rt::Task<bool> verifying = verifySignature(std::move(keys), signatureBytes, dataArray);
      bool signatureValid = co_await std::move(verifying);
      if (!signatureValid) {
        const rt::Promise<KernelPublicKeys> refreshing = getKernelPublicKeys(true);
        KernelPublicKeys refreshedKeys = co_await refreshing.wait();
        if (refreshedKeys.empty()) {
          co_return attestationFailure(500, "Server misconfigured: No kernel public keys loaded. Set 'UOS_AI_KERNEL_PUBLIC_KEY' or use admin endpoints to add pubkeys.",
                                       "server_error");
        }
        rt::Task<bool> reverifying = verifySignature(std::move(refreshedKeys), signatureBytes, dataArray);
        signatureValid = co_await std::move(reverifying);
      }
      if (!signatureValid) co_return attestationFailure(401, "Unauthorized (invalid kernel attestation)", "invalid_kernel_token");
      co_return std::nullopt;
    }

    rt::OrderedMap<std::string, double>& kernelTokenJtiCache() {
      static rt::OrderedMap<std::string, double> cache;
      return cache;
    }

    std::size_t jtiCacheBytes = 0;

    std::size_t tokenIdBytes(const std::string& jti) {
      return sizeof(double) + 2 * (sizeof(jti) + rt::memory::bytes(jti));
    }

    void pruneKernelTokenJtiCache() {
      const double now = rt::dateNow();
      rt::OrderedMap<std::string, double>& cache = kernelTokenJtiCache();
      for (const auto& [jti, expiresAtMs] : cache) {
        if (expiresAtMs <= now) {
          const std::string expired = jti;
          jtiCacheBytes -= tokenIdBytes(jti);
          cache.delete_(expired);
        }
      }
    }

    KernelAttestationResult failed(KernelAttestationFailure failure) { return KernelAttestationResult(std::move(failure)); }
  }

  std::optional<std::string> getEnv(std::string_view key) {
    try {
      return rt::env::get(key);
    } catch (...) {
      return std::nullopt;
    }
  }

  rt::Task<void> reloadKernelPublicKeys() {
    const rt::Promise<KernelPublicKeys> reloading = getKernelPublicKeys(true);
    co_await reloading.wait();
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAttestationPayload& value) {
    out = Writer()
            .required("iss", value.iss)
            .required("aud", value.aud)
            .required("iat", value.iat)
            .required("exp", value.exp)
            .required("jti", value.jti)
            .required("owner", value.owner)
            .required("repo", value.repo)
            .orNull("installation_id", value.installation_id)
            .required("auth_token_sha256", value.auth_token_sha256)
            .required("state_id", value.state_id)
            .done();
  }

  KernelAttestationPayload tag_invoke(const json::value_to_tag<KernelAttestationPayload>&, const json::value& value) {
    const Reader in(value, "KernelAttestationPayload");
    return {
      .iss = in.required<std::string>("iss"),
      .aud = in.required<std::string>("aud"),
      .iat = in.required<double>("iat"),
      .exp = in.required<double>("exp"),
      .jti = in.required<std::string>("jti"),
      .owner = in.required<std::string>("owner"),
      .repo = in.required<std::string>("repo"),
      .installation_id = in.orNull<double>("installation_id"),
      .auth_token_sha256 = in.required<std::string>("auth_token_sha256"),
      .state_id = in.required<std::string>("state_id"),
    };
  }

  rt::Task<KernelAttestationResult> verifyKernelAttestation(rt::Request req, VerifyKernelAttestationOptions options) {
    const std::string kernelToken = rt::trim(req.headers().get("X-Ubiquity-Kernel-Token").value_or(""));
    if (kernelToken.empty()) {
      co_return failed(attestationFailure(401, "Unauthorized: missing 'X-Ubiquity-Kernel-Token' header for kernel attestation", "missing_kernel_token"));
    }

    const rt::Promise<KernelPublicKeys> loading = getKernelPublicKeys(false);
    const KernelPublicKeys keys = co_await loading.wait();
    if (keys.empty()) {
      co_return failed(attestationFailure(500, "Server misconfigured: No kernel public keys loaded. Set 'UOS_AI_KERNEL_PUBLIC_KEY' or use admin endpoints to add pubkeys.",
                                          "server_error"));
    }

    const std::vector<std::string> parts = rt::split(kernelToken, ".");
    if (parts.size() != 3) {
      co_return failed(attestationFailure(401, "Unauthorized: kernel attestation JWT MUST have 3 parts (header, payload, signature)", "invalid_kernel_token"));
    }

    std::variant<KernelAttestationParts, KernelAttestationFailure> parsed = parseKernelAttestationParts(parts);
    if (KernelAttestationFailure* failure = std::get_if<KernelAttestationFailure>(&parsed)) co_return failed(std::move(*failure));
    const json::value header = std::get<KernelAttestationParts>(parsed).header;
    const std::optional<KernelAttestationPayload> payload = std::get<KernelAttestationParts>(parsed).payload;

    // The diagnostic keeps the original shape: a JSON scalar renders the way plain
    // interpolation would, and a non-record header renders 'undefined'.
    std::optional<json::value> headerAlgorithm;  // `unknown`: std::nullopt is undefined
    if (utils::isRecord(header)) {
      if (const json::value* alg = rt::get(header, "alg")) headerAlgorithm = *alg;
    } else {
      headerAlgorithm = json::value("undefined");
    }
    if (utils::getString(headerAlgorithm ? &*headerAlgorithm : nullptr) != "RS256") {
      const std::string rendered = !headerAlgorithm                ? std::string("undefined")
                                   : headerAlgorithm->is_string() ? std::string(headerAlgorithm->get_string())
                                                                  : rt::jsonStringify(*headerAlgorithm);
      co_return failed(attestationFailure(401, "Unauthorized: kernel attestation JWT 'alg' MUST be 'RS256', got '" + rendered + "'", "invalid_kernel_token"));
    }

    if (!payload) {
      co_return failed(attestationFailure(401, "Unauthorized: kernel attestation JWT payload is invalid or fields are missing/type-mismatched", "invalid_kernel_token"));
    }

    const double now = rt::Math::floor(rt::dateNow() / 1000);
    std::optional<KernelAttestationFailure> clockFailure = kernelAttestationClockFailure(*payload, now);
    if (clockFailure) co_return failed(std::move(*clockFailure));

    std::optional<KernelAttestationFailure> repoFailure = kernelAttestationRepoFailure(*payload, options.owner, options.repo);
    if (repoFailure) co_return failed(std::move(*repoFailure));

    std::optional<KernelAttestationFailure> installationFailure = kernelAttestationInstallationFailure(req, *payload);
    if (installationFailure) co_return failed(std::move(*installationFailure));

    rt::Task<std::string> hashing = utils::sha256Base64Url(options.token);
    const std::string expectedTokenSha = co_await std::move(hashing);
    if (payload->auth_token_sha256 != expectedTokenSha) {
      co_return failed(attestationFailure(401, "Unauthorized: kernel attestation 'auth_token_sha256' mismatch. Verify kernel is hashing the current 'authToken'.",
                                          "invalid_kernel_token"));
    }

    rt::Task<std::optional<KernelAttestationFailure>> checking = kernelAttestationSignatureFailure(parts, keys);
    std::optional<KernelAttestationFailure> signatureFailure = co_await std::move(checking);
    if (signatureFailure) co_return failed(std::move(*signatureFailure));

    pruneKernelTokenJtiCache();
    // This token is expected to be re-used within its TTL (e.g. Codex tool-calling
    // results in multiple /v1/responses requests). Enforcing single-use "jti"
    // semantics breaks legitimate traffic and is unreliable in serverless anyway.
    auto& ids = kernelTokenJtiCache();
    if (!ids.has(payload->jti)) jtiCacheBytes += tokenIdBytes(payload->jti);
    ids.set(payload->jti, payload->exp * 1000);
    rt::memory::setCache(rt::memory::Cache::kernel_attestation_ids, ids.size(), jtiCacheBytes);

    co_return KernelAttestationResult(KernelAttestationSuccess{.payload = *payload});
  }

  std::optional<GitHubRepo> getGitHubRepoHeaders(const rt::Request& req) {
    std::string owner = rt::trim(req.headers().get("X-GitHub-Owner").value_or(""));
    std::string repo = rt::trim(req.headers().get("X-GitHub-Repo").value_or(""));
    if (owner.empty() || repo.empty()) return std::nullopt;
    return GitHubRepo{.owner = std::move(owner), .repo = std::move(repo)};
  }

  rt::Task<std::optional<GitHubRepo>> getKernelAttestationContext(rt::Request req, std::optional<std::string> token) {
    if (!rt::truthy(token)) co_return std::nullopt;
    const std::string kernelToken = rt::trim(req.headers().get("X-Ubiquity-Kernel-Token").value_or(""));
    if (kernelToken.empty()) co_return std::nullopt;
    const std::optional<GitHubRepo> repoHeaders = getGitHubRepoHeaders(req);
    VerifyKernelAttestationOptions options;
    options.token = *token;
    if (repoHeaders) {
      options.owner = repoHeaders->owner;
      options.repo = repoHeaders->repo;
    }
    rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(req, std::move(options));
    const KernelAttestationResult attestation = co_await std::move(verifying);
    const KernelAttestationSuccess* verified = std::get_if<KernelAttestationSuccess>(&attestation);
    if (!verified) co_return std::nullopt;
    co_return GitHubRepo{.owner = verified->payload.owner, .repo = verified->payload.repo};
  }
}
