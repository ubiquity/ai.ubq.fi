// Port of tests/kernel-coverage.test.ts (every case). The five kernel attestation cases drive
// src/kernel/attestation.ts. The TS installs one CountingKv for the whole file; these cases only
// seed the kernel public key row and read it back through `kv.get`, so a fresh rt::MemoryKv
// installed with setKvForTest stands in for each `clearData()`. The three kernel quota cases drive
// src/kernel/quota-v2.ts over the shared CountingKv (tests/helpers/counting-kv.ts), whose listing
// includes the prefix row itself and orders keys by their JSON text, as the TS fixture does.
// Links: tests/kernel_coverage_test.cpp src/kernel/attestation.cpp src/http.cpp src/auth/relay.cpp src/config.cpp
//   src/provider/cerebras_rate_limits.cpp src/kv.cpp src/storage/kv.cpp src/utils.cpp src/release.cpp
//   src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
//   src/kernel/quota_v2.cpp src/authority/native_kernel.cpp tests/helpers/counting_kv.cpp
#include "uos/kernel/attestation.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"

#include "tests/helpers/counting_kv.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::kernel::attestation;
using Bytes = std::vector<std::uint8_t>;
using Headers = std::vector<std::pair<std::string, std::string>>;

namespace {
  const rt::KvKey KERNEL_PUBKEYS_KEY{"uos_ai", "kernel_pubkeys"};

  // The file's `kv` fixture (installed with setKvForTest): `clearData()` empties it, `seed()` writes a row.
  struct KernelKv {
    std::shared_ptr<rt::MemoryKv> store;

    void clearData() {
      store = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
      uos::kv::setKvForTest(store);
    }

    rt::Task<void> seed(rt::KvKey key, rt::KvValue value) {
      std::shared_ptr<rt::MemoryKv> current = store;
      co_await current->set(std::move(key), std::move(value));
    }
  };

  // The kernel public key the environment could supply would change every expectation below.
  rt::env::ScopedOverride withoutEnvironmentKey() { return rt::env::ScopedOverride({{"UOS_AI_KERNEL_PUBLIC_KEY", std::nullopt}}); }

  std::string base64Url(const Bytes& bytes) {
    static const rt::Regex PLUS("\\+", "g");
    static const rt::Regex SLASH("\\/", "g");
    const std::string encoded = rt::replace(rt::replace(rt::toBase64(bytes), PLUS, "-"), SLASH, "_");
    // Strip the trailing `=` padding without a `=+$` regex: an unbounded
    // quantifier before `$` is a super-linear backtracking pattern.
    std::int64_t end = rt::length(encoded);
    while (end > 0 && rt::charAt(encoded, end - 1) == "=") end -= 1;
    return rt::slice(encoded, 0, end);
  }

  std::string base64UrlJson(const json::value& value) { return base64Url(rt::encodeUtf8(rt::jsonStringify(value))); }

  std::string pemFromSpki(const Bytes& spki) {
    static const rt::Regex LINE(".{1,64}", "g");
    const std::vector<std::string> lines = rt::matchGlobal(rt::toBase64(spki), LINE).value_or(std::vector<std::string>{});
    std::string joined;
    for (std::size_t index = 0; index < lines.size(); index += 1) joined += (index == 0 ? "" : "\n") + lines[index];
    return "-----BEGIN PUBLIC KEY-----\n" + joined + "\n-----END PUBLIC KEY-----";
  }

  struct AttestationKeys {
    rt::crypto::CryptoKey privateKey;
    std::string pem;
  };

  AttestationKeys createAttestationKeys() {
    const rt::crypto::Algorithm params{.name = "RSASSA-PKCS1-v1_5", .hash = "SHA-256", .modulusLength = 2048, .publicExponent = Bytes{1, 0, 1}};
    const rt::crypto::CryptoKeyPair pair = rt::crypto::subtle::generateKey(params, true, {"sign", "verify"});
    return AttestationKeys{.privateKey = pair.privateKey, .pem = pemFromSpki(rt::crypto::subtle::exportKey("spki", pair.publicKey))};
  }

  json::object defaultHeader() { return json::object{{"alg", "RS256"}, {"typ", "JWT"}}; }

  std::string signAttestation(const AttestationKeys& keys, const json::object& payload, const json::object& header) {
    const std::string signingInput = base64UrlJson(header) + "." + base64UrlJson(payload);
    const Bytes signature = rt::crypto::subtle::sign("RSASSA-PKCS1-v1_5", keys.privateKey, rt::encodeUtf8(signingInput));
    return signingInput + "." + base64Url(signature);
  }

  std::string signAttestation(const AttestationKeys& keys, const json::object& payload) { return signAttestation(keys, payload, defaultHeader()); }

  rt::Task<json::object> attestationPayload(std::string token, json::object overrides) {
    const double nowSeconds = rt::Math::floor(rt::dateNow() / 1'000);
    rt::Task<std::string> hashing = uos::utils::sha256Base64Url(token);
    const std::string tokenSha = co_await std::move(hashing);
    json::object payload;
    payload["iss"] = "ubiquity-os-kernel";
    payload["aud"] = "ai.ubq.fi";
    payload["iat"] = rt::makeNumber(nowSeconds);
    payload["exp"] = rt::makeNumber(nowSeconds + 300);
    payload["jti"] = rt::crypto::randomUUID();
    payload["owner"] = "acme";
    payload["repo"] = "demo";
    payload["installation_id"] = nullptr;
    payload["auth_token_sha256"] = tokenSha;
    payload["state_id"] = "state-1";
    co_return rt::objectSpread(payload, overrides);
  }

  rt::Task<json::object> attestationPayload(std::string token) {
    rt::Task<json::object> building = attestationPayload(std::move(token), json::object());
    json::object payload = co_await std::move(building);
    co_return payload;
  }

  // `null` kernel token: no header.
  rt::Request tokenRequest(std::optional<std::string> kernelToken, const Headers& headers = {}) {
    rt::Headers init;
    if (kernelToken) init.append("X-Ubiquity-Kernel-Token", *kernelToken);
    for (const auto& [name, value] : headers) init.append(name, value);
    rt::RequestInit requestInit;
    requestInit.method = "POST";
    requestInit.headers = init;
    return rt::Request("https://ai.ubq.fi/v1/responses", requestInit);
  }

  json::array keyRows(const std::string& pem) {
    return json::array{json::object{{"app_id", 1}, {"pem", pem}, {"owner", "acme"}, {"added_at_ms", 1}}};
  }

  bool okOf(const KernelAttestationResult& result) { return std::holds_alternative<KernelAttestationSuccess>(result); }

  const rt::Response& responseOf(const KernelAttestationResult& result) { return std::get<KernelAttestationFailure>(result).response; }

  const KernelAttestationPayload& payloadOf(const KernelAttestationResult& result) { return std::get<KernelAttestationSuccess>(result).payload; }

  std::string errorField(const json::value& body, std::string_view field) {
    const json::value* error = rt::get(body, "error");
    if (error == nullptr) return "<no error object>";
    return std::string(rt::getString(*error, field).value_or("<missing>"));
  }

  bool matches(const std::string& text, const char* pattern) { return rt::Regex(pattern, "").test(text); }

  struct FailureView {
    double status;
    std::string code;
    std::string message;
  };

  rt::Task<FailureView> readerOf(KernelAttestationResult result) {
    EXPECT_EQ(okOf(result), false);
    if (okOf(result)) co_return FailureView{.status = 0, .code = "", .message = ""};
    const json::value body = co_await responseOf(result).json();
    co_return FailureView{.status = static_cast<double>(responseOf(result).status()), .code = errorField(body, "code"), .message = errorField(body, "message")};
  }

  VerifyKernelAttestationOptions bound(std::string token) {
    VerifyKernelAttestationOptions options;
    options.token = std::move(token);
    return options;
  }

  json::object with(json::object base, const json::object& overrides) { return rt::objectSpread(base, overrides); }
}

ASYNC_TEST("kernel attestation requires the token header and a loaded public key") {
  const rt::env::ScopedOverride environment = withoutEnvironmentKey();
  KernelKv kv;
  kv.clearData();
  const AttestationKeys keys = createAttestationKeys();
  const std::string apiToken = "u_" + rt::repeat("a", 64);
  rt::Task<json::object> building = attestationPayload(apiToken);
  const json::object payload = co_await std::move(building);
  const std::string token = signAttestation(keys, payload);

  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();

  rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(tokenRequest(std::nullopt), bound(apiToken));
  const KernelAttestationResult missingHeader = co_await std::move(verifying);
  EXPECT_EQ(okOf(missingHeader), false);
  ASSERT(!okOf(missingHeader));
  EXPECT_EQ(responseOf(missingHeader).status(), 401);
  const json::value missingBody = co_await responseOf(missingHeader).json();
  EXPECT_EQ(errorField(missingBody, "code"), "missing_kernel_token");

  // A stored PEM that cannot be imported leaves the verifier with no keys at all.
  kv.clearData();
  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows("-----BEGIN PUBLIC KEY-----\nnot-a-key\n-----END PUBLIC KEY-----")));
  co_await reloadKernelPublicKeys();
  verifying = verifyKernelAttestation(tokenRequest(token), bound(apiToken));
  const KernelAttestationResult malformedKey = co_await std::move(verifying);
  EXPECT_EQ(okOf(malformedKey), false);
  ASSERT(!okOf(malformedKey));
  EXPECT_EQ(responseOf(malformedKey).status(), 500);
  const json::value body = co_await responseOf(malformedKey).json();
  EXPECT_EQ(errorField(body, "code"), "server_error");
  EXPECT(matches(errorField(body, "message"), "No kernel public keys loaded"));

  kv.clearData();
  co_await reloadKernelPublicKeys();
  verifying = verifyKernelAttestation(tokenRequest(token), bound(apiToken));
  const KernelAttestationResult noKeys = co_await std::move(verifying);
  EXPECT_EQ(okOf(noKeys), false);
  ASSERT(!okOf(noKeys));
  EXPECT_EQ(responseOf(noKeys).status(), 500);

  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("kernel attestation rejects malformed JWT parts and unsigned headers") {
  const rt::env::ScopedOverride environment = withoutEnvironmentKey();
  KernelKv kv;
  kv.clearData();
  const AttestationKeys keys = createAttestationKeys();
  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();
  const std::string apiToken = "u_" + rt::repeat("b", 64);
  rt::Task<json::object> building = attestationPayload(apiToken);
  const json::object payload = co_await std::move(building);

  rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(tokenRequest("header.payload"), bound(apiToken));
  KernelAttestationResult result = co_await std::move(verifying);
  rt::Task<FailureView> reading = readerOf(result);
  const FailureView twoParts = co_await std::move(reading);
  EXPECT_EQ(twoParts.status, 401);
  EXPECT_EQ(twoParts.code, "invalid_kernel_token");
  EXPECT(matches(twoParts.message, "MUST have 3 parts"));

  verifying = verifyKernelAttestation(tokenRequest("!!!.payload.signature"), bound(apiToken));
  result = co_await std::move(verifying);
  reading = readerOf(result);
  const FailureView undecodableHeader = co_await std::move(reading);
  EXPECT(matches(undecodableHeader.message, "failed to parse kernel attestation JWT parts"));

  verifying = verifyKernelAttestation(tokenRequest(base64Url(rt::encodeUtf8("not json")) + ".e30.sig"), bound(apiToken));
  result = co_await std::move(verifying);
  reading = readerOf(result);
  const FailureView nonJsonHeader = co_await std::move(reading);
  EXPECT(matches(nonJsonHeader.message, "failed to parse kernel attestation JWT parts"));

  const json::object hs256{{"alg", "HS256"}};
  verifying = verifyKernelAttestation(tokenRequest(signAttestation(keys, payload, hs256)), bound(apiToken));
  result = co_await std::move(verifying);
  reading = readerOf(result);
  const FailureView wrongAlgorithm = co_await std::move(reading);
  EXPECT(matches(wrongAlgorithm.message, "'alg' MUST be 'RS256', got 'HS256'"));

  // A JSON scalar header keeps the diagnostic shape the production code documents.
  verifying = verifyKernelAttestation(tokenRequest(base64Url(rt::encodeUtf8("7")) + "." + base64UrlJson(payload) + ".sig"), bound(apiToken));
  result = co_await std::move(verifying);
  reading = readerOf(result);
  const FailureView scalarHeader = co_await std::move(reading);
  EXPECT(matches(scalarHeader.message, "'alg' MUST be 'RS256', got 'undefined'"));

  const std::vector<json::value> invalidPayloadShapes{
    json::object{{"any", "record"}},
    with(payload, json::object{{"iat", "soon"}}),
    with(payload, json::object{{"exp", rt::makeNumber(rt::Number::NaN)}}),
    with(payload, json::object{{"installation_id", "7"}}),
    with(payload, json::object{{"iss", "someone-else"}}),
    with(payload, json::object{{"aud", "other.example"}}),
    with(payload, json::object{{"jti", ""}}),
    with(payload, json::object{{"owner", ""}}),
    with(payload, json::object{{"repo", ""}}),
    with(payload, json::object{{"auth_token_sha256", ""}}),
    with(payload, json::object{{"state_id", ""}}),
    json::value(7),
  };
  const json::object rs256{{"alg", "RS256"}};
  for (const json::value& shape : invalidPayloadShapes) {
    verifying = verifyKernelAttestation(tokenRequest(base64UrlJson(rs256) + "." + base64UrlJson(shape) + ".sig"), bound(apiToken));
    result = co_await std::move(verifying);
    reading = readerOf(result);
    const FailureView invalid = co_await std::move(reading);
    EXPECT_EQ(invalid.status, 401);
    EXPECT(matches(invalid.message, "payload is invalid or fields are missing"));
  }

  verifying = verifyKernelAttestation(tokenRequest(base64UrlJson(rs256) + "." + base64UrlJson(payload) + ".!!!"), bound(apiToken));
  result = co_await std::move(verifying);
  reading = readerOf(result);
  const FailureView badSignatureBase64 = co_await std::move(reading);
  EXPECT(matches(badSignatureBase64.message, "failed to decode kernel attestation signature"));
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("kernel attestation enforces clock, repo, installation and token-hash bindings") {
  const rt::env::ScopedOverride environment = withoutEnvironmentKey();
  KernelKv kv;
  kv.clearData();
  const AttestationKeys keys = createAttestationKeys();
  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();
  const std::string apiToken = "u_" + rt::repeat("c", 64);
  const double nowSeconds = rt::Math::floor(rt::dateNow() / 1'000);
  rt::Task<json::object> building = attestationPayload(apiToken);
  const json::object base = co_await std::move(building);

  // A missing or unparsable installation header is its own wire code; every
  // other rejection stays `invalid_kernel_token`.
  const auto failureMessage = [&](json::object overrides, Headers headers, std::string token, std::string expectedCode) -> rt::Task<std::string> {
    const std::string signedToken = signAttestation(keys, rt::objectSpread(base, overrides));
    VerifyKernelAttestationOptions options;
    options.token = token;
    for (const auto& [name, value] : headers) {
      if (name == "X-GitHub-Owner") options.owner = value;
      if (name == "X-GitHub-Repo") options.repo = value;
    }
    rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(tokenRequest(signedToken, headers), std::move(options));
    const KernelAttestationResult result = co_await std::move(verifying);
    EXPECT_EQ(okOf(result), false);
    if (okOf(result)) co_return std::string();
    EXPECT_EQ(responseOf(result).status(), 401);
    const json::value body = co_await responseOf(result).json();
    EXPECT_EQ(errorField(body, "code"), expectedCode);
    co_return errorField(body, "message");
  };
  const auto failure = [&](json::object overrides, Headers headers = {}, std::string token = "", std::string expectedCode = "invalid_kernel_token") {
    return rt::async([=, &failureMessage]() -> rt::Task<std::string> {
      rt::Task<std::string> running = failureMessage(overrides, headers, token.empty() ? apiToken : token, expectedCode);
      const std::string message = co_await std::move(running);
      co_return message;
    });
  };
  const auto number = [](double value) { return rt::makeNumber(value); };

  rt::Task<std::string> checking = failure(json::object{{"iat", number(nowSeconds)}, {"exp", number(nowSeconds - 1)}});
  std::string message = co_await std::move(checking);
  EXPECT(matches(message, "'exp' \\(\\d+\\) is before 'iat' \\(\\d+\\)"));
  checking = failure(json::object{{"iat", number(nowSeconds)}, {"exp", number(nowSeconds + 4'000)}});
  message = co_await std::move(checking);
  EXPECT(matches(message, "kernel attestation TTL is too long \\(4000s > 3600s\\)"));
  checking = failure(json::object{{"iat", number(nowSeconds + 3'600)}, {"exp", number(nowSeconds + 7'200)}});
  message = co_await std::move(checking);
  EXPECT(matches(message, "'iat' \\(\\d+\\) is in the future"));
  checking = failure(json::object{{"iat", number(nowSeconds - 7'200)}, {"exp", number(nowSeconds - 3'600)}});
  message = co_await std::move(checking);
  EXPECT(matches(message, "'exp' \\(\\d+\\) is in the past"));

  checking = failure(json::object(), Headers{{"X-GitHub-Owner", "other"}, {"X-GitHub-Repo", "demo"}});
  message = co_await std::move(checking);
  EXPECT(matches(message, "repo mismatch. Expected 'other\\/demo', got 'acme\\/demo'"));
  checking = failure(json::object(), Headers{{"X-GitHub-Owner", "acme"}, {"X-GitHub-Repo", "other"}}, apiToken);
  message = co_await std::move(checking);
  EXPECT(matches(message, "repo mismatch. Expected 'acme\\/other', got 'acme\\/demo'"));

  checking = failure(json::object{{"installation_id", 42}}, Headers{}, apiToken, "missing_installation_id");
  message = co_await std::move(checking);
  EXPECT(matches(message, "missing 'X-GitHub-Installation-Id' header"));
  checking = failure(json::object{{"installation_id", 42}}, Headers{{"X-GitHub-Installation-Id", "43"}});
  message = co_await std::move(checking);
  EXPECT(matches(message, "installation_id mismatch. Expected '43', got '42'"));
  checking = failure(json::object{{"installation_id", 42}}, Headers{{"X-GitHub-Installation-Id", "abc"}}, apiToken, "missing_installation_id");
  message = co_await std::move(checking);
  EXPECT(matches(message, "missing 'X-GitHub-Installation-Id' header"));
  checking = failure(json::object(), Headers{}, "u_" + rt::repeat("d", 64));
  message = co_await std::move(checking);
  EXPECT(matches(message, "'auth_token_sha256' mismatch"));

  rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(
    tokenRequest(signAttestation(keys, rt::objectSpread(base, json::object{{"installation_id", 42}})), Headers{{"X-GitHub-Installation-Id", " 42 "}}), bound(apiToken));
  const KernelAttestationResult installationMatch = co_await std::move(verifying);
  EXPECT_EQ(okOf(installationMatch), true);
  ASSERT(okOf(installationMatch));
  EXPECT_EQ(payloadOf(installationMatch).installation_id, std::optional<double>(42));
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("kernel attestation accepts a signed token, prunes expired jti entries and rejects tampering") {
  const rt::env::ScopedOverride environment = withoutEnvironmentKey();
  KernelKv kv;
  kv.clearData();
  const AttestationKeys keys = createAttestationKeys();
  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();
  const std::string apiToken = "u_" + rt::repeat("e", 64);
  const double nowSeconds = rt::Math::floor(rt::dateNow() / 1'000);

  // A token that expires immediately is still valid (clock skew) and leaves a
  // cache entry the next accepted token prunes.
  rt::Task<json::object> building = attestationPayload(apiToken, json::object{{"exp", rt::makeNumber(nowSeconds)}});
  const json::object expiringPayload = co_await std::move(building);
  const std::string expiring = signAttestation(keys, expiringPayload);
  rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(tokenRequest(expiring), bound(apiToken));
  const KernelAttestationResult accepted = co_await std::move(verifying);
  EXPECT_EQ(okOf(accepted), true);
  ASSERT(okOf(accepted));
  EXPECT_EQ(payloadOf(accepted).owner, "acme");
  EXPECT_EQ(payloadOf(accepted).repo, "demo");
  EXPECT_EQ(payloadOf(accepted).iss, "ubiquity-os-kernel");
  EXPECT_EQ(payloadOf(accepted).aud, "ai.ubq.fi");
  EXPECT_EQ(payloadOf(accepted).exp, nowSeconds);

  building = attestationPayload(apiToken, json::object{{"jti", "jti-prune-me"}});
  const json::object freshPayload = co_await std::move(building);
  const std::string fresh = signAttestation(keys, freshPayload);
  verifying = verifyKernelAttestation(tokenRequest(fresh), bound(apiToken));
  const KernelAttestationResult afterPrune = co_await std::move(verifying);
  EXPECT_EQ(okOf(afterPrune), true);

  const std::string tampered = rt::slice(fresh, 0, -4) + "AAAA";
  verifying = verifyKernelAttestation(tokenRequest(tampered), bound(apiToken));
  const KernelAttestationResult rejected = co_await std::move(verifying);
  EXPECT_EQ(okOf(rejected), false);
  ASSERT(!okOf(rejected));
  EXPECT_EQ(responseOf(rejected).status(), 401);
  const json::value body = co_await responseOf(rejected).json();
  EXPECT(matches(errorField(body, "message"), "invalid kernel attestation"));
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("kernel attestation context resolves repository identity from request headers") {
  const rt::env::ScopedOverride environment = withoutEnvironmentKey();
  KernelKv kv;
  kv.clearData();
  const AttestationKeys keys = createAttestationKeys();
  co_await kv.seed(KERNEL_PUBKEYS_KEY, rt::KvValue(keyRows(keys.pem)));
  co_await reloadKernelPublicKeys();
  const std::string apiToken = "u_" + rt::repeat("f", 64);
  rt::Task<json::object> building = attestationPayload(apiToken);
  const json::object payload = co_await std::move(building);
  const std::string token = signAttestation(keys, payload);

  rt::Task<std::optional<GitHubRepo>> resolving = getKernelAttestationContext(tokenRequest(token), std::nullopt);
  std::optional<GitHubRepo> context = co_await std::move(resolving);
  EXPECT_EQ(context.has_value(), false);
  resolving = getKernelAttestationContext(tokenRequest(std::nullopt), apiToken);
  context = co_await std::move(resolving);
  EXPECT_EQ(context.has_value(), false);
  // Only the owner header: the repo still resolves from the signed payload.
  resolving = getKernelAttestationContext(tokenRequest(token, Headers{{"X-GitHub-Owner", "acme"}}), apiToken);
  context = co_await std::move(resolving);
  EXPECT(context == std::optional<GitHubRepo>(GitHubRepo{.owner = "acme", .repo = "demo"}));

  resolving = getKernelAttestationContext(tokenRequest(token, Headers{{"X-GitHub-Owner", "acme"}, {"X-GitHub-Repo", "demo"}}), apiToken);
  const std::optional<GitHubRepo> resolved = co_await std::move(resolving);
  EXPECT(resolved == std::optional<GitHubRepo>(GitHubRepo{.owner = "acme", .repo = "demo"}));

  resolving = getKernelAttestationContext(tokenRequest(token, Headers{{"X-GitHub-Owner", "acme"}, {"X-GitHub-Repo", "other"}}), apiToken);
  const std::optional<GitHubRepo> mismatched = co_await std::move(resolving);
  EXPECT_EQ(mismatched.has_value(), false);

  EXPECT(getGitHubRepoHeaders(tokenRequest(token, Headers{{"X-GitHub-Owner", " acme "}, {"X-GitHub-Repo", " demo "}})) ==
         std::optional<GitHubRepo>(GitHubRepo{.owner = "acme", .repo = "demo"}));
  EXPECT_EQ(getGitHubRepoHeaders(tokenRequest(token)).has_value(), false);
  uos::kv::setKvForTest(nullptr);
}

namespace {
  namespace quota_v2 = uos::kernel::quota_v2;
  using uos::authority::native_kernel_contracts::KernelQuotaPolicyV2;
  using uos::tests::helpers::counting_kv::CountingKv;

  const rt::KvKey REPO_POLICY_PREFIX{"uos_ai", "kernel_quota", "v2", "repo_policy"};
  const rt::KvKey ORG_POLICY_PREFIX{"uos_ai", "kernel_quota", "v2", "org_policy"};

  // The file's shared CountingKv fixture of the kernel quota cases.
  const std::shared_ptr<CountingKv>& quotaKv() {
    static const std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();
    return kv;
  }

  // `{ ...base, ...overrides }`
  json::object withOverrides(json::object base, const json::object& overrides) {
    for (const auto& member : overrides) rt::objectDefine(base, member.key(), member.value());
    return base;
  }

  json::object quotaIdentity(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo) {
    json::object out;
    out["v"] = 2;
    out["scope"] = scope;
    out["owner"] = owner;
    if (repo) out["repo"] = *repo;
    return out;
  }

  json::object policyRecord(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = quotaIdentity(scope, owner, repo);
    row["usage_limit_requests"] = 10;
    row["window_ms"] = 60'000;
    row["expires_at_ms"] = -1;
    row["created_at_ms"] = 1'000;
    row["updated_at_ms"] = 2'000;
    return withOverrides(std::move(row), overrides);
  }

  json::object windowRecord(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = quotaIdentity(scope, owner, repo);
    row["usage_requests"] = 1;
    row["reserved_requests"] = 0;
    row["usage_reset_at_ms"] = rt::makeNumber(rt::dateNow() + 60'000);
    row["applied_window_ms"] = 60'000;
    row["created_at_ms"] = 1'000;
    row["updated_at_ms"] = 2'000;
    return withOverrides(std::move(row), overrides);
  }

  json::object overrides(std::initializer_list<std::pair<std::string_view, json::value>> entries) {
    json::object out;
    for (const auto& [key, value] : entries) rt::objectDefine(out, key, value);
    return out;
  }

  rt::KvValue stored(const json::object& value) { return rt::KvValue(json::value(value)); }
}

TEST("kernel quota normalizers reject malformed records and coerce loose values") {
  const json::value thirty("30");
  const json::value notANumber("not-a-number");
  const json::value sixtyThousand("60000");
  const json::value zero("0");
  const json::value later("later");
  const json::value* undefinedValue = nullptr;
  EXPECT_EQ(quota_v2::normalizeUsageLimit(thirty, 1), 30.0);
  EXPECT_EQ(quota_v2::normalizeUsageLimit(notANumber, 7), 7.0);
  EXPECT_EQ(quota_v2::normalizeUsageLimit(-1, 7), -1.0);
  EXPECT_EQ(quota_v2::normalizeUsageLimit(-5, 7), 7.0);
  EXPECT_EQ(quota_v2::normalizeUsageLimit(12.9, 7), 12.0);
  EXPECT_EQ(quota_v2::normalizeUsageLimit(undefinedValue, 7), 7.0);

  EXPECT_EQ(quota_v2::normalizeWindow(&sixtyThousand, 1), 60'000.0);
  EXPECT_EQ(quota_v2::normalizeWindow(&zero, 9), 9.0);
  EXPECT_EQ(quota_v2::normalizeWindow(0.0, 9), 9.0);
  EXPECT_EQ(quota_v2::normalizeWindow(rt::Number::NaN, 9), 9.0);
  EXPECT_EQ(quota_v2::normalizeWindow(1'500.7, 9), 1'500.0);

  EXPECT_EQ(quota_v2::normalizeExpiration(-1), -1.0);
  EXPECT_EQ(quota_v2::normalizeExpiration(later), -1.0);
  EXPECT_EQ(quota_v2::normalizeExpiration(1'700'000'000'123), 1'700'000'000'123.0);
  EXPECT_EQ(quota_v2::normalizeExpiration(-9), -1.0);
  EXPECT_EQ(quota_v2::normalizeExpiration(1'500.9), 1'500.0);

  const std::optional<std::string> demo = std::string("demo");
  const std::optional<std::string> other = std::string("other");
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(nullptr), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo)), "org", "acme", std::nullopt).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo)), "repo", "other", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo)), "repo", "acme", other).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo, overrides({{"created_at_ms", -1}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo, overrides({{"updated_at_ms", "now"}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo, overrides({{"window_ms", 0}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("repo", "acme", demo, overrides({{"usage_limit_requests", -9}}))), "repo", "acme", demo)
            .has_value());

  const std::optional<KernelQuotaPolicyV2> accepted = quota_v2::normalizeKernelQuotaPolicyV2(
    json::value(policyRecord("repo", "acme", demo, overrides({{"usage_limit_requests", "25"}, {"expires_at_ms", "soon"}}))), "repo", "acme", demo);
  const KernelQuotaPolicyV2 expected{.v = 2,
                                     .scope = "repo",
                                     .owner = "acme",
                                     .repo = "demo",
                                     .usage_limit_requests = 25,
                                     .window_ms = 60'000,
                                     .expires_at_ms = -1,
                                     .created_at_ms = 1'000,
                                     .updated_at_ms = 2'000};
  EXPECT(accepted == std::optional<KernelQuotaPolicyV2>(expected));
  const std::optional<KernelQuotaPolicyV2> unlimited =
    quota_v2::normalizeKernelQuotaPolicyV2(json::value(policyRecord("org", "acme", std::nullopt, overrides({{"usage_limit_requests", -1}}))), "org", "acme", std::nullopt);
  ASSERT(unlimited.has_value());
  EXPECT_EQ(unlimited->usage_limit_requests, -1.0);
  EXPECT_EQ(unlimited->repo.has_value(), false);

  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(7), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"usage_requests", -1}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"reserved_requests", 1.5}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"usage_reset_at_ms", 0}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"applied_window_ms", 0}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"created_at_ms", -1}}))), "repo", "acme", demo).has_value());
  EXPECT(!quota_v2::normalizeKernelQuotaWindowV2(json::value(windowRecord("repo", "acme", demo, overrides({{"updated_at_ms", nullptr}}))), "repo", "acme", demo).has_value());

  // `{ ...windowRecord("org", "acme", undefined), reserved_requests: undefined, repo: null }`: JSON
  // holds no undefined member, so `reserved_requests` is absent, which the normalizer reads alike.
  json::object legacy = withOverrides(windowRecord("org", "acme", std::nullopt), overrides({{"repo", nullptr}}));
  rt::objectDelete(legacy, "reserved_requests");
  const auto legacyWindow = quota_v2::normalizeKernelQuotaWindowV2(json::value(legacy), "org", "acme", std::nullopt);
  ASSERT(legacyWindow.has_value());
  EXPECT_EQ(legacyWindow->reserved_requests, 0.0);
  EXPECT_EQ(legacyWindow->repo.has_value(), false);

  EXPECT_THROWS_MESSAGE(quota_v2::kernelRepoPart(std::nullopt), rt::JsError, "Kernel quota repo scope requires a repo");
  EXPECT_EQ(quota_v2::kernelRepoPart(demo), std::string("demo"));
  EXPECT(quota_v2::kernelRepoReservationKey("acme", "demo", 5, "req-1") == (rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_reservation", "acme", "demo", 5, "req-1"}));
  EXPECT(quota_v2::kernelOrgReservationKey("acme", 5, "req-1") == (rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_reservation", "acme", 5, "req-1"}));
}

ASYNC_TEST("kernel quota snapshots and policy listings fail closed without KV") {
  uos::kv::setKvForTest(nullptr);
  auto repoSnapshotting = quota_v2::getKernelUsageLimitSnapshot("acme", "demo");
  const std::optional<quota_v2::KernelAuthLimitSnapshot> repoSnapshot = co_await std::move(repoSnapshotting);
  EXPECT(!repoSnapshot.has_value());
  auto orgSnapshotting = quota_v2::getKernelOrgUsageLimitSnapshot("acme");
  const std::optional<quota_v2::KernelOrgLimitSnapshot> orgSnapshot = co_await std::move(orgSnapshotting);
  EXPECT(!orgSnapshot.has_value());
  auto repoListing = quota_v2::listKernelUsageLimits();
  const auto repoRows = co_await std::move(repoListing);
  EXPECT(!repoRows.has_value());
  auto orgListing = quota_v2::listKernelOrgUsageLimits();
  const auto orgRows = co_await std::move(orgListing);
  EXPECT(!orgRows.has_value());
}

ASYNC_TEST("kernel quota listings project stored policies and skip foreign rows") {
  const std::shared_ptr<CountingKv>& kv = quotaKv();
  uos::kv::setKvForTest(kv);
  kv->clearData();
  const std::optional<std::string> demo = std::string("demo");
  kv->seed(rt::KvKey{REPO_POLICY_PREFIX, "acme", "demo"}, stored(policyRecord("repo", "acme", demo, overrides({{"usage_limit_requests", 4}}))));
  kv->seed(rt::KvKey{REPO_POLICY_PREFIX, "acme", "demo", "extra"}, stored(policyRecord("repo", "acme", demo)));
  kv->seed(rt::KvKey{REPO_POLICY_PREFIX, "acme"}, stored(policyRecord("repo", "acme", std::nullopt)));
  kv->seed(REPO_POLICY_PREFIX, stored(policyRecord("repo", "acme", demo)));
  kv->seed(rt::KvKey{REPO_POLICY_PREFIX, "acme", "stale"}, stored(policyRecord("repo", "acme", std::string("stale"), overrides({{"window_ms", 0}}))));
  kv->seed(rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_window", "acme", "demo"}, stored(windowRecord("repo", "acme", demo, overrides({{"usage_requests", 3}}))));
  kv->seed(rt::KvKey{ORG_POLICY_PREFIX, "acme"}, stored(policyRecord("org", "acme", std::nullopt)));
  kv->seed(rt::KvKey{ORG_POLICY_PREFIX, "beta"}, stored(policyRecord("org", "owner-mismatch", std::nullopt)));
  kv->seed(rt::KvKey{ORG_POLICY_PREFIX, ""}, stored(policyRecord("org", "acme", std::nullopt)));

  auto repoListing = quota_v2::listKernelUsageLimits();
  const auto repoRows = co_await std::move(repoListing);
  // The malformed `acme/demo/extra` key is projected onto its first two key
  // segments rather than skipped: kernelPolicyRow (src/kernel/quota-v2.ts) reads
  // only prefix+0 and prefix+1, so that key surfaces as a second acme/demo row.
  ASSERT(repoRows.has_value());
  std::vector<std::string> identities;
  for (const auto& row : *repoRows) identities.push_back(row.owner + "/" + row.repo);
  EXPECT(identities == (std::vector<std::string>{"acme/demo", "acme/demo"}));
  ASSERT(!repoRows->empty());
  EXPECT_EQ((*repoRows)[0].usage_requests, 3.0);
  // The duplicate row carries the same owner/repo pair with its own limit.
  std::vector<double> limits;
  for (const auto& row : *repoRows) limits.push_back(row.usage_limit_requests);
  rt::sort(limits, [](double left, double right) { return left - right; });
  EXPECT(limits == (std::vector<double>{4, 10}));

  auto repoSnapshotting = quota_v2::getKernelUsageLimitSnapshot("acme", "demo");
  const std::optional<quota_v2::KernelAuthLimitSnapshot> repoSnapshot = co_await std::move(repoSnapshotting);
  ASSERT(repoSnapshot.has_value());
  EXPECT_EQ(repoSnapshot->source, std::string("kv"));
  EXPECT_EQ(repoSnapshot->record.usage_limit_requests, 4.0);
  EXPECT_EQ(repoSnapshot->record.usage_requests, 3.0);

  auto orgSnapshotting = quota_v2::getKernelOrgUsageLimitSnapshot("acme");
  const std::optional<quota_v2::KernelOrgLimitSnapshot> orgSnapshot = co_await std::move(orgSnapshotting);
  ASSERT(orgSnapshot.has_value());
  EXPECT_EQ(orgSnapshot->source, std::string("kv"));
  EXPECT_EQ(orgSnapshot->record.owner, std::string("acme"));
  EXPECT_EQ(orgSnapshot->record.window_ms, 60'000.0);

  auto orgListing = quota_v2::listKernelOrgUsageLimits();
  const auto orgRows = co_await std::move(orgListing);
  ASSERT(orgRows.has_value());
  std::vector<std::string> owners;
  for (const auto& row : *orgRows) owners.push_back(row.owner);
  EXPECT(owners == (std::vector<std::string>{"acme"}));

  auto defaultSnapshotting = quota_v2::getKernelUsageLimitSnapshot("nobody", "nothing");
  const std::optional<quota_v2::KernelAuthLimitSnapshot> defaulted = co_await std::move(defaultSnapshotting);
  ASSERT(defaulted.has_value());
  EXPECT_EQ(defaulted->source, std::string("default"));
  EXPECT_EQ(defaulted->record.usage_limit_requests, -1.0);
  uos::kv::setKvForTest(nullptr);
}
