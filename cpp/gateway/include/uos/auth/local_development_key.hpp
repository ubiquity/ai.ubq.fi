#pragma once
// Port of src/auth/local-development-key.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv: Kv | null` a std::shared_ptr<rt::Kv> whose
// null pointer is null.
#include "uos/authority/native_contracts.hpp"
#include "uos/paid_fallback/index.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace uos::auth::local_development_key {
  /**
   * The loopback development principal (`--disable-admin-auth` / the Mac service)
   * authenticates every local request as this one API key. It exists so a local
   * request is a *super-admin* principal with no limits rather than a
   * policy-free one: without a key record the paid-provider admission path has no
   * policy to read, so every model that routes directly to a paid provider is
   * refused with `paid_fallback_disabled`.
   *
   * The record is created only by a local loopback server at boot. Its token is
   * minted, stored, and never printed: nothing authenticates with it as a bearer
   * credential, and the bypass never reads it back.
   */
  inline constexpr std::string_view LOCAL_DEVELOPMENT_KEY_ID = "local-development";

  /** The single account the loopback development server provisions for itself. */
  inline constexpr std::string_view LOCAL_DEVELOPMENT_KEY_NAME = "Local development (loopback)";

  /** Extends the retained guard without changing any other guard reader. */
  struct LocalDevelopmentDeletionGuard {
    // `Readonly<{ owner: string | null; completed_at_ms: number | null }>`
    struct LocalDeletion {
      std::optional<std::string> owner;
      std::optional<double> completed_at_ms;
      friend bool operator==(const LocalDeletion&, const LocalDeletion&) = default;
    };

    double created_at_ms;
    LocalDeletion local_deletion;
    friend bool operator==(const LocalDevelopmentDeletionGuard&, const LocalDevelopmentDeletionGuard&) = default;
  };

  /**
   * Fixed internal deadline for the local paid-pricing snapshot.
   *
   * Listener startup awaits local provisioning, so a pricing source that stalls
   * (or that ignores the abort signal it is handed) must not hold the listener
   * open. The deadline is deliberately internal - no environment variable, CLI
   * flag, or caller option - and it leaves generous room next to the 10s fetch
   * budget each upstream metadata call already carries, so a healthy
   * initialization is never cut short.
   */
  inline constexpr double LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS = 30'000;

  /** Test-only override; `null` restores the fixed production deadline. */
  // `deadlineMs: number | null`: std::nullopt is null.
  void setLocalDevelopmentPricingDeadlineMsForTest(std::optional<double> deadlineMs);

  using LocalDevelopmentKeyStatus = std::string;  // "created" | "present" | "revoked" | "unconfigured" | "unavailable" | "conflict"

  /** The pricing fields an unlimited local key inherits; injectable for tests. */
  // `(signal?: AbortSignal) => Promise<Pick<ReturnType<typeof defaultPaidFallbackPolicy>, ...>>`: the
  // Pick is the value initializePaidFallbackPolicy resolves to. std::nullopt is an omitted signal.
  using LocalDevelopmentPricingInitializer =
    std::function<rt::Task<paid_fallback::index::InitializePaidFallbackPolicyResult>(std::optional<rt::AbortSignal> signal)>;

  // The `options` of ensureLocalDevelopmentApiKey.
  struct EnsureLocalDevelopmentApiKeyOptions {
    std::optional<rt::AbortSignal> signal{};
    // An empty function is undefined (initializePaidFallbackPolicy).
    LocalDevelopmentPricingInitializer initializePolicy{};
  };

  /**
   * Creates the loopback development key when it is absent. Idempotent and
   * best-effort: an existing record is never rewritten, so an operator can revoke
   * it to switch local paid routing off, or adjust it in the console. Deleting it
   * re-provisions a fresh unlimited key (with current pricing) at the next local
   * start. Pricing initialization is bounded by
   * {@link LOCAL_DEVELOPMENT_PRICING_DEADLINE_MS}, so listener startup resumes
   * even when the initializer never settles.
   */
  rt::Task<LocalDevelopmentKeyStatus> ensureLocalDevelopmentApiKey(std::shared_ptr<rt::Kv> kv);
  rt::Task<LocalDevelopmentKeyStatus> ensureLocalDevelopmentApiKey(std::shared_ptr<rt::Kv> kv, EnsureLocalDevelopmentApiKeyOptions options);

  /**
   * The policy every loopback development request is authenticated as, or null
   * when the local development key is absent, revoked, malformed, or expired.
   * A null result degrades the local principal to the policy-free `disabled`
   * method, which is the pre-existing behavior.
   */
  // `nowMs = Date.now()`: the one-argument overload reads the clock.
  rt::Task<std::optional<authority::native_contracts::ApiKeyPolicy>> resolveLocalDevelopmentApiKeyPolicy(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<authority::native_contracts::ApiKeyPolicy>> resolveLocalDevelopmentApiKeyPolicy(std::shared_ptr<rt::Kv> kv, double nowMs);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const LocalDevelopmentDeletionGuard::LocalDeletion& value);
  LocalDevelopmentDeletionGuard::LocalDeletion tag_invoke(const json::value_to_tag<LocalDevelopmentDeletionGuard::LocalDeletion>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const LocalDevelopmentDeletionGuard& value);
  LocalDevelopmentDeletionGuard tag_invoke(const json::value_to_tag<LocalDevelopmentDeletionGuard>&, const json::value& value);
}
