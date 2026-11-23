#pragma once
// Port of src/codex/rate-limit-429.ts.
// Native Codex 429 I/O and CAS orchestration over the source-owned portable decisions.
#include "uos/authority/native_codex_429.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

namespace uos::codex::rate_limit_429 {
  // export { type Codex429Classification, type CodexKnownProbeLease } from "../authority/native-codex-429.ts"
  using authority::native_codex_429::Codex429Classification;
  using authority::native_codex_429::CodexKnownProbeLease;

  // Not async in the TS: it returns classifyCodex429's promise. `now = Date.now()` is the
  // overload without it.
  [[nodiscard]] rt::Task<Codex429Classification> readCodex429(rt::Response response);
  [[nodiscard]] rt::Task<Codex429Classification> readCodex429(rt::Response response, double now);

  // `now = Date.now()` is the overload without it; `recoveryProbe = false`.
  [[nodiscard]] rt::Task<Codex429Classification> markCodexQuotaBlockedWithMode(routing_state::RoutingAccount account, rt::Response response);
  [[nodiscard]] rt::Task<Codex429Classification> markCodexQuotaBlockedWithMode(routing_state::RoutingAccount account, rt::Response response, double now,
                                                                             bool recoveryProbe = false);

  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<Codex429Classification> markCodexQuotaBlocked(routing_state::RoutingAccount account, rt::Response response);
  [[nodiscard]] rt::Task<Codex429Classification> markCodexQuotaBlocked(routing_state::RoutingAccount account, rt::Response response, double now);
}
