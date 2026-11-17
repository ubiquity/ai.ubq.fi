#pragma once
// Port of src/kv-conversion/cli.ts. The module exports nothing; its top-level
// `if (import.meta.main) main().catch(...)` is what running it as the program does.
#include "uos/rt/async.hpp"

namespace uos::kv_conversion::cli {
  // The `if (import.meta.main)` block: runs `main()` over rt::process::argv() (Bun.argv) and
  // completes when its `.catch` handler has run (an error prints its message, or "Conversion
  // failed", with console.error and sets process.exitCode = 1). A C++ entry point calls
  // rt::process::setArgv, awaits this on the loop and returns rt::process::exitStatus().
  [[nodiscard]] rt::Task<void> importMetaMain();
}
