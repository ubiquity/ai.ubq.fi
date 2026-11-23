// Differential cases for src/kernel/policy-queue.ts (no direct TS tests). The expected values were
// printed by Bun 1.4.0 running the TS module against a JS fake KV with versionstamped atomic checks,
// a fixed Date.now and console.warn recorded; here rt::MemoryKv sits behind a forwarding wrapper that
// can lose commits or fail reads the same way.
// Links: tests/kernel_policy_queue_differential_test.cpp src/kernel/policy_queue.cpp src/types.cpp src/kv.cpp src/storage/kv.cpp
//   src/release.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/kernel/policy_queue.hpp"
#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <cmath>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::kernel::policy_queue::listKernelPolicyQueue;
using uos::kernel::policy_queue::recordKernelPolicyQueue;
using uos::types::KernelPolicyQueueItem;

namespace {
  struct Faults {
    int failCommits = 0;
    bool throwOnGet = false;
  };

  class LosingOperation final : public rt::ForwardingAtomicOperation {
  public:
    LosingOperation(std::unique_ptr<rt::AtomicOperation> inner_, std::shared_ptr<Faults> faults_)
      : rt::ForwardingAtomicOperation(std::move(inner_)), faults(std::move(faults_)) {}

    rt::Task<rt::KvCommitResult> commit() override {
      if (faults->failCommits > 0) {
        faults->failCommits -= 1;
        co_await rt::microtask();
        co_return rt::KvCommitResult{};
      }
      rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
      const rt::KvCommitResult result = co_await std::move(committing);
      co_return result;
    }

  private:
    std::shared_ptr<Faults> faults;
  };

  class QueueKv final : public rt::ForwardingKv {
  public:
    explicit QueueKv(std::shared_ptr<rt::MemoryKv> store_) : rt::ForwardingKv(*store_), store(std::move(store_)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      if (faults->throwOnGet) throw rt::TypeError("get exploded");
      rt::Task<rt::KvEntryMaybe<>> reading = rt::ForwardingKv::get(std::move(key), options);
      rt::KvEntryMaybe<> entry = co_await std::move(reading);
      co_return entry;
    }

    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<LosingOperation>(rt::ForwardingKv::atomic(), faults); }

    std::shared_ptr<Faults> faults = std::make_shared<Faults>();

  private:
    std::shared_ptr<rt::MemoryKv> store;
  };

  const rt::KvKey KEY{"uos_ai", "kernel_policy_queue"};

  rt::Task<std::string> storedJson(std::shared_ptr<QueueKv> kv) {
    const rt::KvEntryMaybe<> entry = co_await kv->rt::ForwardingKv::get(KEY, rt::KvGetOptions{});
    co_return rt::jsonStringify(entry.value.toJson());
  }

  rt::Task<void> seed(std::shared_ptr<QueueKv> kv, rt::KvValue value) {
    const rt::KvCommitResult committed = co_await kv->set(KEY, std::move(value), rt::KvSetOptions{});
    EXPECT(committed.ok);
  }

  std::string listJson(const std::optional<std::vector<KernelPolicyQueueItem>>& items) {
    if (!items) return "null";
    return rt::jsonStringify(json::value_from(*items));
  }

  // The warnings recorded since the last call, as the oracle printed them (an Error as "Name: message").
  std::vector<std::vector<std::string>> drainWarnings(rt::console::Capture& capture) {
    std::vector<std::vector<std::string>> out;
    for (const rt::console::Call& call : capture.calls()) {
      std::vector<std::string> args;
      for (const rt::console::Value& arg : call.args) {
        if (arg.kind() == rt::console::Value::Kind::error) args.push_back(arg.errorInfo().name + ": " + arg.errorInfo().message);
        else args.push_back(arg.string());
      }
      out.push_back(std::move(args));
    }
    capture.clear();
    return out;
  }

  using Warnings = std::vector<std::vector<std::string>>;
}

ASYNC_TEST("policy queue records, updates, normalizes, caps and fails soft like the TS module") {
  double clock = 1'700'000'000'000;
  rt::globalDateNow() = [&clock] { return clock; };
  auto kv = std::make_shared<QueueKv>(std::make_shared<rt::MemoryKv>([&clock] { return clock; }));
  uos::kv::setKvForTest(kv);
  rt::console::Capture capture({rt::console::Method::warn});

  auto listing = listKernelPolicyQueue();
  std::optional<std::vector<KernelPolicyQueueItem>> listed = co_await std::move(listing);
  EXPECT_EQ(listJson(listed), std::string("[]"));

  co_await recordKernelPolicyQueue(std::string("  acme "), std::string(" repo-one "), std::string(" /v1/responses "));
  std::string stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string(R"([{"owner":"acme","repo":"repo-one","request_count":1,"first_seen_at_ms":1700000000000,"last_seen_at_ms":1700000000000,"last_route":"/v1/responses"}])"));

  clock += 10;
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo-one"), std::string("   "));
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string(R"([{"owner":"acme","repo":"repo-one","request_count":2,"first_seen_at_ms":1700000000000,"last_seen_at_ms":1700000000010,"last_route":"/v1/responses"}])"));

  clock += 10;
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo-two"), std::string("/v1/chat/completions"));
  stored = co_await storedJson(kv);
  const std::string twoRepos =
    R"([{"owner":"acme","repo":"repo-two","request_count":1,"first_seen_at_ms":1700000000020,"last_seen_at_ms":1700000000020,"last_route":"/v1/chat/completions"},)"
    R"({"owner":"acme","repo":"repo-one","request_count":2,"first_seen_at_ms":1700000000000,"last_seen_at_ms":1700000000010,"last_route":"/v1/responses"}])";
  EXPECT_EQ(stored, twoRepos);

  co_await recordKernelPolicyQueue(std::string("   "), std::string("repo"), std::string("route"));
  co_await recordKernelPolicyQueue(std::string("acme"), std::string(""), std::string("route"));
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, twoRepos);

  rt::KvObject notAnArray;
  notAnArray.set("not", rt::KvValue("an array"));
  co_await seed(kv, rt::KvValue(std::move(notAnArray)));
  listing = listKernelPolicyQueue();
  listed = co_await std::move(listing);
  EXPECT_EQ(listJson(listed), std::string("[]"));

  // Elements of every shape: only plain objects with non-blank owner and repo survive.
  const json::value malformed = rt::jsonParse(
    R"([null,5,"x",[],{"owner":" a ","repo":"b","request_count":-3.7,"first_seen_at_ms":"x","last_seen_at_ms":12.9,"last_route":"  "},)"
    R"({"owner":"c","repo":"d","request_count":4.2,"first_seen_at_ms":"INFINITY","last_seen_at_ms":-0.5,"last_route":" r "},)"
    R"({"owner":"e","repo":" ","request_count":1},{"owner":"f","repo":"g","request_count":"7","first_seen_at_ms":5,"last_seen_at_ms":12.2,"last_route":9},)"
    R"({"owner":"h","repo":"i","request_count":"NAN","first_seen_at_ms":-0.25,"last_seen_at_ms":99,"extra":true}])");
  rt::KvValue malformedValue(malformed);
  // JSON text cannot carry Infinity or NaN: put them in place as the oracle's literal did.
  rt::KvArray& elements = malformedValue.asArray();
  elements[5].asObject().set("first_seen_at_ms", rt::KvValue(rt::Number::POSITIVE_INFINITY));
  elements[8].asObject().set("request_count", rt::KvValue(rt::Number::NaN));
  co_await seed(kv, std::move(malformedValue));
  listing = listKernelPolicyQueue();
  listed = co_await std::move(listing);
  EXPECT_EQ(listJson(listed),
            std::string(R"([{"owner":"h","repo":"i","request_count":0,"first_seen_at_ms":0,"last_seen_at_ms":99,"last_route":null},)"
                        R"({"owner":"a","repo":"b","request_count":0,"first_seen_at_ms":1700000000020,"last_seen_at_ms":12,"last_route":null},)"
                        R"({"owner":"f","repo":"g","request_count":0,"first_seen_at_ms":5,"last_seen_at_ms":12,"last_route":null},)"
                        R"({"owner":"c","repo":"d","request_count":4,"first_seen_at_ms":1700000000020,"last_seen_at_ms":0,"last_route":"r"}])"));
  // `Math.trunc(-0.25)` and `Math.trunc(-0.5)` are -0, kept as such in the item.
  EXPECT(listed && std::signbit((*listed)[0].first_seen_at_ms));
  EXPECT(listed && std::signbit((*listed)[3].last_seen_at_ms));

  clock = 50;
  co_await recordKernelPolicyQueue(std::string("f"), std::string("g"), std::string(""));
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string(R"([{"owner":"h","repo":"i","request_count":0,"first_seen_at_ms":0,"last_seen_at_ms":99,"last_route":null},)"
                                R"({"owner":"f","repo":"g","request_count":1,"first_seen_at_ms":5,"last_seen_at_ms":50,"last_route":null},)"
                                R"({"owner":"a","repo":"b","request_count":0,"first_seen_at_ms":50,"last_seen_at_ms":12,"last_route":null},)"
                                R"({"owner":"c","repo":"d","request_count":4,"first_seen_at_ms":50,"last_seen_at_ms":0,"last_route":"r"}])"));

  rt::KvArray many;
  for (int index = 0; index < 205; ++index) {
    rt::KvObject item;
    item.set("owner", rt::KvValue("o" + std::to_string(index)));
    item.set("repo", rt::KvValue("r"));
    item.set("request_count", rt::KvValue(index));
    item.set("first_seen_at_ms", rt::KvValue(index));
    item.set("last_seen_at_ms", rt::KvValue(index % 7));
    item.set("last_route", rt::KvValue(nullptr));
    many.emplace_back(std::move(item));
  }
  co_await seed(kv, rt::KvValue(std::move(many)));
  clock = 3;
  co_await recordKernelPolicyQueue(std::string("new"), std::string("repo"), std::string("route"));
  const rt::KvEntryMaybe<> cappedEntry = co_await kv->rt::ForwardingKv::get(KEY, rt::KvGetOptions{});
  const rt::KvArray& capped = cappedEntry.value.asArray();
  EXPECT_EQ(capped.size(), std::size_t{200});
  std::vector<std::string> head;
  for (std::size_t index = 0; index < 12; ++index) head.push_back(capped[index]["owner"].asString() + ":" + rt::numberToString(capped[index]["last_seen_at_ms"].asNumber()));
  EXPECT_EQ(head, (std::vector<std::string>{"o6:6", "o13:6", "o20:6", "o27:6", "o34:6", "o41:6", "o48:6", "o55:6", "o62:6", "o69:6", "o76:6", "o83:6"}));
  std::vector<std::string> tail;
  for (std::size_t index = 195; index < 200; ++index) tail.push_back(capped[index]["owner"].asString() + ":" + rt::numberToString(capped[index]["last_seen_at_ms"].asNumber()));
  EXPECT_EQ(tail, (std::vector<std::string>{"o133:0", "o140:0", "o147:0", "o154:0", "o161:0"}));
  EXPECT_EQ(drainWarnings(capture), Warnings{});

  co_await seed(kv, rt::KvValue(rt::KvArray{}));
  kv->faults->failCommits = 3;
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo"), std::string("route"));
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string("[]"));
  EXPECT_EQ(drainWarnings(capture), (Warnings{{"[ai.ubq.fi] Failed to record kernel policy queue after retries:", "acme/repo"}}));

  kv->faults->failCommits = 2;
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo"), std::string("route"));
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string(R"([{"owner":"acme","repo":"repo","request_count":1,"first_seen_at_ms":3,"last_seen_at_ms":3,"last_route":"route"}])"));
  EXPECT_EQ(drainWarnings(capture), Warnings{});

  kv->faults->throwOnGet = true;
  listing = listKernelPolicyQueue();
  listed = co_await std::move(listing);
  EXPECT_EQ(listJson(listed), std::string("null"));
  EXPECT_EQ(drainWarnings(capture), (Warnings{{"[ai.ubq.fi] Failed to list kernel policy queue:", "TypeError: get exploded"}}));
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo"), std::string("route"));
  EXPECT_EQ(drainWarnings(capture), (Warnings{{"[ai.ubq.fi] Failed to record kernel policy queue:", "TypeError: get exploded"}}));
  kv->faults->throwOnGet = false;
  stored = co_await storedJson(kv);
  EXPECT_EQ(stored, std::string(R"([{"owner":"acme","repo":"repo","request_count":1,"first_seen_at_ms":3,"last_seen_at_ms":3,"last_route":"route"}])"));

  uos::kv::setKvForTest(nullptr);
  listing = listKernelPolicyQueue();
  listed = co_await std::move(listing);
  EXPECT_EQ(listJson(listed), std::string("null"));
  co_await recordKernelPolicyQueue(std::string("acme"), std::string("repo"), std::string("route"));
  EXPECT_EQ(drainWarnings(capture), Warnings{});
}
