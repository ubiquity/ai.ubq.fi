#include "uos/api_keys.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/utils.hpp"

#include <array>
#include <cstdint>

namespace uos::api_keys {
  rt::KvKey apiKeyIdKey(std::string_view id) { return rt::KvKey{API_KEY_ID_PREFIX, id}; }

  rt::KvKey apiKeyHashKey(std::string_view hash) { return rt::KvKey{API_KEY_HASH_PREFIX, hash}; }

  std::optional<double> normalizePaidFallbackMicrocredits(const json::value* value) {
    if (value == nullptr || !rt::isNumber(*value)) return std::nullopt;
    return normalizePaidFallbackMicrocredits(*rt::getNumber(*value));
  }

  std::optional<double> normalizePaidFallbackMicrocredits(const json::value& value) { return normalizePaidFallbackMicrocredits(&value); }

  std::optional<double> normalizePaidFallbackMicrocredits(double value) {
    if (!rt::Number::isSafeInteger(value) || value < 0) return std::nullopt;
    return value;
  }

  std::optional<double> paidFallbackCreditsToMicrocredits(const json::value* value) {
    if (value == nullptr || !rt::isNumber(*value)) return std::nullopt;
    return paidFallbackCreditsToMicrocredits(*rt::getNumber(*value));
  }

  std::optional<double> paidFallbackCreditsToMicrocredits(const json::value& value) { return paidFallbackCreditsToMicrocredits(&value); }

  std::optional<double> paidFallbackCreditsToMicrocredits(double value) {
    if (value == PAID_FALLBACK_NO_LIMIT) return PAID_FALLBACK_NO_LIMIT;
    if (!rt::Number::isFinite(value) || value < 0) return std::nullopt;
    const double microcredits = rt::Math::round(value * MICROCREDITS_PER_CREDIT);
    if (!rt::Number::isSafeInteger(microcredits) || microcredits < 0) return std::nullopt;
    return microcredits;
  }

  double paidFallbackMicrocreditsToCredits(const json::value* value) {
    if (value != nullptr && rt::isNumber(*value)) return paidFallbackMicrocreditsToCredits(*rt::getNumber(*value));
    const double microcredits = normalizePaidFallbackMicrocredits(value).value_or(0);
    return microcredits / MICROCREDITS_PER_CREDIT;
  }

  double paidFallbackMicrocreditsToCredits(const json::value& value) { return paidFallbackMicrocreditsToCredits(&value); }

  double paidFallbackMicrocreditsToCredits(double value) {
    if (value == PAID_FALLBACK_NO_LIMIT) return PAID_FALLBACK_NO_LIMIT;
    const double microcredits = normalizePaidFallbackMicrocredits(value).value_or(0);
    return microcredits / MICROCREDITS_PER_CREDIT;
  }

  std::string generateApiKeyToken() {
    std::array<std::uint8_t, 32> bytes{};
    rt::crypto::getRandomValues(bytes);
    return "u_" + utils::encodeHex(bytes);
  }

  double coerceApiKeyExpiresAtMs(const json::value* record) {
    if (!utils::isRecord(record)) return API_KEY_NO_EXPIRATION_MS;
    const std::optional<double> value = rt::getNumber(rt::get(*record, "expires_at_ms"));
    if (!value || !rt::Number::isFinite(*value)) return API_KEY_NO_EXPIRATION_MS;
    const double expiresAtMs = rt::Math::trunc(*value);
    if (expiresAtMs == API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
    if (expiresAtMs < 0) return API_KEY_NO_EXPIRATION_MS;
    return expiresAtMs;
  }

  double coerceApiKeyExpiresAtMs(const json::value& record) { return coerceApiKeyExpiresAtMs(&record); }

  double coerceApiKeyExpiresAtMs(const json::object& record) {
    const std::optional<double> value = rt::getNumber(record, "expires_at_ms");
    if (!value || !rt::Number::isFinite(*value)) return API_KEY_NO_EXPIRATION_MS;
    const double expiresAtMs = rt::Math::trunc(*value);
    if (expiresAtMs == API_KEY_NO_EXPIRATION_MS) return API_KEY_NO_EXPIRATION_MS;
    if (expiresAtMs < 0) return API_KEY_NO_EXPIRATION_MS;
    return expiresAtMs;
  }

  double normalizeApiKeyWindowMs(const json::value* value, double fallback) {
    if (value == nullptr || !rt::isNumber(*value)) return fallback;
    return normalizeApiKeyWindowMs(*rt::getNumber(*value), fallback);
  }

  double normalizeApiKeyWindowMs(const json::value& value, double fallback) { return normalizeApiKeyWindowMs(&value, fallback); }

  double normalizeApiKeyWindowMs(double value, double fallback) {
    if (!rt::Number::isFinite(value)) return fallback;
    const double windowMs = rt::Math::trunc(value);
    if (windowMs <= 0) return fallback;
    return windowMs;
  }

  double coerceApiKeyWindowMs(const json::value* record, double fallback) {
    if (!utils::isRecord(record)) return fallback;
    return normalizeApiKeyWindowMs(rt::get(*record, "window_ms"), fallback);
  }

  double coerceApiKeyWindowMs(const json::value& record, double fallback) { return coerceApiKeyWindowMs(&record, fallback); }

  double coerceApiKeyWindowMs(const json::object& record, double fallback) { return normalizeApiKeyWindowMs(rt::get(record, "window_ms"), fallback); }

  double calculateNextResetMs(double nowMs, double windowMs) { return nowMs + windowMs; }

  double getDefaultExpiryMs(double nowMs) { return nowMs + DEFAULT_EXPIRY_DAYS * 24 * 60 * 60 * 1000; }
}
