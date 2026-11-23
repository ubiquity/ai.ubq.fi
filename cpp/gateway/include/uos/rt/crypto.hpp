#pragma once
// The global `crypto` object (Web Crypto: randomUUID, getRandomValues, subtle) and
// the node:crypto functions, with Bun 1.4's results, error classes and messages.
// OpenSSL 3 EVP underneath. Every `crypto.subtle` method is synchronous here; see
// docs/rt/platform.md for keeping the JS suspension point. Errors: Web Crypto
// failures throw rt::DOMException (OperationError, DataError, NotSupportedError,
// InvalidAccessError) or rt::TypeError / rt::SyntaxError with Bun's text.
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace uos::rt::crypto {
  using Bytes = std::vector<std::uint8_t>;
  using BytesView = std::span<const std::uint8_t>;

  // UTF-8 bytes of a string (`new TextEncoder().encode(text)` for well-formed text).
  inline BytesView bytesOf(std::string_view text) noexcept { return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}; }

  // ---- Randomness. Every random byte below comes from the current source.

  // `crypto.randomUUID()`: RFC 4122 version 4, lowercase.
  std::string randomUUID();
  // `crypto.getRandomValues(new Uint8Array(n))`. Bun enforces no 65536-byte quota.
  void getRandomValues(std::span<std::uint8_t> array);
  // `crypto.getRandomValues(new Uint32Array(n))`: words are the random bytes read
  // in host (little-endian) order, as the typed array views them.
  void getRandomValues(std::span<std::uint32_t> array);
  // node:crypto `randomBytes(size)`. RangeError outside 0 .. 2147483647.
  Bytes randomBytes(double size);

  // Test hook: replaces the random source for the current thread while alive
  // (OpenSSL RAND_bytes otherwise). Nested scopes restore the previous source.
  using RandomSource = std::function<void(std::span<std::uint8_t>)>;
  class ScopedRandomSource {
  public:
    explicit ScopedRandomSource(RandomSource source);
    ~ScopedRandomSource();
    ScopedRandomSource(const ScopedRandomSource&) = delete;
    ScopedRandomSource& operator=(const ScopedRandomSource&) = delete;

  private:
    RandomSource previous_;
  };

  // ---- Web Crypto keys

  // `key.algorithm` (KeyAlgorithm and its subtypes). `hash` is the canonical hash
  // name ("SHA-256"); `length` is AES/HMAC; modulusLength/publicExponent are RSA.
  struct KeyAlgorithm {
    std::string name;
    std::optional<std::string> hash;
    std::optional<std::uint32_t> length;
    std::optional<std::uint32_t> modulusLength;
    std::optional<Bytes> publicExponent;
  };

  // A CryptoKey. Copies share the key (JS object identity is not observable here).
  class CryptoKey {
  public:
    struct Impl;
    explicit CryptoKey(std::shared_ptr<const Impl> state);
    // "secret", "public" or "private".
    const std::string& type() const noexcept;
    bool extractable() const noexcept;
    const KeyAlgorithm& algorithm() const noexcept;
    // Canonical order (encrypt, decrypt, sign, verify, deriveKey, deriveBits,
    // wrapKey, unwrapKey) without duplicates, as Bun reports them.
    const std::vector<std::string>& usages() const noexcept;
    const Impl& impl() const noexcept { return *impl_; }

  private:
    std::shared_ptr<const Impl> impl_;
  };

  struct CryptoKeyPair {
    CryptoKey publicKey;
    CryptoKey privateKey;
  };

  // An algorithm identifier or params dictionary (`{ name: "AES-GCM", iv }`).
  // Members are in a fixed order for designated initializers; set only those the
  // TypeScript object literal has. `hash` is a name ("SHA-256"); the `{ name }`
  // form of a hash is written as its name.
  struct Algorithm {
    std::string name = {};
    std::optional<std::string> hash = std::nullopt;
    std::optional<Bytes> iv = std::nullopt;
    std::optional<Bytes> additionalData = std::nullopt;
    std::optional<std::uint32_t> tagLength = std::nullopt;
    std::optional<Bytes> salt = std::nullopt;
    std::optional<Bytes> info = std::nullopt;
    std::optional<std::uint32_t> iterations = std::nullopt;
    std::optional<std::uint32_t> length = std::nullopt;
    std::optional<std::uint32_t> modulusLength = std::nullopt;
    std::optional<Bytes> publicExponent = std::nullopt;
  };

  // `crypto.subtle`. Implemented algorithms: digest SHA-1/224/256/384/512;
  // AES-GCM (raw/jwk import, raw/jwk export, encrypt/decrypt with any non-empty
  // IV, additionalData, tagLength); HMAC (raw/jwk); HKDF and PBKDF2 (raw import,
  // deriveBits/deriveKey to AES-GCM or HMAC); RSASSA-PKCS1-v1_5 (spki/pkcs8/jwk
  // import and export, sign/verify, generateKey). A name Web Crypto knows but this
  // file does not implement (AES-CBC, ECDSA, Ed25519, RSA-PSS, ...) throws
  // std::logic_error: that is a porting gap, not a JS behaviour. JWK members
  // convert like WebIDL: strings by ToString (5 -> "5", null -> "null"), `ext`
  // by ToBoolean, `key_ops` must be an array of usage names (TypeError otherwise).
  namespace subtle {
    Bytes digest(const Algorithm& algorithm, BytesView data);
    Bytes digest(std::string_view algorithm, BytesView data);

    // format "raw", "spki" or "pkcs8".
    CryptoKey importKey(std::string_view format, BytesView keyData, const Algorithm& algorithm, bool extractable, const std::vector<std::string>& keyUsages);
    CryptoKey importKey(std::string_view format, BytesView keyData, std::string_view algorithm, bool extractable, const std::vector<std::string>& keyUsages);
    // format "jwk" with a JsonWebKey object.
    CryptoKey importKey(std::string_view format, const json::object& keyData, const Algorithm& algorithm, bool extractable, const std::vector<std::string>& keyUsages);
    CryptoKey importKey(std::string_view format, const json::object& keyData, std::string_view algorithm, bool extractable, const std::vector<std::string>& keyUsages);

    // `exportKey("raw" | "spki" | "pkcs8", key)`.
    Bytes exportKey(std::string_view format, const CryptoKey& key);
    // `exportKey("jwk", key)`: members in Bun's (alphabetical) order.
    json::object exportJwk(const CryptoKey& key);

    Bytes encrypt(const Algorithm& algorithm, const CryptoKey& key, BytesView data);
    Bytes decrypt(const Algorithm& algorithm, const CryptoKey& key, BytesView data);

    Bytes sign(const Algorithm& algorithm, const CryptoKey& key, BytesView data);
    Bytes sign(std::string_view algorithm, const CryptoKey& key, BytesView data);
    bool verify(const Algorithm& algorithm, const CryptoKey& key, BytesView signature, BytesView data);
    bool verify(std::string_view algorithm, const CryptoKey& key, BytesView signature, BytesView data);

    // `deriveBits(algorithm, baseKey, length)`; std::nullopt is `null`/`undefined`.
    Bytes deriveBits(const Algorithm& algorithm, const CryptoKey& baseKey, std::optional<std::uint32_t> length);
    CryptoKey deriveKey(const Algorithm& algorithm, const CryptoKey& baseKey, const Algorithm& derivedKeyType, bool extractable,
                        const std::vector<std::string>& keyUsages);
    CryptoKey deriveKey(const Algorithm& algorithm, const CryptoKey& baseKey, std::string_view derivedKeyType, bool extractable,
                        const std::vector<std::string>& keyUsages);

    // RSASSA-PKCS1-v1_5 only: `{ name, hash, modulusLength, publicExponent }`.
    // The public key is always extractable and gets the "verify" usage.
    CryptoKeyPair generateKey(const Algorithm& algorithm, bool extractable, const std::vector<std::string>& keyUsages);
  }

  // ---- node:crypto

  // `createHash(algorithm)`: any OpenSSL digest name, case-insensitive ("sha256",
  // "SHA-256", "RSA-SHA256", "md5"). Unknown: Error "Digest method not supported".
  class Hash {
  public:
    // `hash.update(data)`: strings are UTF-8 (the default encoding). Returns *this.
    Hash& update(std::string_view data);
    Hash& update(BytesView data);
    Hash& update(const Bytes& data) { return update(BytesView(data)); }
    Hash& update(const char* data) { return update(std::string_view(data)); }
    // `hash.digest()` (a Buffer).
    Bytes digest();
    // `hash.digest(encoding)` for "hex", "base64", "base64url" and "latin1"/"binary"
    // (one code point per byte). Other encodings throw std::invalid_argument.
    // A second digest() throws Error "Digest already called" (Hmac: returns empty).
    std::string digest(std::string_view encoding);

    struct Impl;
    explicit Hash(std::shared_ptr<Impl> state);

  private:
    std::shared_ptr<Impl> impl_;
  };
  Hash createHash(std::string_view algorithm);

  // `createHmac(algorithm, key)`: TypeError "Invalid digest: <name>" for an unknown
  // digest. A second digest() returns "" / empty bytes, as in Bun; update() after
  // digest() throws Error "Digest already called".
  Hash createHmac(std::string_view algorithm, std::string_view key);
  Hash createHmac(std::string_view algorithm, BytesView key);

  // `timingSafeEqual(a, b)`: RangeError "Input buffers must have the same byte length".
  bool timingSafeEqual(BytesView a, BytesView b);
}
