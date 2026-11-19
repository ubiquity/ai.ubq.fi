// Links: tests/admin_capacity_status_test.cpp src/provider/selection.cpp src/kv.cpp src/storage/kv.cpp src/storage/key_compatibility.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp
// Port of tests/admin-capacity-status.test.ts: the cases that call src/provider/selection.ts.
//
// Every case of the TS file executes slices of the shipped browser console (static/admin.js) inside node:vm against a
// DOM stub; that browser code has no C++ counterpart, so those cases are not portable here: "Claude credential actions
// stay within the existing provider entry and survive roster renders", "provider selection save ignores a response after
// the gateway target changes", "Claude credential disclosure uses the existing single-column body and wrapping form
// actions", "shared Kernel metadata shrinks and wraps long values without hiding content", "shared Claude analytics
// diagnostics retain credential, workspace and routing facts", "shipped Claude rows never reuse another account or
// replacement cohort's masked diagnostics", "the Claude meter displays observed response headers without a direct
// refresh action", "shipped shared capacity list supports legacy singleton and distinct roster rows with stable
// disclosure state", "shipped routing warnings describe only disabled capacity and paid fallback, not a false Claude
// paid-first route", "shipped capacity summary counts exactly displayed Claude roster or legacy rows without changing
// Codex chart inputs", "current accepted quota access is reachable while its failed refresh attempt stays visible",
// "unavailable, stale and expired access retain warnings, while inference success remains healthy", "capacity refresh
// preserves a scrolled anchor before the queued scroll event", "capacity refresh retains follow-latest intent after a
// hidden render", "shipped retention renderer preserves measured zero, normal accounting and storage warnings",
// "shipped retention renderer refuses incomplete, errored, unavailable or corrupt accounting without hiding log
// warnings", "shipped Claude chart uses actual window timestamps and never anchors a current value", "shipped Claude
// chart breaks cohort, reset, unavailable and missing-bucket runs without inventing downtime", "shipped shared account
// controls preserve scoped drafts and send only explicit account/access actions", "a narrowed Codex tier filtered alone
// keeps Uncheck shown enabled, and unchecking clears its subscription", "a narrowed Codex tier filtered alone keeps
// Check shown enabled, and checking expands it to every subscription", "Only active keeps a narrowed Codex row shown and
// rerenders after Uncheck shown clears its subscription", "provider bulk buttons keep their umbrella and unchecked-row
// states", "provider saves reject empty and missing-only selections through both the button and handler", "Uncheck all
// removes missing Codex ids along with configured subscriptions", "provider save toasts count saved tiers, not
// subscription or missing ids", "admin API-key editors retain staged and in-flight changes through another save and
// expiry", "admin API-key expiry updates filters without detaching open editors" and "admin API-key actions execute
// confirmation, revoke and expired unrevoke".
//
// "the provider picker preserves missing subscriptions until the Codex tier is explicitly unchecked" mixes the two: the
// console's picker (browser code) serializes staged ids, and the test checks those ids against the server's selection
// validation. The C++ case runs the server-side assertions over exactly the id lists the TS asserts the picker
// serializes at each step (its `assert.deepEqual(serialize(), [...])` literals).
#include "uos/provider/selection.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <string>
#include <variant>
#include <vector>

using namespace uos;
namespace selection = provider::selection;

namespace {
  std::vector<std::string> sorted(std::vector<std::string> values) {
    rt::sort(values, [](const std::string& left, const std::string& right) { return static_cast<double>(rt::localeCompare(left, right)); });
    return values;
  }

  // The `serialize` helper of the TS case, minus the browser picker: every serialized id passes the POST validation and
  // survives storage normalization, so the console saves exactly what it shows as staged.
  std::vector<std::string> serialize(const std::vector<std::string>& ids) {
    EXPECT(std::all_of(ids.begin(), ids.end(), [](const std::string& id) { return selection::isProviderSelectionId(std::string_view(id)); }));
    EXPECT_EQ(sorted(selection::normalizeSelectedProviderIds(ids)), sorted(ids));
    return ids;
  }
}

TEST("the provider picker preserves missing subscriptions until the Codex tier is explicitly unchecked") {
  const std::string removed = "codex:" + std::string(64, 'a');
  const std::string configuredA = "codex:" + std::string(64, 'b');

  // load([removed, "surplus"]): saving an untouched restriction keeps the removed subscription.
  serialize({"surplus", removed});
  // picker.setProviderChecked("surplus", false): a nonempty staged restriction never serializes to the empty no-filter list.
  serialize({removed});

  // load([removed, "surplus"]); picker.setSubscriptionChecked(configuredA, true): narrowing to a configured account keeps the removed one.
  serialize({"surplus", configuredA, removed});

  // load([removed, configuredA, "surplus"]); picker.setProviderChecked("codex", false): turning Codex off also removes its missing subscriptions.
  serialize({"surplus"});
  const std::optional<selection::ProviderSelection> saved =
    selection::ProviderSelection{.provider_ids = selection::normalizeSelectedProviderIds(serialize({"surplus"})), .updated_at_ms = 1};
  EXPECT_EQ(selection::isProviderEnabled("codex", saved), false);  // the server also sees Codex as disabled
  EXPECT(std::holds_alternative<selection::CodexAccountEligibilityNone>(selection::codexAccountEligibility(saved)));

  // load([removed, "surplus"]); picker.setProviderChecked("codex", true): the Codex umbrella already covers every configured account.
  serialize({"codex", "surplus"});
  // picker.setProviderChecked("openlux", true): checking every provider is still the no-filter state.
  serialize({});
}
