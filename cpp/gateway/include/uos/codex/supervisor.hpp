#pragma once
// Port of src/codex/supervisor.ts.
// Supervisor admin handlers, split out of the former monolithic module.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

namespace uos::codex::supervisor {
  rt::Task<rt::Response> handleAdminCodexSupervisorSessions();

  /**
   * SSE follow for one selected session. It polls the newest persisted turn every
   * two seconds and appends newly recorded assistant messages and command output.
   * It is explicitly not a token-level stream, and reports `unavailable` instead
   * of inventing output when the source cannot be read.
   */
  rt::Task<rt::Response> handleAdminCodexSupervisorOutput(rt::Request req);
}
