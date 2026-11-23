#pragma once
// Port of lib/jev_compaction/jev-codex-smoke.ts.
#include "uos/rt/async.hpp"

namespace uos::lib::jev_compaction::jev_codex_smoke {
  // Explicit boundary for the complete source script, including Codex process/live-Jev selection and exit.
  // Never invoked by module registration or linking. Requires separately authorized smoke execution.
  rt::Task<void> importMetaMain();
}
