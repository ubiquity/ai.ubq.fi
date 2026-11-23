#pragma once
// Port of src/api-keys.ts.
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace uos::api_keys {
  inline const rt::KvKey API_KEY_ID_PREFIX{"ubq_ai", "api_keys", "id"};
  inline const rt::KvKey API_KEY_HASH_PREFIX{"ubq_ai", "api_keys", "hash"};
  inline constexpr double API_KEY_NO_EXPIRATION_MS = -1;
  inline constexpr double API_KEY_NO_USAGE_LIMIT = -1;
  inline constexpr double PAID_FALLBACK_NO_LIMIT = -1;
  inline constexpr double MICROCREDITS_PER_CREDIT = 1'000'000;

  inline constexpr double DEFAULT_USAGE_LIMIT_REQUESTS = 50;
  inline constexpr double DEFAULT_EXPIRY_DAYS = 90;
  inline constexpr double USAGE_RESET_PERIOD_MS = 7.0 * 24 * 60 * 60 * 1000;

  rt::KvKey apiKeyIdKey(std::string_view id);
  rt::KvKey apiKeyHashKey(std::string_view hash);

  // `value: unknown`: a pointer is the member as read (nullptr is undefined); a double is a number.
  std::optional<double> normalizePaidFallbackMicrocredits(const json::value* value);
  std::optional<double> normalizePaidFallbackMicrocredits(const json::value& value);
  std::optional<double> normalizePaidFallbackMicrocredits(double value);

  std::optional<double> paidFallbackCreditsToMicrocredits(const json::value* value);
  std::optional<double> paidFallbackCreditsToMicrocredits(const json::value& value);
  std::optional<double> paidFallbackCreditsToMicrocredits(double value);

  double paidFallbackMicrocreditsToCredits(const json::value* value);
  double paidFallbackMicrocreditsToCredits(const json::value& value);
  double paidFallbackMicrocreditsToCredits(double value);

  std::string generateApiKeyToken();

  double coerceApiKeyExpiresAtMs(const json::value* record);
  double coerceApiKeyExpiresAtMs(const json::value& record);
  double coerceApiKeyExpiresAtMs(const json::object& record);

  double normalizeApiKeyWindowMs(const json::value* value, double fallback = USAGE_RESET_PERIOD_MS);
  double normalizeApiKeyWindowMs(const json::value& value, double fallback = USAGE_RESET_PERIOD_MS);
  double normalizeApiKeyWindowMs(double value, double fallback = USAGE_RESET_PERIOD_MS);

  double coerceApiKeyWindowMs(const json::value* record, double fallback = USAGE_RESET_PERIOD_MS);
  double coerceApiKeyWindowMs(const json::value& record, double fallback = USAGE_RESET_PERIOD_MS);
  double coerceApiKeyWindowMs(const json::object& record, double fallback = USAGE_RESET_PERIOD_MS);

  double calculateNextResetMs(double nowMs, double windowMs = USAGE_RESET_PERIOD_MS);

  double getDefaultExpiryMs(double nowMs);
}
