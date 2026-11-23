#include "uos/rt/crypto.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/param_build.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace uos::rt::crypto {
  namespace {
    // ---- errors (Bun's DOMException names and texts)

    constexpr const char* OPERATION_FAILED = "The operation failed for an operation-specific reason";
    constexpr const char* DATA_INVALID = "Data provided to an operation does not meet requirements";
    constexpr const char* ALGORITHM_NOT_SUPPORTED = "The algorithm is not supported";
    constexpr const char* UNRECOGNIZED_ALGORITHM = "Unrecognized algorithm name";
    constexpr const char* INVALID_KEY_DATA = "Invalid keyData";

    [[noreturn]] void domError(const char* name, std::string message) { throw DOMException(std::move(message), name); }
    [[noreturn]] void operationError(std::string message = OPERATION_FAILED) { domError("OperationError", std::move(message)); }
    [[noreturn]] void dataError(std::string message = DATA_INVALID) { domError("DataError", std::move(message)); }
    [[noreturn]] void notSupported(std::string message) { domError("NotSupportedError", std::move(message)); }
    [[noreturn]] void invalidAccess(std::string message) { domError("InvalidAccessError", std::move(message)); }
    [[noreturn]] void missingMember(const char* dictionary, const char* member, const char* type) {
      throw TypeError(std::string("Member ") + dictionary + "." + member + " is required and must be an instance of " + type);
    }

    struct OpenSslFailure : std::runtime_error {
      using std::runtime_error::runtime_error;
    };

    // ---- random source

    void openSslRandom(std::span<std::uint8_t> bytes) {
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const std::size_t chunk = std::min<std::size_t>(bytes.size() - offset, static_cast<std::size_t>(std::numeric_limits<int>::max()));
        if (RAND_bytes(bytes.data() + offset, static_cast<int>(chunk)) != 1) throw OpenSslFailure("uos::rt::crypto: RAND_bytes failed");
        offset += chunk;
      }
    }

    RandomSource& currentSource() {
      thread_local RandomSource source;
      return source;
    }

    void fillRandom(std::span<std::uint8_t> bytes) {
      if (bytes.empty()) return;
      if (const auto& source = currentSource()) source(bytes);
      else openSslRandom(bytes);
    }

    // ---- small codecs

    // JWK members allow the URL alphabet and optional trailing padding.
    std::optional<Bytes> fromBase64Url(std::string_view text) {
      while (!text.empty() && text.back() == '=') text.remove_suffix(1);
      if (text.size() % 4 == 1 || text.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") != std::string_view::npos)
        return std::nullopt;
      return rt::fromBase64(text);
    }

    std::string lowerAscii(std::string_view text) {
      std::string out(text);
      for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      return out;
    }

    bool equalsIgnoreCase(std::string_view left, std::string_view right) { return left.size() == right.size() && lowerAscii(left) == lowerAscii(right); }

    // ---- algorithm registry

    enum class Id { sha1, sha224, sha256, sha384, sha512, aesGcm, hmac, hkdf, pbkdf2, rsassaPkcs1 };

    struct Registered {
      const char* name;
      Id id;
    };
    constexpr Registered REGISTERED[] = {
      {"SHA-1", Id::sha1},     {"SHA-224", Id::sha224}, {"SHA-256", Id::sha256}, {"SHA-384", Id::sha384},
      {"SHA-512", Id::sha512}, {"AES-GCM", Id::aesGcm}, {"HMAC", Id::hmac},      {"HKDF", Id::hkdf},
      {"PBKDF2", Id::pbkdf2},  {"RSASSA-PKCS1-v1_5", Id::rsassaPkcs1},
    };
    // Names Bun's Web Crypto implements that this runtime does not.
    constexpr const char* UNIMPLEMENTED[] = {"AES-CTR", "AES-CBC", "AES-CFB-8", "AES-KW", "ECDSA", "ECDH", "Ed25519", "X25519", "RSA-PSS", "RSA-OAEP"};

    const char* canonicalName(Id id) {
      for (const auto& entry : REGISTERED)
        if (entry.id == id) return entry.name;
      return "";
    }

    std::optional<Id> lookup(std::string_view name) {
      for (const auto& entry : REGISTERED)
        if (equalsIgnoreCase(name, entry.name)) return entry.id;
      return std::nullopt;
    }

    bool unimplemented(std::string_view name) {
      for (const char* entry : UNIMPLEMENTED)
        if (equalsIgnoreCase(name, entry)) return true;
      return false;
    }

    [[noreturn]] void notImplemented(std::string_view name) {
      throw std::logic_error("uos::rt::crypto: Web Crypto algorithm " + std::string(name) + " is not implemented by the runtime");
    }

    // Normalizes an algorithm name for an operation that must implement it.
    Id requireAlgorithm(std::string_view name) {
      if (auto id = lookup(name)) return *id;
      if (unimplemented(name)) notImplemented(name);
      notSupported(UNRECOGNIZED_ALGORITHM);
    }

    bool isHash(Id id) { return id == Id::sha1 || id == Id::sha224 || id == Id::sha256 || id == Id::sha384 || id == Id::sha512; }

    // A hash member or digest name.
    Id requireHashName(std::string_view name) {
      auto id = lookup(name);
      if (id && isHash(*id)) return *id;
      notSupported(UNRECOGNIZED_ALGORITHM);
    }

    Id requireHash(const std::optional<std::string>& hash, const char* dictionary) {
      if (!hash) missingMember(dictionary, "hash", "(object or DOMString)");
      return requireHashName(*hash);
    }

    const EVP_MD* digestOf(Id hash) {
      switch (hash) {
        case Id::sha1: return EVP_sha1();
        case Id::sha224: return EVP_sha224();
        case Id::sha256: return EVP_sha256();
        case Id::sha384: return EVP_sha384();
        case Id::sha512: return EVP_sha512();
        default: return nullptr;
      }
    }

    std::size_t digestLength(Id hash) { return static_cast<std::size_t>(EVP_MD_get_size(digestOf(hash))); }

    // ---- usages

    enum Usage : unsigned {
      encryptUsage = 1,
      decryptUsage = 2,
      signUsage = 4,
      verifyUsage = 8,
      deriveKeyUsage = 16,
      deriveBitsUsage = 32,
      wrapKeyUsage = 64,
      unwrapKeyUsage = 128,
    };
    constexpr std::pair<const char*, unsigned> USAGE_NAMES[] = {
      {"encrypt", encryptUsage},      {"decrypt", decryptUsage},       {"sign", signUsage},       {"verify", verifyUsage},
      {"deriveKey", deriveKeyUsage}, {"deriveBits", deriveBitsUsage}, {"wrapKey", wrapKeyUsage}, {"unwrapKey", unwrapKeyUsage},
    };

    unsigned parseUsages(const std::vector<std::string>& usages) {
      unsigned mask = 0;
      for (const auto& usage : usages) {
        bool found = false;
        for (const auto& [name, bit] : USAGE_NAMES)
          if (usage == name) {
            mask |= bit;
            found = true;
          }
        if (!found) throw TypeError("value must be enumeration (string)");
      }
      return mask;
    }

    std::vector<std::string> usageList(unsigned mask) {
      std::vector<std::string> out;
      for (const auto& [name, bit] : USAGE_NAMES)
        if (mask & bit) out.emplace_back(name);
      return out;
    }

    // ---- OpenSSL helpers

    struct PkeyFree {
      void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
    };
    struct BnFree {
      void operator()(BIGNUM* number) const noexcept { BN_free(number); }
    };
    struct CipherFree {
      void operator()(EVP_CIPHER_CTX* context) const noexcept { EVP_CIPHER_CTX_free(context); }
    };
    struct MdCtxFree {
      void operator()(EVP_MD_CTX* context) const noexcept { EVP_MD_CTX_free(context); }
    };
    struct PkeyCtxFree {
      void operator()(EVP_PKEY_CTX* context) const noexcept { EVP_PKEY_CTX_free(context); }
    };
    struct ParamBldFree {
      void operator()(OSSL_PARAM_BLD* builder) const noexcept { OSSL_PARAM_BLD_free(builder); }
    };
    struct ParamFree {
      void operator()(OSSL_PARAM* params) const noexcept { OSSL_PARAM_free(params); }
    };
    struct P8Free {
      void operator()(PKCS8_PRIV_KEY_INFO* info) const noexcept { PKCS8_PRIV_KEY_INFO_free(info); }
    };
    using Bignum = std::unique_ptr<BIGNUM, BnFree>;

    int checkedInt(std::size_t size) {
      if (size > static_cast<std::size_t>(std::numeric_limits<int>::max())) operationError();
      return static_cast<int>(size);
    }

    const std::uint8_t* dataOrDummy(BytesView bytes) {
      static const std::uint8_t dummy = 0;
      return bytes.empty() ? &dummy : bytes.data();
    }

    Bytes digestBytes(Id hash, BytesView data) {
      Bytes out(static_cast<std::size_t>(EVP_MAX_MD_SIZE));
      unsigned int length = 0;
      if (EVP_Digest(dataOrDummy(data), data.size(), out.data(), &length, digestOf(hash), nullptr) != 1) operationError();
      out.resize(length);
      return out;
    }

    Bytes hmacBytes(const EVP_MD* md, BytesView key, BytesView data) {
      Bytes out(static_cast<std::size_t>(EVP_MAX_MD_SIZE));
      unsigned int length = 0;
      if (!HMAC(md, dataOrDummy(key), checkedInt(key.size()), dataOrDummy(data), data.size(), out.data(), &length)) operationError();
      out.resize(length);
      return out;
    }

    // RFC 5869 with HMAC (an empty salt is HashLen zero bytes once HMAC pads it).
    Bytes hkdf(Id hash, BytesView key, BytesView salt, BytesView info, std::size_t length) {
      const EVP_MD* md = digestOf(hash);
      const std::size_t hashLength = digestLength(hash);
      if (length > 255 * hashLength) operationError();
      const Bytes prk = hmacBytes(md, salt, key);
      Bytes out;
      out.reserve(length);
      Bytes previous;
      for (std::uint8_t counter = 1; out.size() < length; ++counter) {
        Bytes input(previous);
        input.insert(input.end(), info.begin(), info.end());
        input.push_back(counter);
        previous = hmacBytes(md, prk, input);
        const std::size_t take = std::min(previous.size(), length - out.size());
        out.insert(out.end(), previous.begin(), previous.begin() + static_cast<std::ptrdiff_t>(take));
      }
      return out;
    }

    Bytes bignumBytes(const BIGNUM* number) {
      Bytes out(static_cast<std::size_t>(BN_num_bytes(number)));
      if (!out.empty()) BN_bn2bin(number, out.data());
      return out;
    }

    Bignum bignumOf(BytesView bytes) {
      Bignum number(BN_bin2bn(dataOrDummy(bytes), checkedInt(bytes.size()), nullptr));
      if (!number) throw OpenSslFailure("uos::rt::crypto: BN_bin2bn failed");
      return number;
    }

    Bytes rsaComponent(EVP_PKEY* key, const char* name) {
      BIGNUM* raw = nullptr;
      if (EVP_PKEY_get_bn_param(key, name, &raw) != 1) throw OpenSslFailure(std::string("uos::rt::crypto: missing RSA component ") + name);
      Bignum number(raw);
      return bignumBytes(number.get());
    }
  }

  // ---- CryptoKey

  struct CryptoKey::Impl {
    std::string type;
    bool extractable = false;
    KeyAlgorithm algorithm;
    std::vector<std::string> usages;
    unsigned usageMask = 0;
    Id id = Id::aesGcm;
    std::optional<Id> hash;
    Bytes secret;
    std::shared_ptr<EVP_PKEY> pkey;
  };

  CryptoKey::CryptoKey(std::shared_ptr<const Impl> state) : impl_(std::move(state)) {}
  const std::string& CryptoKey::type() const noexcept { return impl_->type; }
  bool CryptoKey::extractable() const noexcept { return impl_->extractable; }
  const KeyAlgorithm& CryptoKey::algorithm() const noexcept { return impl_->algorithm; }
  const std::vector<std::string>& CryptoKey::usages() const noexcept { return impl_->usages; }

  namespace {
    using KeyImpl = CryptoKey::Impl;

    CryptoKey makeKey(KeyImpl impl) { return CryptoKey(std::make_shared<const KeyImpl>(std::move(impl))); }

    KeyImpl secretKey(Id id, Bytes secret, bool extractable, unsigned usages) {
      KeyImpl impl;
      impl.type = "secret";
      impl.extractable = extractable;
      impl.id = id;
      impl.algorithm.name = canonicalName(id);
      impl.usageMask = usages;
      impl.usages = usageList(usages);
      impl.secret = std::move(secret);
      return impl;
    }

    KeyImpl rsaKey(std::shared_ptr<EVP_PKEY> pkey, bool isPrivate, Id hash, bool extractable, unsigned usages) {
      KeyImpl impl;
      impl.type = isPrivate ? "private" : "public";
      impl.extractable = extractable;
      impl.id = Id::rsassaPkcs1;
      impl.hash = hash;
      impl.algorithm.name = canonicalName(Id::rsassaPkcs1);
      impl.algorithm.hash = canonicalName(hash);
      impl.algorithm.modulusLength = static_cast<std::uint32_t>(rsaComponent(pkey.get(), OSSL_PKEY_PARAM_RSA_N).size() * 8);
      impl.algorithm.publicExponent = rsaComponent(pkey.get(), OSSL_PKEY_PARAM_RSA_E);
      impl.usageMask = usages;
      impl.usages = usageList(usages);
      impl.pkey = std::move(pkey);
      return impl;
    }

    void requireNonEmptyUsages(const KeyImpl& impl) {
      if (impl.usageMask != 0) return;
      if (impl.type == "secret") throw SyntaxError("Usages cannot be empty when importing a secret key.");
      if (impl.type == "private") throw SyntaxError("Usages cannot be empty when importing a private key.");
    }

    // ---- JWK members (the JsonWebKey dictionary after WebIDL conversion)

    // `String(value)` for a parsed JSON value.
    std::string jsToString(const json::value& value) {
      switch (value.kind()) {
        case json::kind::string: return std::string(value.get_string());
        case json::kind::bool_: return value.get_bool() ? "true" : "false";
        case json::kind::null: return "null";
        case json::kind::int64:
        case json::kind::uint64:
        case json::kind::double_: return numberToString(value.to_number<double>());
        case json::kind::array: {
          std::string out;
          bool first = true;
          for (const auto& element : value.get_array()) {
            if (!first) out += ',';
            first = false;
            if (!element.is_null()) out += jsToString(element);
          }
          return out;
        }
        case json::kind::object: return "[object Object]";
      }
      return "";
    }

    // `Boolean(value)` for a parsed JSON value.
    bool jsToBoolean(const json::value& value) {
      switch (value.kind()) {
        case json::kind::bool_: return value.get_bool();
        case json::kind::null: return false;
        case json::kind::string: return !value.get_string().empty();
        case json::kind::int64:
        case json::kind::uint64:
        case json::kind::double_: {
          const double number = value.to_number<double>();
          return number == number && number != 0;
        }
        default: return true;
      }
    }

    struct Jwk {
      std::optional<std::string> kty, use, alg, k, n, e, d, p, q, dp, dq, qi;
      std::optional<unsigned> keyOps;
      std::optional<bool> ext;
    };

    Jwk convertJwk(const json::object& object) {
      Jwk jwk;
      auto text = [&](std::string_view name) -> std::optional<std::string> {
        const auto* value = object.if_contains(name);
        if (!value) return std::nullopt;
        return jsToString(*value);
      };
      jwk.kty = text("kty");
      jwk.use = text("use");
      if (const auto* ops = object.if_contains("key_ops")) {
        if (!ops->is_array()) throw TypeError("Value is not a sequence");
        unsigned mask = 0;
        for (const auto& op : ops->get_array()) {
          const std::string name = jsToString(op);
          bool found = false;
          for (const auto& [usage, bit] : USAGE_NAMES)
            if (name == usage) {
              mask |= bit;
              found = true;
            }
          if (!found) throw TypeError("value must be enumeration (string)");
        }
        jwk.keyOps = mask;
      }
      jwk.alg = text("alg");
      if (const auto* ext = object.if_contains("ext")) jwk.ext = jsToBoolean(*ext);
      jwk.k = text("k");
      jwk.n = text("n");
      jwk.e = text("e");
      jwk.d = text("d");
      jwk.p = text("p");
      jwk.q = text("q");
      jwk.dp = text("dp");
      jwk.dq = text("dq");
      jwk.qi = text("qi");
      return jwk;
    }

    // `key_ops` present: the requested usages must all be listed.
    bool jwkKeyOpsAllow(const Jwk& jwk, unsigned usages) { return !jwk.keyOps || (*jwk.keyOps & usages) == usages; }

    bool jwkExtAllows(const Jwk& jwk, bool extractable) { return !(jwk.ext && !*jwk.ext && extractable); }

    // ---- AES-GCM

    constexpr unsigned AES_USAGES = encryptUsage | decryptUsage | wrapKeyUsage | unwrapKeyUsage;

    const char* aesJwkAlg(std::size_t bits) {
      switch (bits) {
        case 128: return "A128GCM";
        case 192: return "A192GCM";
        case 256: return "A256GCM";
        default: return nullptr;
      }
    }

    KeyImpl importAes(std::string_view format, const BytesView* raw, const Jwk* jwk, bool extractable, unsigned usages) {
      if (usages & ~AES_USAGES) throw SyntaxError("Unsupported key usage for an AES key");
      Bytes secret;
      if (format == "raw") {
        if (raw->size() != 16 && raw->size() != 24 && raw->size() != 32) dataError();
        secret.assign(raw->begin(), raw->end());
      } else if (format == "jwk") {
        if (jwk->kty != "oct" || !jwk->k) dataError();
        auto decoded = fromBase64Url(*jwk->k);
        if (!decoded) dataError();
        const char* expected = aesJwkAlg(decoded->size() * 8);
        if (!expected || (jwk->alg && *jwk->alg != expected)) dataError();
        if (usages && jwk->use && *jwk->use != "enc") dataError();
        if (!jwkKeyOpsAllow(*jwk, usages) || !jwkExtAllows(*jwk, extractable)) dataError();
        secret = std::move(*decoded);
      } else {
        notSupported(ALGORITHM_NOT_SUPPORTED);
      }
      KeyImpl impl = secretKey(Id::aesGcm, std::move(secret), extractable, usages);
      impl.algorithm.length = static_cast<std::uint32_t>(impl.secret.size() * 8);
      return impl;
    }

    const EVP_CIPHER* gcmCipher(std::size_t keyBytes) {
      switch (keyBytes) {
        case 16: return EVP_aes_128_gcm();
        case 24: return EVP_aes_192_gcm();
        default: return EVP_aes_256_gcm();
      }
    }

    std::size_t gcmTagBytes(const Algorithm& algorithm) {
      const std::uint32_t tagLength = algorithm.tagLength.value_or(128);
      switch (tagLength) {
        case 32:
        case 64:
        case 96:
        case 104:
        case 112:
        case 120:
        case 128: return tagLength / 8;
        default: operationError(std::to_string(tagLength) + " is not a valid AES-GCM tag length");
      }
    }

    Bytes aesGcm(bool encrypting, const Algorithm& algorithm, const KeyImpl& key, BytesView data) {
      const std::size_t tagBytes = gcmTagBytes(algorithm);
      const Bytes& iv = *algorithm.iv;
      if (iv.empty()) operationError();
      if (!encrypting && data.size() < tagBytes) operationError();
      std::unique_ptr<EVP_CIPHER_CTX, CipherFree> context(EVP_CIPHER_CTX_new());
      if (!context) operationError();
      const int enc = encrypting ? 1 : 0;
      if (EVP_CipherInit_ex(context.get(), gcmCipher(key.secret.size()), nullptr, nullptr, nullptr, enc) != 1) operationError();
      if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN, checkedInt(iv.size()), nullptr) != 1) operationError();
      if (EVP_CipherInit_ex(context.get(), nullptr, nullptr, key.secret.data(), iv.data(), enc) != 1) operationError();
      int written = 0;
      if (algorithm.additionalData && !algorithm.additionalData->empty())
        if (EVP_CipherUpdate(context.get(), nullptr, &written, algorithm.additionalData->data(), checkedInt(algorithm.additionalData->size())) != 1) operationError();
      const std::size_t bodyBytes = encrypting ? data.size() : data.size() - tagBytes;
      Bytes out(bodyBytes + (encrypting ? tagBytes : 0) + 16);
      std::size_t produced = 0;
      if (bodyBytes > 0) {
        if (EVP_CipherUpdate(context.get(), out.data(), &written, data.data(), checkedInt(bodyBytes)) != 1) operationError();
        produced = static_cast<std::size_t>(written);
      }
      if (!encrypting) {
        Bytes tag(data.begin() + static_cast<std::ptrdiff_t>(bodyBytes), data.end());
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG, checkedInt(tagBytes), tag.data()) != 1) operationError();
      }
      if (EVP_CipherFinal_ex(context.get(), out.data() + produced, &written) != 1) operationError();
      produced += static_cast<std::size_t>(written);
      if (encrypting) {
        if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG, checkedInt(tagBytes), out.data() + produced) != 1) operationError();
        produced += tagBytes;
      }
      out.resize(produced);
      return out;
    }

    // ---- HMAC

    constexpr unsigned HMAC_USAGES = signUsage | verifyUsage;

    const char* hmacJwkAlg(Id hash) {
      switch (hash) {
        case Id::sha1: return "HS1";
        case Id::sha224: return "HS224";
        case Id::sha256: return "HS256";
        case Id::sha384: return "HS384";
        default: return "HS512";
      }
    }

    // The default HMAC key length: the hash's block size.
    std::uint32_t hmacBlockBits(Id hash) { return hash == Id::sha384 || hash == Id::sha512 ? 1024 : 512; }

    KeyImpl importHmac(std::string_view format, const BytesView* raw, const Jwk* jwk, const Algorithm& algorithm, bool extractable, unsigned usages) {
      const Id hash = requireHash(algorithm.hash, "HmacKeyParams");
      if (usages & ~HMAC_USAGES) throw SyntaxError("Unsupported key usage for HMAC key");
      Bytes secret;
      if (format == "raw") {
        secret.assign(raw->begin(), raw->end());
      } else if (format == "jwk") {
        if (!jwk->kty) dataError(INVALID_KEY_DATA);
        if (*jwk->kty != "oct") dataError("Invalid JWK \"kty\" Parameter");
        if (!jwk->k) dataError(INVALID_KEY_DATA);
        if (usages && jwk->use && *jwk->use != "sig") dataError("Invalid JWK \"use\" Parameter");
        if (!jwkKeyOpsAllow(*jwk, usages)) dataError("Key operations and usage mismatch");
        if (!jwkExtAllows(*jwk, extractable)) dataError("JWK \"ext\" Parameter and extractable mismatch");
        if (jwk->alg && *jwk->alg != hmacJwkAlg(hash)) dataError("JWK \"alg\" does not match the requested algorithm");
        // A `k` that is not base64url imports as an empty key (and fails below).
        secret = fromBase64Url(*jwk->k).value_or(Bytes{});
      } else {
        notSupported(ALGORITHM_NOT_SUPPORTED);
      }
      if (secret.empty()) dataError("Zero-length key is not supported");
      const std::size_t bits = secret.size() * 8;
      if (algorithm.length) {
        const std::size_t length = *algorithm.length;
        if (length > bits || length + 8 <= bits) dataError("Invalid key length");
        if (length != bits) notSupported("Unsupported HmacImportParams.length");
      }
      KeyImpl impl = secretKey(Id::hmac, std::move(secret), extractable, usages);
      impl.hash = hash;
      impl.algorithm.hash = canonicalName(hash);
      impl.algorithm.length = static_cast<std::uint32_t>(bits);
      return impl;
    }

    // ---- HKDF / PBKDF2

    KeyImpl importDerivation(Id id, std::string_view format, const BytesView* raw, bool extractable, unsigned usages) {
      if (format != "raw") notSupported(ALGORITHM_NOT_SUPPORTED);
      const bool isHkdf = id == Id::hkdf;
      if (usages & ~(deriveKeyUsage | deriveBitsUsage)) throw SyntaxError(isHkdf ? "Unsupported key usage for a HKDF key" : "Unsupported key usage for a PBKDF2 key");
      if (extractable) throw SyntaxError(isHkdf ? "HKDF keys are not extractable" : "PBKDF2 keys are not extractable");
      return secretKey(id, Bytes(raw->begin(), raw->end()), false, usages);
    }

    // ---- RSASSA-PKCS1-v1_5

    const char* rsaJwkAlg(Id hash) {
      switch (hash) {
        case Id::sha1: return "RS1";
        case Id::sha224: return "RS224";
        case Id::sha256: return "RS256";
        case Id::sha384: return "RS384";
        default: return "RS512";
      }
    }

    std::shared_ptr<EVP_PKEY> rsaFromComponents(const std::vector<std::pair<const char*, Bytes>>& components, bool isPrivate) {
      std::unique_ptr<OSSL_PARAM_BLD, ParamBldFree> builder(OSSL_PARAM_BLD_new());
      if (!builder) throw OpenSslFailure("uos::rt::crypto: OSSL_PARAM_BLD_new failed");
      std::vector<Bignum> numbers;
      for (const auto& [name, bytes] : components) {
        numbers.push_back(bignumOf(bytes));
        if (OSSL_PARAM_BLD_push_BN(builder.get(), name, numbers.back().get()) != 1) throw OpenSslFailure("uos::rt::crypto: OSSL_PARAM_BLD_push_BN failed");
      }
      std::unique_ptr<OSSL_PARAM, ParamFree> params(OSSL_PARAM_BLD_to_param(builder.get()));
      std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
      if (!params || !context || EVP_PKEY_fromdata_init(context.get()) != 1) throw OpenSslFailure("uos::rt::crypto: EVP_PKEY_fromdata_init failed");
      EVP_PKEY* raw = nullptr;
      if (EVP_PKEY_fromdata(context.get(), &raw, isPrivate ? EVP_PKEY_KEYPAIR : EVP_PKEY_PUBLIC_KEY, params.get()) != 1 || !raw) dataError(INVALID_KEY_DATA);
      return std::shared_ptr<EVP_PKEY>(raw, PkeyFree());
    }

    std::shared_ptr<EVP_PKEY> rsaFromJwk(const Jwk& jwk, bool extractable, unsigned usages) {
      if (jwk.kty != "RSA") dataError(INVALID_KEY_DATA);
      if (!jwkKeyOpsAllow(jwk, usages) || !jwkExtAllows(jwk, extractable)) dataError(INVALID_KEY_DATA);
      auto decode = [](const std::optional<std::string>& member) {
        if (!member) dataError(INVALID_KEY_DATA);
        auto bytes = fromBase64Url(*member);
        if (!bytes) dataError(INVALID_KEY_DATA);
        return std::move(*bytes);
      };
      Bytes modulus = decode(jwk.n);
      // RFC 7518 section 6.3.1.1: one leading zero octet is tolerated.
      if (!modulus.empty() && modulus[0] == 0) modulus.erase(modulus.begin());
      Bytes exponent = decode(jwk.e);
      if (modulus.empty() || exponent.empty()) dataError(INVALID_KEY_DATA);
      if (!jwk.d) return rsaFromComponents({{OSSL_PKEY_PARAM_RSA_N, modulus}, {OSSL_PKEY_PARAM_RSA_E, exponent}}, false);
      // A private key needs every CRT member.
      Bytes privateExponent = decode(jwk.d);
      Bytes firstPrime = decode(jwk.p);
      Bytes firstExponent = decode(jwk.dp);
      Bytes secondPrime = decode(jwk.q);
      Bytes secondExponent = decode(jwk.dq);
      Bytes coefficient = decode(jwk.qi);
      if (privateExponent.empty() || firstPrime.empty() || secondPrime.empty()) dataError(INVALID_KEY_DATA);
      return rsaFromComponents({{OSSL_PKEY_PARAM_RSA_N, modulus},
                                {OSSL_PKEY_PARAM_RSA_E, exponent},
                                {OSSL_PKEY_PARAM_RSA_D, privateExponent},
                                {OSSL_PKEY_PARAM_RSA_FACTOR1, firstPrime},
                                {OSSL_PKEY_PARAM_RSA_FACTOR2, secondPrime},
                                {OSSL_PKEY_PARAM_RSA_EXPONENT1, firstExponent},
                                {OSSL_PKEY_PARAM_RSA_EXPONENT2, secondExponent},
                                {OSSL_PKEY_PARAM_RSA_COEFFICIENT1, coefficient}},
                               true);
    }

    std::shared_ptr<EVP_PKEY> rsaFromSpki(BytesView der) {
      if (der.size() > limits::kCertificateBytes) dataError(INVALID_KEY_DATA);
      const unsigned char* cursor = der.data();
      EVP_PKEY* raw = der.empty() ? nullptr : d2i_PUBKEY(nullptr, &cursor, static_cast<long>(der.size()));
      if (!raw) dataError(INVALID_KEY_DATA);
      std::shared_ptr<EVP_PKEY> key(raw, PkeyFree());
      if (EVP_PKEY_get_base_id(raw) != EVP_PKEY_RSA) dataError("Invalid key type");
      return key;
    }

    std::shared_ptr<EVP_PKEY> rsaFromPkcs8(BytesView der) {
      if (der.size() > limits::kCertificateBytes) dataError(INVALID_KEY_DATA);
      const unsigned char* cursor = der.data();
      std::unique_ptr<PKCS8_PRIV_KEY_INFO, P8Free> info(der.empty() ? nullptr : d2i_PKCS8_PRIV_KEY_INFO(nullptr, &cursor, static_cast<long>(der.size())));
      if (!info) dataError(INVALID_KEY_DATA);
      EVP_PKEY* raw = EVP_PKCS82PKEY(info.get());
      if (!raw) dataError(INVALID_KEY_DATA);
      std::shared_ptr<EVP_PKEY> key(raw, PkeyFree());
      if (EVP_PKEY_get_base_id(raw) != EVP_PKEY_RSA) dataError("Invalid key type");
      return key;
    }

    KeyImpl importRsa(std::string_view format, const BytesView* raw, const Jwk* jwk, const Algorithm& algorithm, bool extractable, unsigned usages) {
      const Id hash = requireHash(algorithm.hash, "RsaHashedImportParams");
      const char* unsupportedUsage = "Unsupported key usage for an RSASSA-PKCS1-v1_5 key";
      std::shared_ptr<EVP_PKEY> key;
      bool isPrivate;
      if (format == "spki") {
        if (usages & ~verifyUsage) throw SyntaxError(unsupportedUsage);
        key = rsaFromSpki(*raw);
        isPrivate = false;
      } else if (format == "pkcs8") {
        if (usages & ~signUsage) throw SyntaxError(unsupportedUsage);
        key = rsaFromPkcs8(*raw);
        isPrivate = true;
      } else if (format == "jwk") {
        isPrivate = jwk->d.has_value();
        if (usages & ~(isPrivate ? signUsage : verifyUsage)) throw SyntaxError(unsupportedUsage);
        if (usages && jwk->use && *jwk->use != "sig") dataError("Invalid JWK \"use\" Parameter");
        if (jwk->alg && *jwk->alg != rsaJwkAlg(hash)) dataError("JWK \"alg\" does not match the requested algorithm");
        key = rsaFromJwk(*jwk, extractable, usages);
      } else {
        notSupported(ALGORITHM_NOT_SUPPORTED);
      }
      return rsaKey(std::move(key), isPrivate, hash, extractable, usages);
    }

    // ---- import dispatch

    CryptoKey importAny(std::string_view format, const BytesView* raw, const json::object* jwkObject, const Algorithm& algorithm, bool extractable,
                        const std::vector<std::string>& keyUsages) {
      if (format != "raw" && format != "spki" && format != "pkcs8" && format != "jwk")
        throw TypeError("Failed to execute 'importKey' on 'SubtleCrypto': 1st argument '" + std::string(format) + "' is not a valid enum value of type KeyFormat.");
      if (format == "jwk" && !jwkObject) throw TypeError("Type error");
      if (format != "jwk" && !raw) throw TypeError("Failed to execute 'importKey' on 'SubtleCrypto': 2nd argument is not instance of ArrayBuffer, Buffer, TypedArray, or DataView.");
      std::optional<Jwk> converted;
      if (jwkObject) converted = convertJwk(*jwkObject);
      const Jwk* jwk = converted ? &*converted : nullptr;
      const unsigned usages = parseUsages(keyUsages);
      const Id id = requireAlgorithm(algorithm.name);
      KeyImpl impl;
      switch (id) {
        case Id::aesGcm: impl = importAes(format, raw, jwk, extractable, usages); break;
        case Id::hmac: impl = importHmac(format, raw, jwk, algorithm, extractable, usages); break;
        case Id::hkdf:
        case Id::pbkdf2: impl = importDerivation(id, format, raw, extractable, usages); break;
        case Id::rsassaPkcs1: impl = importRsa(format, raw, jwk, algorithm, extractable, usages); break;
        default: notSupported(UNRECOGNIZED_ALGORITHM);
      }
      requireNonEmptyUsages(impl);
      return makeKey(std::move(impl));
    }

    Algorithm named(std::string_view name) { return Algorithm{.name = std::string(name)}; }

    // The algorithm of an operation on an existing key: a name the key cannot
    // have (every implemented and unimplemented Web Crypto name) is a mismatch.
    Id operationAlgorithm(const Algorithm& algorithm) {
      if (auto id = lookup(algorithm.name)) return *id;
      if (unimplemented(algorithm.name)) return Id::sha1;
      notSupported(UNRECOGNIZED_ALGORITHM);
    }

    void requireKeyAlgorithm(Id requested, const KeyImpl& key) {
      if (isHash(requested) || requested != key.id) invalidAccess("Key algorithm mismatch");
    }

    // Normalizes deriveBits parameters (TypeError for missing members) and checks
    // the base key, in Bun's order.
    Id deriveParams(const Algorithm& algorithm) {
      const Id id = operationAlgorithm(algorithm);
      if (id == Id::hkdf) {
        const Id hash = requireHash(algorithm.hash, "HkdfParams");
        if (!algorithm.salt) missingMember("HkdfParams", "salt", "(ArrayBufferView or ArrayBuffer)");
        if (!algorithm.info) missingMember("HkdfParams", "info", "(ArrayBufferView or ArrayBuffer)");
        return hash;
      }
      if (id == Id::pbkdf2) {
        const Id hash = requireHash(algorithm.hash, "Pbkdf2Params");
        if (!algorithm.salt) missingMember("Pbkdf2Params", "salt", "(ArrayBufferView or ArrayBuffer)");
        if (!algorithm.iterations) missingMember("Pbkdf2Params", "iterations", "unsigned long");
        return hash;
      }
      if (lookup(algorithm.name)) notSupported(UNRECOGNIZED_ALGORITHM);
      return Id::sha1;
    }

    Bytes deriveBitsChecked(const Algorithm& algorithm, Id hash, const KeyImpl& key, std::optional<std::uint32_t> length) {
      if (!length) operationError("length cannot be null");
      if (*length % 8 != 0) operationError("length must be a multiple of 8");
      const std::size_t bytes = *length / 8;
      if (key.id == Id::hkdf) {
        if (bytes == 0) return {};
        return hkdf(hash, key.secret, *algorithm.salt, *algorithm.info, bytes);
      }
      if (*algorithm.iterations == 0) operationError();
      if (bytes == 0) return {};
      Bytes out(bytes);
      if (PKCS5_PBKDF2_HMAC(reinterpret_cast<const char*>(dataOrDummy(key.secret)), checkedInt(key.secret.size()), dataOrDummy(*algorithm.salt),
                            checkedInt(algorithm.salt->size()), checkedInt(*algorithm.iterations), digestOf(hash), checkedInt(bytes), out.data()) != 1)
        operationError();
      return out;
    }
  }

  // ---- randomness

  ScopedRandomSource::ScopedRandomSource(RandomSource source) : previous_(std::exchange(currentSource(), std::move(source))) {}
  ScopedRandomSource::~ScopedRandomSource() { currentSource() = std::move(previous_); }

  std::string randomUUID() {
    std::array<std::uint8_t, 16> bytes{};
    fillRandom(bytes);
    bytes[6] = static_cast<std::uint8_t>((bytes[6] & 0x0f) | 0x40);
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    std::string out = rt::toHex(bytes);
    out.reserve(36);
    for (const std::size_t position : {20u, 16u, 12u, 8u}) out.insert(position, 1, '-');
    return out;
  }

  void getRandomValues(std::span<std::uint8_t> array) { fillRandom(array); }

  void getRandomValues(std::span<std::uint32_t> array) {
    Bytes bytes(array.size() * 4);
    fillRandom(bytes);
    for (std::size_t index = 0; index < array.size(); ++index)
      array[index] = std::uint32_t(bytes[index * 4]) | (std::uint32_t(bytes[index * 4 + 1]) << 8) | (std::uint32_t(bytes[index * 4 + 2]) << 16) |
                     (std::uint32_t(bytes[index * 4 + 3]) << 24);
  }

  Bytes randomBytes(double size) {
    if (!(size >= 0 && size <= 2147483647.0))
      throw RangeError("The value of \"size\" is out of range. It must be >= 0 && <= 2147483647. Received " + numberToString(size));
    Bytes out(static_cast<std::size_t>(size));
    fillRandom(out);
    return out;
  }

  // ---- subtle

  namespace subtle {
    Bytes digest(const Algorithm& algorithm, BytesView data) { return digestBytes(requireHashName(algorithm.name), data); }
    Bytes digest(std::string_view algorithm, BytesView data) { return digest(named(algorithm), data); }

    CryptoKey importKey(std::string_view format, BytesView keyData, const Algorithm& algorithm, bool extractable, const std::vector<std::string>& keyUsages) {
      return importAny(format, &keyData, nullptr, algorithm, extractable, keyUsages);
    }
    CryptoKey importKey(std::string_view format, BytesView keyData, std::string_view algorithm, bool extractable, const std::vector<std::string>& keyUsages) {
      return importKey(format, keyData, named(algorithm), extractable, keyUsages);
    }
    CryptoKey importKey(std::string_view format, const json::object& keyData, const Algorithm& algorithm, bool extractable,
                        const std::vector<std::string>& keyUsages) {
      return importAny(format, nullptr, &keyData, algorithm, extractable, keyUsages);
    }
    CryptoKey importKey(std::string_view format, const json::object& keyData, std::string_view algorithm, bool extractable,
                        const std::vector<std::string>& keyUsages) {
      return importKey(format, keyData, named(algorithm), extractable, keyUsages);
    }

    Bytes exportKey(std::string_view format, const CryptoKey& key) {
      const KeyImpl& impl = key.impl();
      if (!impl.extractable) invalidAccess("key is not extractable");
      if (format == "jwk") throw std::invalid_argument("uos::rt::crypto::subtle::exportKey: use exportJwk for the \"jwk\" format");
      if (format != "raw" && format != "spki" && format != "pkcs8")
        throw TypeError("Failed to execute 'exportKey' on 'SubtleCrypto': 1st argument '" + std::string(format) + "' is not a valid enum value of type KeyFormat.");
      if (impl.id == Id::rsassaPkcs1) {
        if (format == "raw") notSupported(ALGORITHM_NOT_SUPPORTED);
        const bool isPrivate = impl.type == "private";
        if ((format == "spki") == isPrivate) invalidAccess("The requested operation is not valid for the provided key");
        Bytes out;
        if (!isPrivate) {
          const int size = i2d_PUBKEY(impl.pkey.get(), nullptr);
          if (size <= 0) operationError();
          out.resize(static_cast<std::size_t>(size));
          unsigned char* cursor = out.data();
          i2d_PUBKEY(impl.pkey.get(), &cursor);
        } else {
          std::unique_ptr<PKCS8_PRIV_KEY_INFO, P8Free> info(EVP_PKEY2PKCS8(impl.pkey.get()));
          if (!info) operationError();
          const int size = i2d_PKCS8_PRIV_KEY_INFO(info.get(), nullptr);
          if (size <= 0) operationError();
          out.resize(static_cast<std::size_t>(size));
          unsigned char* cursor = out.data();
          i2d_PKCS8_PRIV_KEY_INFO(info.get(), &cursor);
        }
        return out;
      }
      if (format != "raw") notSupported(ALGORITHM_NOT_SUPPORTED);
      return impl.secret;
    }

    json::object exportJwk(const CryptoKey& key) {
      const KeyImpl& impl = key.impl();
      if (!impl.extractable) invalidAccess("key is not extractable");
      json::array keyOps;
      for (const auto& usage : impl.usages) keyOps.emplace_back(usage);
      json::object out;
      if (impl.id == Id::rsassaPkcs1) {
        EVP_PKEY* pkey = impl.pkey.get();
        const bool isPrivate = impl.type == "private";
        out["alg"] = rsaJwkAlg(*impl.hash);
        if (isPrivate) {
          out["d"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_D));
          out["dp"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_EXPONENT1));
          out["dq"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_EXPONENT2));
        }
        out["e"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_E));
        out["ext"] = true;
        out["key_ops"] = std::move(keyOps);
        out["kty"] = "RSA";
        out["n"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_N));
        if (isPrivate) {
          out["p"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_FACTOR1));
          out["q"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_FACTOR2));
          out["qi"] = rt::toBase64Url(rsaComponent(pkey, OSSL_PKEY_PARAM_RSA_COEFFICIENT1));
        }
        return out;
      }
      if (impl.id == Id::aesGcm) out["alg"] = aesJwkAlg(impl.secret.size() * 8);
      else if (impl.id == Id::hmac) out["alg"] = hmacJwkAlg(*impl.hash);
      else notSupported(ALGORITHM_NOT_SUPPORTED);
      out["ext"] = true;
      out["k"] = rt::toBase64Url(impl.secret);
      out["key_ops"] = std::move(keyOps);
      out["kty"] = "oct";
      return out;
    }

    Bytes encrypt(const Algorithm& algorithm, const CryptoKey& key, BytesView data) {
      const Id id = operationAlgorithm(algorithm);
      if (id == Id::aesGcm && !algorithm.iv) missingMember("AesGcmParams", "iv", "(ArrayBufferView or ArrayBuffer)");
      requireKeyAlgorithm(id, key.impl());
      if (!(key.impl().usageMask & encryptUsage)) invalidAccess("Unable to use this key to encrypt");
      return aesGcm(true, algorithm, key.impl(), data);
    }

    Bytes decrypt(const Algorithm& algorithm, const CryptoKey& key, BytesView data) {
      const Id id = operationAlgorithm(algorithm);
      if (id == Id::aesGcm && !algorithm.iv) missingMember("AesGcmParams", "iv", "(ArrayBufferView or ArrayBuffer)");
      requireKeyAlgorithm(id, key.impl());
      if (!(key.impl().usageMask & decryptUsage)) invalidAccess("Unable to use this key to decrypt");
      return aesGcm(false, algorithm, key.impl(), data);
    }

    Bytes sign(const Algorithm& algorithm, const CryptoKey& key, BytesView data) {
      const KeyImpl& impl = key.impl();
      requireKeyAlgorithm(operationAlgorithm(algorithm), impl);
      if (!(impl.usageMask & signUsage)) invalidAccess("Unable to use this key to sign");
      if (impl.id == Id::hmac) return hmacBytes(digestOf(*impl.hash), impl.secret, data);
      if (impl.id != Id::rsassaPkcs1) invalidAccess("Key algorithm mismatch");
      std::unique_ptr<EVP_MD_CTX, MdCtxFree> context(EVP_MD_CTX_new());
      if (!context || EVP_DigestSignInit(context.get(), nullptr, digestOf(*impl.hash), nullptr, impl.pkey.get()) != 1) operationError();
      std::size_t size = 0;
      if (EVP_DigestSign(context.get(), nullptr, &size, dataOrDummy(data), data.size()) != 1) operationError();
      Bytes out(size);
      if (EVP_DigestSign(context.get(), out.data(), &size, dataOrDummy(data), data.size()) != 1) operationError();
      out.resize(size);
      return out;
    }
    Bytes sign(std::string_view algorithm, const CryptoKey& key, BytesView data) { return sign(named(algorithm), key, data); }

    bool verify(const Algorithm& algorithm, const CryptoKey& key, BytesView signature, BytesView data) {
      const KeyImpl& impl = key.impl();
      requireKeyAlgorithm(operationAlgorithm(algorithm), impl);
      if (!(impl.usageMask & verifyUsage)) invalidAccess("Unable to use this key to verify");
      if (impl.id == Id::hmac) {
        const Bytes expected = hmacBytes(digestOf(*impl.hash), impl.secret, data);
        return expected.size() == signature.size() && CRYPTO_memcmp(expected.data(), signature.data(), expected.size()) == 0;
      }
      if (impl.id != Id::rsassaPkcs1) invalidAccess("Key algorithm mismatch");
      std::unique_ptr<EVP_MD_CTX, MdCtxFree> context(EVP_MD_CTX_new());
      if (!context || EVP_DigestVerifyInit(context.get(), nullptr, digestOf(*impl.hash), nullptr, impl.pkey.get()) != 1) return false;
      return EVP_DigestVerify(context.get(), dataOrDummy(signature), signature.size(), dataOrDummy(data), data.size()) == 1;
    }
    bool verify(std::string_view algorithm, const CryptoKey& key, BytesView signature, BytesView data) {
      return verify(named(algorithm), key, signature, data);
    }

    Bytes deriveBits(const Algorithm& algorithm, const CryptoKey& baseKey, std::optional<std::uint32_t> length) {
      const Id hash = deriveParams(algorithm);
      requireKeyAlgorithm(operationAlgorithm(algorithm), baseKey.impl());
      if (!(baseKey.impl().usageMask & deriveBitsUsage)) invalidAccess("baseKey does not have deriveBits usage");
      return deriveBitsChecked(algorithm, hash, baseKey.impl(), length);
    }

    CryptoKey deriveKey(const Algorithm& algorithm, const CryptoKey& baseKey, const Algorithm& derivedKeyType, bool extractable,
                        const std::vector<std::string>& keyUsages) {
      const Id hash = deriveParams(algorithm);
      const Id derived = requireAlgorithm(derivedKeyType.name);
      if (derived == Id::hmac) requireHash(derivedKeyType.hash, "HmacKeyParams");
      if (derived == Id::aesGcm && !derivedKeyType.length) missingMember("AesKeyParams", "length", "unsigned short");
      requireKeyAlgorithm(operationAlgorithm(algorithm), baseKey.impl());
      if (!(baseKey.impl().usageMask & deriveKeyUsage)) invalidAccess("baseKey does not have deriveKey usage");
      std::optional<std::uint32_t> length;
      if (derived == Id::aesGcm) {
        length = *derivedKeyType.length;
        if (*length != 128 && *length != 192 && *length != 256) operationError("Cannot get key length from derivedKeyType");
      } else if (derived == Id::hmac) {
        length = derivedKeyType.length ? *derivedKeyType.length : hmacBlockBits(*lookup(*derivedKeyType.hash));
        if (*length == 0) throw TypeError("Cannot get key length from derivedKeyType");
      } else if (derived != Id::hkdf && derived != Id::pbkdf2) {
        notSupported(ALGORITHM_NOT_SUPPORTED);
      }
      const Bytes bits = deriveBitsChecked(algorithm, hash, baseKey.impl(), length);
      Algorithm importAlgorithm = derivedKeyType;
      if (derived == Id::hmac) importAlgorithm.length = length;
      return importKey("raw", bits, importAlgorithm, extractable, keyUsages);
    }
    CryptoKey deriveKey(const Algorithm& algorithm, const CryptoKey& baseKey, std::string_view derivedKeyType, bool extractable,
                        const std::vector<std::string>& keyUsages) {
      return deriveKey(algorithm, baseKey, named(derivedKeyType), extractable, keyUsages);
    }

    CryptoKeyPair generateKey(const Algorithm& algorithm, bool extractable, const std::vector<std::string>& keyUsages) {
      const unsigned usages = parseUsages(keyUsages);
      const Id id = requireAlgorithm(algorithm.name);
      if (id != Id::rsassaPkcs1) throw std::logic_error("uos::rt::crypto: generateKey is implemented for RSASSA-PKCS1-v1_5 only");
      if (!algorithm.modulusLength) missingMember("RsaHashedKeyGenParams", "modulusLength", "unsigned long");
      if (!algorithm.publicExponent) missingMember("RsaHashedKeyGenParams", "publicExponent", "Uint8Array");
      const Id hash = requireHash(algorithm.hash, "RsaHashedKeyGenParams");
      if (usages & ~(signUsage | verifyUsage)) throw SyntaxError("Unsupported key usage for an RSASSA-PKCS1-v1_5 key");
      std::unique_ptr<EVP_PKEY_CTX, PkeyCtxFree> context(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
      if (!context || EVP_PKEY_keygen_init(context.get()) != 1) operationError();
      if (EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), checkedInt(*algorithm.modulusLength)) <= 0) operationError();
      Bignum exponent = bignumOf(*algorithm.publicExponent);
      if (EVP_PKEY_CTX_set1_rsa_keygen_pubexp(context.get(), exponent.get()) <= 0) operationError();
      EVP_PKEY* raw = nullptr;
      if (EVP_PKEY_generate(context.get(), &raw) != 1 || !raw) operationError();
      std::shared_ptr<EVP_PKEY> pair(raw, PkeyFree());
      if (!(usages & signUsage)) throw SyntaxError("Usages cannot be empty when creating a key.");
      // The public half is a separate public-only key, as in Bun.
      std::vector<std::pair<const char*, Bytes>> publicParts = {{OSSL_PKEY_PARAM_RSA_N, rsaComponent(raw, OSSL_PKEY_PARAM_RSA_N)},
                                                                {OSSL_PKEY_PARAM_RSA_E, rsaComponent(raw, OSSL_PKEY_PARAM_RSA_E)}};
      auto publicKey = makeKey(rsaKey(rsaFromComponents(publicParts, false), false, hash, true, usages & verifyUsage));
      auto privateKey = makeKey(rsaKey(std::move(pair), true, hash, extractable, usages & signUsage));
      return CryptoKeyPair{publicKey, privateKey};
    }
  }

  // ---- node:crypto

  struct Hash::Impl {
    std::unique_ptr<EVP_MD_CTX, MdCtxFree> digest;
    const EVP_MD* md = nullptr;
    Bytes hmacKey;
    Bytes hmacData;
    bool hmac = false;
    bool finalized = false;
  };

  Hash::Hash(std::shared_ptr<Impl> state) : impl_(std::move(state)) {}

  Hash& Hash::update(std::string_view data) { return update(bytesOf(data)); }

  Hash& Hash::update(BytesView data) {
    if (impl_->finalized) throw JsError("Digest already called");
    if (impl_->hmac) impl_->hmacData.insert(impl_->hmacData.end(), data.begin(), data.end());
    else if (EVP_DigestUpdate(impl_->digest.get(), dataOrDummy(data), data.size()) != 1) throw OpenSslFailure("uos::rt::crypto: EVP_DigestUpdate failed");
    return *this;
  }

  Bytes Hash::digest() {
    if (impl_->finalized) {
      if (impl_->hmac) return {};
      throw JsError("Digest already called");
    }
    impl_->finalized = true;
    if (impl_->hmac) {
      Bytes out = hmacBytes(impl_->md, impl_->hmacKey, impl_->hmacData);
      OPENSSL_cleanse(impl_->hmacData.data(), impl_->hmacData.size());
      return out;
    }
    Bytes out(static_cast<std::size_t>(EVP_MAX_MD_SIZE));
    unsigned int length = 0;
    if (EVP_DigestFinal_ex(impl_->digest.get(), out.data(), &length) != 1) throw OpenSslFailure("uos::rt::crypto: EVP_DigestFinal_ex failed");
    out.resize(length);
    return out;
  }

  std::string Hash::digest(std::string_view encoding) {
    const std::string name = lowerAscii(encoding);
    if (name != "hex" && name != "base64" && name != "base64url" && name != "latin1" && name != "binary")
      throw std::invalid_argument("uos::rt::crypto::Hash::digest: unsupported encoding " + std::string(encoding));
    const Bytes bytes = digest();
    if (name == "hex") return rt::toHex(bytes);
    if (name == "base64") return rt::toBase64(bytes);
    if (name == "base64url") return rt::toBase64Url(bytes);
    return rt::binaryString(bytes);
  }

  namespace {
    const EVP_MD* digestByName(std::string_view algorithm) {
      const std::string name(algorithm);
      if (const EVP_MD* md = EVP_get_digestbyname(name.c_str())) return md;
      return EVP_get_digestbyname(lowerAscii(name).c_str());
    }
  }

  Hash createHash(std::string_view algorithm) {
    const EVP_MD* md = digestByName(algorithm);
    auto impl = std::make_shared<Hash::Impl>();
    impl->digest.reset(EVP_MD_CTX_new());
    if (!md || !impl->digest || EVP_DigestInit_ex(impl->digest.get(), md, nullptr) != 1) throw JsError("Digest method not supported");
    impl->md = md;
    return Hash(std::move(impl));
  }

  Hash createHmac(std::string_view algorithm, std::string_view key) { return createHmac(algorithm, bytesOf(key)); }

  Hash createHmac(std::string_view algorithm, BytesView key) {
    const EVP_MD* md = digestByName(algorithm);
    if (!md) throw TypeError("Invalid digest: " + std::string(algorithm));
    auto impl = std::make_shared<Hash::Impl>();
    impl->hmac = true;
    impl->md = md;
    impl->hmacKey.assign(key.begin(), key.end());
    return Hash(std::move(impl));
  }

  bool timingSafeEqual(BytesView a, BytesView b) {
    if (a.size() != b.size()) throw RangeError("Input buffers must have the same byte length");
    return a.empty() || CRYPTO_memcmp(a.data(), b.data(), a.size()) == 0;
  }
}
