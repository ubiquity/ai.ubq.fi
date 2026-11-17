#pragma once
// Port of lib/jev_compaction/jev-compaction-smoke.ts.
#include "uos/rt/async.hpp"

namespace uos::lib::jev_compaction::jev_compaction_smoke {
  // Explicit boundary for the complete source script, including cleanup and exit.
  // Never invoked by module registration or linking. Requires separately authorized smoke execution.
  rt::Task<void> importMetaMain();
}
