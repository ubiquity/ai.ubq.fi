#pragma once
// Port of src/admin/codex-reset.ts.
#include "uos/codex/manual_reset.hpp"
#include "uos/rt/http.hpp"
namespace uos::admin::codex_reset {
  rt::Task<rt::Response> handleAdminCodexResetConsume(rt::Request request);
  rt::Task<rt::Response> handleAdminCodexResetConsume(rt::Request request, ::uos::codex::manual_reset::ManualResetDependencies dependencies);
}
