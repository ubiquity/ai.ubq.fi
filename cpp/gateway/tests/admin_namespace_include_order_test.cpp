// Links: tests/admin_namespace_include_order_test.cpp
// The two admin siblings must preserve lookup of their core defining namespaces.
#include "uos/admin/defaults.hpp"
#include "uos/admin/codex.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/codex_reset.hpp"
#include "uos/rt/test.hpp"
#include <type_traits>
TEST("admin sibling includes preserve defining core types") {
  using Extract = decltype(uos::admin::api_keys::extractModelReasoningLevels(std::optional<uos::json::object>{}));
  static_assert(std::is_same_v<Extract, std::vector<uos::defaults::ReasoningEffort>>);
  using Reset = uos::rt::Task<uos::rt::Response> (*)(uos::rt::Request, uos::codex::manual_reset::ManualResetDependencies);
  static_assert(std::is_same_v<decltype(static_cast<Reset>(&uos::admin::codex_reset::handleAdminCodexResetConsume)), Reset>);
  EXPECT(true);
}
