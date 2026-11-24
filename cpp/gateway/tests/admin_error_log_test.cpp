// Links: tests/admin_error_log_test.cpp src/admin/error_log.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp
//        src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention_schema.cpp
//        src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp
//        src/sentinel/replay_limits.cpp src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp
//        src/kv.cpp src/http.cpp src/auth/relay.cpp src/provider/cerebras_rate_limits.cpp src/storage/key_compatibility.cpp
//        src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp
// Port of tests/admin-error-log.test.ts. The TS fakes are objects with only the members a case calls (`set`, or
// `list`); here they are rt::Kv subclasses whose other members throw, as calling a missing member would.
#include "uos/admin/error_log.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::admin::error_log {
  // Test display hook (found by argument-dependent lookup).
  std::string show(const AdminErrorBucket& bucket) {
    return "{bucket_start_at_ms: " + rt::numberToString(bucket.bucket_start_at_ms) + ", count: " + rt::numberToString(bucket.count) + "}";
  }
}

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::admin::error_log;

  // An object that is not a whole Kv: every member a case does not provide throws.
  class PartialKv : public rt::Kv {
  public:
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey, rt::KvGetOptions) override { throw rt::TypeError("kv.get is not a function"); }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override {
      throw rt::TypeError("kv.getMany is not a function");
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey, rt::KvValue, rt::KvSetOptions) override { throw rt::TypeError("kv.set is not a function"); }
    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw rt::TypeError("kv.list is not a function"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override { throw rt::TypeError("kv.atomic is not a function"); }
    void close() override {}
  };

  struct Write {
    rt::KvKey key;
    rt::KvValue value;
    std::optional<double> expireIn;
  };

  class SetOnlyKv final : public PartialKv {
  public:
    std::vector<Write> writes;
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options) override {
      writes.push_back(Write{.key = std::move(key), .value = std::move(value), .expireIn = options.expireIn});
      return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = "00000000000000000001"});
    }
  };

  // `({ *[Symbol.asyncIterator]() { listCalls += 1; listOptions = options; yield* records; } })`
  struct ListLog {
    int calls = 0;
    std::optional<rt::KvListOptions> options;
  };

  class RecordsIterator final : public rt::KvListIterator {
  public:
    RecordsIterator(std::vector<rt::KvValue> records, std::shared_ptr<ListLog> log, rt::KvListOptions options)
        : records_(std::move(records)), log_(std::move(log)), options_(std::move(options)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (!started_) {
        // The generator body runs at the first `next()`.
        started_ = true;
        log_->calls += 1;
        log_->options = options_;
      }
      if (index_ >= records_.size()) co_return std::nullopt;
      rt::KvEntry<> entry;
      entry.value = records_[index_++];
      co_return entry;
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { co_return; }

  private:
    std::vector<rt::KvValue> records_;
    std::shared_ptr<ListLog> log_;
    rt::KvListOptions options_;
    bool started_ = false;
    std::size_t index_ = 0;
  };

  class ListOnlyKv final : public PartialKv {
  public:
    ListOnlyKv(std::vector<rt::KvValue> records, std::shared_ptr<ListLog> log) : records_(std::move(records)), log_(std::move(log)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions options) override {
      return std::make_unique<RecordsIterator>(records_, log_, std::move(options));
    }

  private:
    std::vector<rt::KvValue> records_;
    std::shared_ptr<ListLog> log_;
  };

  AdminErrorInput terminal(const std::function<void(AdminErrorInput&)>& overrides = nullptr) {
    AdminErrorInput input{
      .request_id = "request-1",
      .route = "responses",
      .status = 503,
      .provider = "chatgpt_codex",
      .model = "gpt-5.6-terra",
      .reasoning = "max",
      .stream = true,
      .terminal_type = "http.error",
      .failure_kind = "codex_admission_busy",
      .delivery_outcome = "delivered",
      .created_at_ms = 1'777'000'000'000,
      .latency_ms = 1'692,
      .git_sha = "fixture-sha",
      .deno_revision = "fixture-revision",
    };
    if (overrides) overrides(input);
    return input;
  }

  rt::KvValue textOrNull(const std::optional<std::string>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }

  // `{ ...value, version: 1 }`
  rt::KvValue withVersion(const AdminErrorInput& input) {
    rt::KvObject record;
    record.set("request_id", rt::KvValue(input.request_id));
    record.set("route", rt::KvValue(input.route));
    record.set("status", rt::KvValue(input.status));
    record.set("provider", rt::KvValue(input.provider));
    record.set("model", textOrNull(input.model));
    record.set("reasoning", textOrNull(input.reasoning));
    record.set("stream", input.stream ? rt::KvValue(*input.stream) : rt::KvValue(nullptr));
    record.set("terminal_type", textOrNull(input.terminal_type));
    record.set("failure_kind", textOrNull(input.failure_kind));
    record.set("delivery_outcome", rt::KvValue(input.delivery_outcome));
    record.set("created_at_ms", rt::KvValue(input.created_at_ms));
    record.set("latency_ms", rt::KvValue(input.latency_ms));
    record.set("git_sha", textOrNull(input.git_sha));
    record.set("deno_revision", textOrNull(input.deno_revision));
    record.set("version", rt::KvValue(1));
    return rt::KvValue(std::move(record));
  }

  std::vector<std::string> requestIds(const AdminErrorHistoryScan& history) {
    std::vector<std::string> ids;
    for (const rt::KvValue& record : history.data) ids.push_back(record["request_id"].asString());
    return ids;
  }

  ASYNC_TEST("admin error ledger stores every HTTP failure with bounded diagnostics") {
    const auto kv = std::make_shared<SetOnlyKv>();
    const AdminErrorInput input = terminal();
    auto recording = recordAdminError(input, std::shared_ptr<rt::Kv>(kv));
    co_await std::move(recording);

    EXPECT_EQ(kv->writes.size(), std::size_t{1});
    EXPECT_EQ(kv->writes[0].key, (rt::KvKey{ADMIN_ERROR_LOG_PREFIX, 1'777'000'000'000.0, "request-1"}));
    EXPECT_EQ(kv->writes[0].value, withVersion(terminal()));
    EXPECT(kv->writes[0].expireIn.value_or(0) > 0);
  }

  ASYNC_TEST("admin error ledger excludes successful completed requests") {
    const auto kv = std::make_shared<SetOnlyKv>();
    const AdminErrorInput input = terminal([](AdminErrorInput& value) {
      value.status = 200;
      value.terminal_type = "response.completed";
      value.failure_kind = std::nullopt;
    });
    auto recording = recordAdminError(input, std::shared_ptr<rt::Kv>(kv));
    co_await std::move(recording);
    EXPECT_EQ(kv->writes.size(), std::size_t{0});
  }

  ASYNC_TEST("admin error ledger excludes anonymous gateway 401 but retains authenticated gateway 401") {
    const auto kv = std::make_shared<SetOnlyKv>();
    auto input = terminal([](AdminErrorInput& value) {
      value.status = 401;
      value.provider = "gateway";
    });
    auto refusing = recordAdminError(input, std::shared_ptr<rt::Kv>(kv));
    co_await std::move(refusing);
    EXPECT(kv->writes.empty());
    input.authenticated = true;
    auto recording = recordAdminError(input, std::shared_ptr<rt::Kv>(kv));
    co_await std::move(recording);
    ASSERT_EQ(kv->writes.size(), std::size_t{1});
    EXPECT_EQ(kv->writes[0].value, withVersion(input));
  }

  ASYNC_TEST("admin error history hides only affected cancellations and collects analytics in one scan") {
    std::vector<rt::KvValue> records{
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "legacy-cancellation";
        value.status = 200;
        value.terminal_type = "error";
        value.failure_kind = "missing_sse_terminal";
        value.delivery_outcome = "interrupted";
        value.git_sha = "ce37210d58746a2ed3aec34c38b370f7060639e1";
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "post-fix-premature-eof";
        value.status = 200;
        value.terminal_type = "error";
        value.failure_kind = "missing_sse_terminal";
        value.delivery_outcome = "interrupted";
        value.git_sha = "post-fix-sha";
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "stream-read-error";
        value.status = 200;
        value.terminal_type = "error";
        value.failure_kind = "stream_read_error";
        value.delivery_outcome = "delivered";
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "http-error";
        value.status = 503;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS + 1;
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "older-http-error";
        value.status = 500;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS * 2 + 1;
      })),
    };
    const auto log = std::make_shared<ListLog>();
    const std::shared_ptr<rt::Kv> kv = std::make_shared<ListOnlyKv>(std::move(records), log);

    auto listing = listAdminErrorHistory(3, kv);
    const AdminErrorHistoryScan history = co_await std::move(listing);
    EXPECT_EQ(requestIds(history), (std::vector<std::string>{"post-fix-premature-eof", "stream-read-error", "http-error"}));
    EXPECT_EQ(history.five_xx_buckets, (std::vector<AdminErrorBucket>{
                                         {.bucket_start_at_ms = ADMIN_ERROR_BUCKET_MS, .count = 1},
                                         {.bucket_start_at_ms = ADMIN_ERROR_BUCKET_MS * 2, .count = 1},
                                       }));
    EXPECT_EQ(log->calls, 1);
    // `{ reverse: true, batchSize: 500 }`: nothing else is set.
    ASSERT(log->options.has_value());
    EXPECT(log->options->reverse);
    EXPECT_EQ(log->options->batchSize, std::optional<double>(500));
    EXPECT(!log->options->limit.has_value());
    EXPECT(!log->options->cursor.has_value());
    EXPECT(!log->options->consistency.has_value());
  }

  ASYNC_TEST("admin error history preserves records outside the exact legacy cancellation signature") {
    std::vector<rt::KvValue> records{
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "server-missing-terminal";
        value.status = 500;
        value.terminal_type = "error";
        value.failure_kind = "missing_sse_terminal";
        value.delivery_outcome = "interrupted";
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "delivered-missing-terminal";
        value.status = 200;
        value.terminal_type = "error";
        value.failure_kind = "missing_sse_terminal";
        value.delivery_outcome = "delivered";
      })),
    };
    const std::shared_ptr<rt::Kv> kv = std::make_shared<ListOnlyKv>(std::move(records), std::make_shared<ListLog>());

    auto listing = listAdminErrorHistory(2, kv);
    const AdminErrorHistoryScan history = co_await std::move(listing);
    EXPECT_EQ(requestIds(history), (std::vector<std::string>{"server-missing-terminal", "delivered-missing-terminal"}));
  }

  ASYNC_TEST("admin error analytics counts every inference 5xx in fifteen-minute buckets") {
    std::vector<rt::KvValue> records{
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "one";
        value.status = 500;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS + 1;
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "two";
        value.status = 503;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS + 2;
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "three";
        value.status = 429;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS + 3;
      })),
      withVersion(terminal([](AdminErrorInput& value) {
        value.request_id = "four";
        value.status = 599;
        value.created_at_ms = ADMIN_ERROR_BUCKET_MS * 2 + 1;
      })),
    };
    const std::shared_ptr<rt::Kv> kv = std::make_shared<ListOnlyKv>(std::move(records), std::make_shared<ListLog>());

    auto listing = listAdminErrorHistory(1, kv);
    const AdminErrorHistoryScan history = co_await std::move(listing);
    EXPECT_EQ(history.five_xx_buckets, (std::vector<AdminErrorBucket>{
                                         {.bucket_start_at_ms = ADMIN_ERROR_BUCKET_MS, .count = 2},
                                         {.bucket_start_at_ms = ADMIN_ERROR_BUCKET_MS * 2, .count = 1},
                                       }));
  }
}
