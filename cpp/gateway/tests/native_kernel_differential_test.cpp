// Differential cases for src/authority/native-kernel.ts beyond the conformance vectors, recorded from
// the TypeScript under Bun 1.4.0: normalizeUsageLimit / normalizeWindow / normalizeExpiration over
// unknown inputs, the policy, window, reservation and cutover normalizers, kernelWindowScopeTarget,
// settledKernelReservationRow (UTF-16 truncation of the reason) and requireKernelReservationRow.
// The TS reservation and cutover normalizers return the stored object itself; the C++ structs drop
// undeclared members and an org row's `repo: null` (documented divergence, checked explicitly).
#include "uos/authority/native_kernel.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <cstdio>
#include <optional>
#include <string>

#include "native_kernel_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace kernel = uos::authority::native_kernel;
using namespace uos::authority::native_kernel_contracts;

namespace {
  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(kNativeKernelVectors);
    return parsed;
  }
  const json::array& section(std::string_view name) { return *rt::getArray(vectors(), name); }
  // {"nonFinite": "Infinity"} back to the number it stands for.
  json::value input(const json::value& value) {
    if (const json::object* object = value.if_object(); object != nullptr && object->size() == 1 && object->contains("nonFinite"))
      return rt::makeNumber(rt::stringToNumber(object->at("nonFinite").as_string()));
    return value;
  }
  std::optional<std::string> repoOf(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return std::string(value.as_string());
  }
  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }
  // The expected stored object without what the C++ struct cannot carry.
  json::value declaredOnly(const json::value& expected, std::initializer_list<std::string_view> declared) {
    if (!expected.is_object()) return expected;
    json::object out;
    for (const std::string_view key : declared) {
      const json::value* member = rt::get(expected, key);
      if (member == nullptr) continue;
      if (key == "repo" && member->is_null()) continue;
      out[key] = *member;
    }
    return out;
  }
  template <class T> json::value orNull(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
}

TEST("normalizeUsageLimit, normalizeWindow and normalizeExpiration match the oracle") {
  int cases = 0;
  for (const json::value& pair : section("limits")) {
    const json::value value = input(pair.as_array()[0]);
    EXPECT_EQ(kernel::normalizeUsageLimit(&value, 42, -1), *rt::getNumber(pair.as_array()[1]));
    ++cases;
  }
  for (const json::value& pair : section("windows")) {
    const json::value value = input(pair.as_array()[0]);
    EXPECT_EQ(kernel::normalizeWindow(&value, 42), *rt::getNumber(pair.as_array()[1]));
  }
  for (const json::value& pair : section("expirations")) {
    const json::value value = input(pair.as_array()[0]);
    EXPECT_EQ(kernel::normalizeExpiration(&value, -1), *rt::getNumber(pair.as_array()[1]));
  }
  const json::array& absent = section("absent");
  EXPECT_EQ(kernel::normalizeUsageLimit(nullptr, 42, -1), *rt::getNumber(absent[0]));
  EXPECT_EQ(kernel::normalizeWindow(nullptr, 42), *rt::getNumber(absent[1]));
  EXPECT_EQ(kernel::normalizeExpiration(nullptr, -1), *rt::getNumber(absent[2]));
  EXPECT_EQ(cases, 22);
}

TEST("the row normalizers match the oracle") {
  const KernelQuotaConstants constants{.noUsageLimit = -1, .noExpirationMs = -1};
  int index = 0;
  for (const json::value& vector : section("normalizedPolicies")) {
    const json::array& row = vector.as_array();
    const std::optional<KernelQuotaPolicyV2> policy =
      kernel::normalizeKernelQuotaPolicyV2(row[0], std::string(row[1].as_string()), std::string(row[2].as_string()), repoOf(row[3]), constants);
    EXPECT(same(orNull(policy), row[4], "policy " + std::to_string(index++)));
  }
  index = 0;
  for (const json::value& vector : section("normalizedWindows")) {
    const std::optional<KernelQuotaWindowV2> window = kernel::normalizeKernelQuotaWindowV2(vector.as_array()[0], "repo", "o", "r");
    EXPECT(same(orNull(window), vector.as_array()[1], "window " + std::to_string(index++)));
  }
  index = 0;
  for (const json::value& vector : section("normalizedReservations")) {
    const std::optional<KernelQuotaReservationRowV2> reservation = kernel::normalizeKernelQuotaReservationRowV2(vector.as_array()[0], "org", "o", std::nullopt);
    const json::value expected =
      declaredOnly(vector.as_array()[1], {"v", "scope", "owner", "repo", "request_id", "route", "window_created_at_ms", "window_reset_at_ms", "state", "terminal_intent",
                                          "reserved_at_ms", "lease_expires_at_ms", "committed_at_ms", "released_at_ms", "release_reason"});
    EXPECT(same(orNull(reservation), expected, "reservation " + std::to_string(index++)));
  }
  index = 0;
  for (const json::value& vector : section("normalizedCutovers")) {
    const std::optional<KernelDefaultWindowCutoverV2> cutover = kernel::normalizeKernelDefaultWindowCutoverV2(vector.as_array()[0]);
    const json::value expected = declaredOnly(vector.as_array()[1], {"v", "id", "created_at_ms", "expires_at_ms"});
    EXPECT(same(orNull(cutover), expected, "cutover " + std::to_string(index++)));
  }
  // The divergence itself: the TS keeps an undeclared member and an org row's `repo: null`.
  const json::array& reservations = section("normalizedReservations");
  EXPECT(rt::get(reservations[14].as_array()[1], "repo") != nullptr && rt::get(reservations[14].as_array()[1], "repo")->is_null());
  EXPECT(rt::get(reservations[16].as_array()[1], "extra") != nullptr);
  EXPECT(rt::get(section("normalizedCutovers")[3].as_array()[1], "extra") != nullptr);
}

TEST("kernelWindowScopeTarget, settledKernelReservationRow and requireKernelReservationRow match the oracle") {
  const rt::KvKey prefixBase{"uos_ai", "kernel_quota", "v2"};
  for (const json::value& vector : section("targets")) {
    const json::array& row = vector.as_array();
    const std::string scope(row[0].as_string());
    rt::KvKey key;
    for (const json::value& part : row[1].as_array()) key.push_back(part.is_string() ? rt::KvKeyPart(std::string(part.as_string())) : rt::KvKeyPart(*rt::getNumber(part)));
    const rt::KvKey prefix{prefixBase, scope + "_window"};
    json::value actual;
    try {
      const kernel::KernelWindowScopeTarget target = kernel::kernelWindowScopeTarget(scope, key, prefix);
      json::object value;
      value["owner"] = target.owner;
      if (target.repo) value["repo"] = *target.repo;
      json::object wrapped;
      wrapped["value"] = std::move(value);
      actual = std::move(wrapped);
    } catch (const rt::JsError& error) {
      json::object wrapped;
      wrapped["error"] = error.message;
      actual = std::move(wrapped);
    }
    EXPECT(same(actual, row[2], "target " + rt::jsonStringify(row[1])));
  }
  const KernelQuotaReservationRowV2 reservation = json::value_to<KernelQuotaReservationRowV2>(section("normalizedReservations")[0].as_array()[0]);
  for (const json::value& vector : section("settled")) {
    const json::array& row = vector.as_array();
    const KernelQuotaReservationRowV2 settled = kernel::settledKernelReservationRow(reservation, std::string(row[0].as_string()), std::string(row[1].as_string()), 99);
    EXPECT(same(json::value_from(settled), row[2], "settled " + std::string(row[0].as_string())));
  }
  for (const json::value& vector : section("required")) {
    const json::array& row = vector.as_array();
    rt::KvKey key;
    for (const json::value& part : row[0].as_array()) key.push_back(part.is_string() ? rt::KvKeyPart(std::string(part.as_string())) : rt::KvKeyPart(*rt::getNumber(part)));
    json::value actual;
    try {
      const KernelQuotaReservationRowV2 required = kernel::requireKernelReservationRow(key, row[1], "org", "o", std::nullopt, *rt::getNumber(row[2]));
      json::object wrapped;
      wrapped["value"] = json::value_from(required);
      actual = std::move(wrapped);
    } catch (const rt::JsError& error) {
      json::object wrapped;
      wrapped["error"] = error.message;
      actual = std::move(wrapped);
    }
    EXPECT(same(actual, row[3], "required " + rt::jsonStringify(row[0])));
  }
}
