// Links: tests/sentinel_replay_bootstrap_retry_test.cpp src/sentinel/replay_store.cpp src/sentinel/replay_retention.cpp
//        src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_read.cpp
//        src/sentinel/replay_retention_schema.cpp src/sentinel/replay_model.cpp src/sentinel/replay_envelope.cpp
//        src/sentinel/replay_observation.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_limits.cpp
//        src/sentinel/incident_outbox.cpp src/request.cpp src/utils.cpp src/config.cpp src/release.cpp src/kv.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
// Port of tests/sentinel-replay-bootstrap-retry.test.ts, in full (all 25 cases). Storage is `openKv(":memory:")`:
// rt::MemoryKv. `{ ...kv }` (another handle object on the same store, the shape of another process) is an
// rt::ForwardingKv; the TS wrappers that replace `list`, `get`, `atomic` or an operation's `set`/`commit` are
// ForwardingKv / ForwardingAtomicOperation subclasses. A wrapper iterator the TS builds without a `return` method has a
// return_() that does nothing, as for-await then calls nothing. `publishCapture` hands the store to
// persistEncryptedSentinelReplay as a non-owning shared_ptr to the test's handle.
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention.hpp"
#include "uos/sentinel/replay_retention_bootstrap.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/sentinel/replay_store.hpp"
#include "uos/utils.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <charconv>
#include <cstdint>
#include <deque>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace utils = uos::utils;
  using namespace uos::sentinel::replay_model;
  using namespace uos::sentinel::replay_retention_bootstrap;
  using namespace uos::sentinel::replay_retention_schema;

  using Ledger = SentinelReplayBudgetLedger;
  using MaybeLedger = std::optional<Ledger>;

  constexpr double now = 1'702'000'000'000;
  constexpr double budget = 4 * 1'024 * 1'024;

  double kvClock() { return rt::dateNow(); }

  // `{ ...ledger, ...history }`
  Ledger withHistory(Ledger ledger) {
    ledger.evicted_bytes = 17;
    ledger.evicted_records = 1;
    ledger.expired_bytes = 29;
    ledger.expired_records = 2;
    ledger.status_pruned_records = 3;
    ledger.last_eviction_at_ms = now - 3;
    ledger.last_warning_at_ms = now - 2;
    ledger.last_full_at_ms = now - 1;
    ledger.last_skip_reason = "fixture_history";
    return ledger;
  }

  const rt::KvKey& corruptKey() {
    static const rt::KvKey key{SENTINEL_REPLAY_MANIFEST_PREFIX, now + 10'000, std::string(64, 'f'), "corrupt"};
    return key;
  }

  rt::KvValue objectValue(std::initializer_list<rt::KvObject::Entry> entries) { return rt::KvValue(rt::KvObject(entries)); }
  rt::KvValue bytesValue(std::size_t length) { return rt::KvValue(rt::KvBytes(length, 0)); }
  rt::KvValue ledgerValue(const Ledger& ledger) { return rt::KvValue(json::value_from(ledger)); }

  SentinelReplayManifest manifestFor(double index) {
    char hex[8];
    const auto end = std::to_chars(hex, hex + sizeof hex, static_cast<std::uint32_t>(index), 16).ptr;
    return {
      .version = 1,
      .capture_id = "bootstrap-retry-" + rt::numberToString(index),
      .fingerprint = rt::padStart(std::string(hex, end), 64, "0"),
      .case_group_digest = std::string(64, 'c'),
      .captured_at_ms = now + index,
      .expires_at_ms = now + index + SENTINEL_REPLAY_TTL_MS,
      .algorithm = "AES-256-GCM",
      .compression = "gzip",
      .iv = utils::base64UrlEncode(rt::Bytes(12, 0)),
      .chunk_count = 1,
      .ciphertext_bytes = 100,
    };
  }

  rt::KvKey manifestKey(const SentinelReplayManifest& manifest) {
    return rt::KvKey{SENTINEL_REPLAY_MANIFEST_PREFIX, manifest.captured_at_ms, manifest.fingerprint, manifest.capture_id};
  }

  SentinelReplayAccountingRow accountingFor(const SentinelReplayManifest& manifest) {
    return {
      .version = 1,
      .capture_id = manifest.capture_id,
      .request_id = "",
      .fingerprint = manifest.fingerprint,
      .bytes = sentinelReplayManifestCharge(manifest),
      .state = "stored",
      .fence = 1,
      .created_at_ms = manifest.captured_at_ms,
      .expires_at_ms = manifest.expires_at_ms,
      .status_expires_at_ms = manifest.captured_at_ms + SENTINEL_REPLAY_STATUS_TTL_MS,
      .stage = 0,
    };
  }
  rt::KvValue accountingValue(const SentinelReplayAccountingRow& row) { return rt::KvValue(json::value_from(row)); }

  // The two chunk indexes an orphan fixture writes (a named array: a range-for over a braced list must not suspend).
  const std::vector<double> CHUNK_INDEXES{0.0, 1.0};

  rt::KvKey chunkKey(const std::string& captureId, double index = 0) { return rt::KvKey{SENTINEL_REPLAY_CHUNK_PREFIX, captureId, index}; }

  rt::Task<void> seedManifestTask(rt::Kv& kv, SentinelReplayManifest manifest, bool accounted) {
    const rt::KvKey key = manifestKey(manifest);
    const rt::KvValue value(json::value_from(manifest));
    co_await kv.set(key, value);
    const rt::KvKey chunk = chunkKey(manifest.capture_id);
    const rt::KvValue bytes = bytesValue(static_cast<std::size_t>(manifest.ciphertext_bytes));
    co_await kv.set(chunk, bytes);
    if (accounted) {
      const rt::KvKey accountingKey = sentinelReplayAccountingKey(manifest.captured_at_ms, manifest.capture_id);
      const rt::KvValue row = accountingValue(accountingFor(manifest));
      co_await kv.set(accountingKey, row);
    }
  }
  // The aggregate is copied into the coroutine here, outside the caller's co_await expression (GCC 12.2).
  rt::Task<void> seedManifest(rt::Kv& kv, const SentinelReplayManifest& manifest, bool accounted) { return seedManifestTask(kv, manifest, accounted); }

  rt::Task<Ledger> runBatch(rt::Kv& kv) {
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
    auto running = runBootstrapBatch(kv, *state.ledger, state.versionstamp, budget, now);
    Ledger ledger = co_await std::move(running);
    co_return ledger;
  }

  rt::Task<Ledger> finishSweep(rt::Kv& kv) {
    for (int pass = 0; pass < 200; pass += 1) {
      Ledger ledger = co_await runBatch(kv);
      if (!ledger.bootstrap_cursor) co_return ledger;
    }
    throw rt::JsError("bounded fixture sweep did not finish");
  }

  struct LiveCounters {
    double stored_bytes;
    double reserved_bytes;
    double records;
    double status_records;
    double metadata_bytes;
    friend bool operator==(const LiveCounters&, const LiveCounters&) = default;
  };
  std::string show(const LiveCounters& counters) {
    return "{stored_bytes: " + rt::numberToString(counters.stored_bytes) + ", reserved_bytes: " + rt::numberToString(counters.reserved_bytes) +
           ", records: " + rt::numberToString(counters.records) + ", status_records: " + rt::numberToString(counters.status_records) +
           ", metadata_bytes: " + rt::numberToString(counters.metadata_bytes) + "}";
  }
  LiveCounters liveCounters(const Ledger& ledger) {
    return {ledger.stored_bytes, ledger.reserved_bytes, ledger.records, ledger.status_records, ledger.metadata_bytes};
  }

  void assertHistory(const Ledger& ledger) {
    const Ledger expected = withHistory(ledger);
    EXPECT_EQ(ledger.evicted_bytes, expected.evicted_bytes);
    EXPECT_EQ(ledger.evicted_records, expected.evicted_records);
    EXPECT_EQ(ledger.expired_bytes, expected.expired_bytes);
    EXPECT_EQ(ledger.expired_records, expected.expired_records);
    EXPECT_EQ(ledger.status_pruned_records, expected.status_pruned_records);
    EXPECT_EQ(ledger.last_eviction_at_ms, expected.last_eviction_at_ms);
    EXPECT_EQ(ledger.last_warning_at_ms, expected.last_warning_at_ms);
    EXPECT_EQ(ledger.last_full_at_ms, expected.last_full_at_ms);
    EXPECT_EQ(ledger.last_skip_reason, expected.last_skip_reason);
  }

  std::string joined(const rt::KvKey& key, std::string_view separator) {
    std::vector<std::string> parts;
    for (const rt::KvKeyPart& part : key) parts.push_back(part.toString());
    return rt::join(parts, separator);
  }
  bool isOwnerPrefix(const rt::KvKey& prefix) {
    const std::string text = joined(prefix, "/");
    return text == joined(SENTINEL_REPLAY_ACCOUNTING_PREFIX, "/") || text == joined(SENTINEL_REPLAY_MANIFEST_PREFIX, "/");
  }
  bool isChunkPrefix(const rt::KvKey& prefix) { return joined(prefix, "/") == joined(SENTINEL_REPLAY_CHUNK_PREFIX, "/"); }
  /**
   * An owner-index slice list: an owner prefix read a whole slice at a time. The
   * sweep's own accounting and manifest pages list at most BOOTSTRAP_BATCH_ENTRIES,
   * so in fixtures whose owner prefixes hold whole slices this is only the index.
   */
  bool isOwnerIndexSlice(const rt::KvKey& prefix, const rt::KvListOptions& options) {
    return isOwnerPrefix(prefix) && options.limit && *options.limit == BOOTSTRAP_OWNER_SCAN_ENTRIES;
  }
  using ListPredicate = std::function<bool(const rt::KvKey& prefix, const rt::KvListOptions& options)>;

  struct ListReads {
    double count = 0;
    std::optional<double> largest{};
  };

  class CountedIterator final : public rt::KvListIterator {
  public:
    CountedIterator(std::unique_ptr<rt::KvListIterator> inner, ListReads& reads) : inner_(std::move(inner)), reads_(reads) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      std::optional<rt::KvEntry<>> result = co_await inner_->next();
      if (result) {
        yielded_ += 1;
        reads_.count += 1;
        reads_.largest = rt::Math::max(reads_.largest.value_or(0), yielded_);
      }
      co_return result;
    }
    std::string cursor() const override { return inner_->cursor(); }
    rt::Task<void> return_() override { co_return; }

  private:
    std::unique_ptr<rt::KvListIterator> inner_;
    ListReads& reads_;
    double yielded_ = 0;
  };

  /**
   * A separate handle on the same store (as another process would hold) that
   * counts every row a matching list actually yields (accounting/manifest rows
   * by default), and the most any one list yielded, so a test can bound the
   * reads of the real consumer.
   */
  class CountListReads : public rt::ForwardingKv {
  public:
    CountListReads(rt::Kv& kv, ListReads& reads, ListPredicate counts = nullptr) : rt::ForwardingKv(kv), reads_(reads), counts_(std::move(counts)) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      std::unique_ptr<rt::KvListIterator> iterator = inner.list(selector, options);
      const bool counted = selector.prefix && (counts_ ? counts_(*selector.prefix, options) : isOwnerPrefix(*selector.prefix));
      if (!counted) return iterator;
      return std::make_unique<CountedIterator>(std::move(iterator), reads_);
    }

  private:
    ListReads& reads_;
    ListPredicate counts_;
  };

  // `Readonly<{ reached: () => void; gate: Promise<undefined> }>`
  struct Hold {
    rt::Resolve<void> reached;
    rt::Promise<void> gate;
  };
  // The test's side of an armed hold.
  struct ArmedHold {
    rt::Promise<void> reached;
    std::function<void()> release;
    std::function<void()> disarm;
  };
  /** One armable pause point: `arm` returns the test's side of the next hold. */
  class HoldPoint {
  public:
    std::optional<Hold> take() {
      std::optional<Hold> hold = std::move(*armed_);
      armed_->reset();
      return hold;
    }
    ArmedHold arm() {
      const rt::Resolvers<void> reached = rt::withResolvers<void>();
      const rt::Resolvers<void> gate = rt::withResolvers<void>();
      *armed_ = Hold{.reached = reached.resolve, .gate = gate.promise};
      const rt::Resolve<void> open = gate.resolve;
      const rt::Promise<void> identity = gate.promise;
      const std::shared_ptr<std::optional<Hold>> armed = armed_;
      return ArmedHold{
        .reached = reached.promise,
        .release = [open] { open(); },
        .disarm =
          [armed, identity] {
            if (*armed && (*armed)->gate == identity) armed->reset();
          },
      };
    }

  private:
    std::shared_ptr<std::optional<Hold>> armed_ = std::make_shared<std::optional<Hold>>();
  };

  class HeldIterator final : public rt::KvListIterator {
  public:
    HeldIterator(rt::Kv& kv, rt::KvListSelector selector, rt::KvListOptions options, Hold hold, bool afterFirstRow, std::vector<rt::KvKey>& released)
        : kv_(kv), selector_(std::move(selector)), options_(std::move(options)), hold_(std::move(hold)), released_(released) {
      if (afterFirstRow) inner_ = kv_.list(selector_, options_);
    }
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      if (!inner_) {
        co_await wait();
        inner_ = kv_.list(selector_, options_);
      }
      std::optional<rt::KvEntry<>> result = co_await inner_->next();
      if (waiting_) co_await wait();
      if (result) released_.push_back(result->key);
      co_return result;
    }
    std::string cursor() const override { return inner_ ? inner_->cursor() : std::string(); }
    rt::Task<void> return_() override { co_return; }

  private:
    rt::Task<void> wait() {
      waiting_ = false;
      hold_.reached();
      co_await hold_.gate.wait();
    }
    rt::Kv& kv_;
    rt::KvListSelector selector_;
    rt::KvListOptions options_;
    Hold hold_;
    std::vector<rt::KvKey>& released_;
    std::unique_ptr<rt::KvListIterator> inner_;
    bool waiting_ = true;
  };

  /**
   * A handle whose next matching list, once armed, is held until released. By
   * default it is held before its first read and issued only on release, so it
   * sees every write made meanwhile, and `released` keeps the keys it then
   * yields. With `afterFirstRow` it is held once its first row has been read, so
   * that row keeps the versionstamp it had before the hold.
   */
  class HoldNextList : public rt::ForwardingKv {
  public:
    HoldNextList(rt::Kv& kv, ListPredicate holds, bool afterFirstRow = false) : rt::ForwardingKv(kv), holds_(std::move(holds)), afterFirstRow_(afterFirstRow) {}
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options = {}) override {
      std::optional<Hold> hold = selector.prefix && holds_(*selector.prefix, options) ? point_.take() : std::nullopt;
      if (!hold) return inner.list(std::move(selector), std::move(options));
      return std::make_unique<HeldIterator>(inner, std::move(selector), std::move(options), std::move(*hold), afterFirstRow_, released);
    }
    ArmedHold arm() { return point_.arm(); }
    std::vector<rt::KvKey> released;

  private:
    ListPredicate holds_;
    bool afterFirstRow_;
    HoldPoint point_;
  };

  bool isLedgerKey(const rt::KvKey& key) { return joined(key, "/") == joined(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, "/"); }

  class LedgerCommitHold final : public rt::ForwardingAtomicOperation {
  public:
    LedgerCommitHold(std::unique_ptr<rt::AtomicOperation> operation, HoldPoint& point) : rt::ForwardingAtomicOperation(std::move(operation)), point_(point) {}
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
      writesLedger_ = writesLedger_ || isLedgerKey(key);
      rt::ForwardingAtomicOperation::set(std::move(key), std::move(value), options);
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      std::optional<Hold> hold = writesLedger_ ? point_.take() : std::nullopt;
      if (hold) {
        hold->reached();
        co_await hold->gate.wait();
      }
      auto committing = rt::ForwardingAtomicOperation::commit();
      rt::KvCommitResult result = co_await std::move(committing);
      co_return result;
    }

  private:
    HoldPoint& point_;
    bool writesLedger_ = false;
  };

  /** A handle on which, once armed, the next commit that writes the budget ledger is held until released. */
  class HoldNextLedgerCommit : public rt::ForwardingKv {
  public:
    explicit HoldNextLedgerCommit(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<LedgerCommitHold>(inner.atomic(), point_); }
    ArmedHold arm() { return point_.arm(); }

  private:
    HoldPoint point_;
  };

  /** A concurrently refused admission records its skip reason: the ledger write recordSkipReason makes, which leaves the sweep in place. */
  rt::Task<void> writeSkipReason(rt::Kv& kv) {
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
    Ledger skipped = *state.ledger;
    skipped.last_skip_reason = std::string(SENTINEL_REPLAY_ACCOUNTING_REASON);
    skipped.last_warning_at_ms = now;
    const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp)).set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(skipped));
    const rt::KvCommitResult committed = co_await operation->commit();
    EXPECT(committed.ok);  // "the interposed skip reason must land"
  }

  using Step = std::function<rt::Task<void>()>;

  /**
   * A handle on which each armed step runs, in order, once a ledger read has been
   * answered and before that reader resumes, so the reader acts on the value it
   * read while the step changes the store.
   */
  class InterposeLedgerReads : public rt::ForwardingKv {
  public:
    explicit InterposeLedgerReads(rt::Kv& kv) : rt::ForwardingKv(kv) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      const bool ledger = isLedgerKey(key);
      auto reading = inner.get(std::move(key), options);
      rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (ledger) {
        // `await steps.shift()?.()`: awaiting undefined still takes a turn.
        if (steps_.empty()) {
          co_await rt::microtask();
        } else {
          const Step step = std::move(steps_.front());
          steps_.pop_front();
          const rt::Promise<void> running(step());
          co_await running.wait();
        }
      }
      co_return entry;
    }
    void arm(Step step) { steps_.push_back(std::move(step)); }

  private:
    std::deque<Step> steps_;
  };

  rt::Task<std::optional<std::string>> ledgerCursor(rt::Kv& kv) {
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
    co_return state.ledger->bootstrap_cursor;
  }

  rt::Task<bool> isAbsent(rt::Kv& kv, rt::KvKey key) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(std::move(key));
    co_return entry.value.isNull();
  }

  /** Every orphan chunk is reclaimed and every owned capture keeps its payload. */
  rt::Task<void> assertOrphansReclaimedTask(rt::Kv& kv, std::vector<std::string> orphans, std::vector<SentinelReplayManifest> owned) {
    for (const std::string& orphan : orphans) {
      for (const double index : CHUNK_INDEXES) {
        const bool absent = co_await isAbsent(kv, chunkKey(orphan, index));
        if (!absent) rt::test::report(__FILE__, __LINE__, orphan + "/" + rt::numberToString(index) + " is reclaimed", false);
      }
    }
    for (const SentinelReplayManifest& manifest : owned) {
      const bool absent = co_await isAbsent(kv, chunkKey(manifest.capture_id));
      if (absent) rt::test::report(__FILE__, __LINE__, manifest.capture_id + " keeps its payload", false);
    }
  }
  rt::Task<void> assertOrphansReclaimed(rt::Kv& kv, const std::vector<std::string>& orphans, const std::vector<SentinelReplayManifest>& owned) {
    return assertOrphansReclaimedTask(kv, orphans, owned);
  }

  /** Stored-charge manifests keep a large owner fixture far inside the test budget. */
  std::vector<SentinelReplayManifest> ownedManifests(double count) {
    std::vector<SentinelReplayManifest> manifests;
    for (double index = 0; index < count; index += 1) {
      SentinelReplayManifest manifest = manifestFor(index + 1);
      manifest.stored_bytes = 1'024;
      manifests.push_back(std::move(manifest));
    }
    return manifests;
  }

  // C++ only. A TS test returns while a background owner fill may still run on one of its handles, which the
  // fill's closure keeps alive; it later stops (or fails on the closed store) unobserved. Here the handles are
  // locals of the test, so such a test lets the fill reach that point before it returns: fake time advances
  // only when no store operation is pending, and a fill yields 1 ms between slices.
  rt::Task<void> settleBackgroundFills() { co_await rt::sleep(1'000); }

  const rt::Regex& JUDGED_CHUNKS() {
    static const rt::Regex pattern("^1\\|.", "");
    return pattern;
  }
  bool judged(const std::optional<std::string>& cursor) { return JUDGED_CHUNKS().test(cursor.value_or("")); }
}

ASYNC_TEST("bootstrap retry: orphan chunks are reclaimed instead of being ignored") {
  rt::MemoryKv kv(kvClock);
  const rt::KvKey orphanKey{SENTINEL_REPLAY_CHUNK_PREFIX, "orphan-bootstrap", 0};
  const rt::KvValue orphan(rt::KvBytes{1, 2, 3});
  co_await kv.set(orphanKey, orphan);
  const Ledger ledger = co_await finishSweep(kv);
  EXPECT_EQ(ledger.bootstrap_complete, true);
  EXPECT_EQ(ledger.stored_bytes, 0.0);
  const bool absent = co_await isAbsent(kv, orphanKey);
  EXPECT(absent);
  kv.close();
}

namespace {
  rt::Task<void> chargesRebuildOnce(bool accounted) {
    rt::MemoryKv kv(kvClock);
    const SentinelReplayManifest manifest = manifestFor(1);
    SentinelReplayAccountingRow reserved = accountingFor(manifestFor(2));
    reserved.state = "reserved";
    reserved.bytes = 200;
    json::object status;
    status["version"] = 1;
    status["request_id"] = "retry-status";
    status["status"] = "disabled";
    status["reason"] = "fixture";
    status["captured_at_ms"] = rt::makeNumber(now);
    status["manifest_key"] = nullptr;
    status["fingerprint"] = nullptr;
    status["expires_at_ms"] = nullptr;
    json::object tombstone;
    tombstone["fingerprint"] = std::string(64, 'e');
    tombstone["reason"] = "fixture";
    tombstone["expires_at_ms"] = rt::makeNumber(now + SENTINEL_REPLAY_STATUS_TTL_MS);
    const LiveCounters expected{
      .stored_bytes = sentinelReplayManifestCharge(manifest),
      .reserved_bytes = reserved.bytes,
      .records = 1,
      .status_records = 2,
      .metadata_bytes = sentinelReplayStatusMetadataBytes(json::value(status)) + sentinelReplayStatusMetadataBytes(json::value(tombstone)),
    };
    const LedgerState initial = co_await readLedger(kv, budget);
    if (initial.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
    const rt::KvValue historyLedger = ledgerValue(withHistory(*initial.ledger));
    co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, historyLedger);
    co_await seedManifest(kv, manifest, accounted);
    const rt::KvKey reservedKey = sentinelReplayAccountingKey(reserved.created_at_ms, reserved.capture_id);
    const rt::KvValue reservedValue = accountingValue(reserved);
    co_await kv.set(reservedKey, reservedValue);
    const rt::KvKey statusKey{SENTINEL_REPLAY_REQUEST_PREFIX, "retry-status"};
    const rt::KvValue statusValue(status);
    co_await kv.set(statusKey, statusValue);
    const rt::KvKey tombstoneKey{SENTINEL_REPLAY_EVICTION_PREFIX, std::string(64, 'e')};
    const rt::KvValue tombstoneValue(tombstone);
    co_await kv.set(tombstoneKey, tombstoneValue);
    const rt::KvValue corrupt = objectValue({{"invalid", rt::KvValue(true)}});
    co_await kv.set(corruptKey(), corrupt);
    for (int attempt = 0; attempt < 3; attempt += 1) {
      const Ledger failed = co_await finishSweep(kv);
      EXPECT_EQ(failed.bootstrap_complete, false);
      EXPECT_EQ(failed.bootstrap_cursor, std::nullopt);
      EXPECT_EQ(failed.accounting_error, std::optional<std::string>("invalid_in_scope_row"));
      EXPECT_EQ(liveCounters(failed), expected);
      assertHistory(failed);
    }
    const rt::KvEntryMaybe<> materialized = co_await kv.get(sentinelReplayAccountingKey(manifest.captured_at_ms, manifest.capture_id));
    EXPECT(materialized.value == accountingValue(accountingFor(manifest)));
    co_await kv.delete_(corruptKey());
    const Ledger recovered = co_await finishSweep(kv);
    EXPECT_EQ(recovered.bootstrap_complete, true);
    EXPECT_EQ(recovered.accounting_error, std::nullopt);
    EXPECT_EQ(recovered.over_budget, false);
    EXPECT_EQ(liveCounters(recovered), expected);
    assertHistory(recovered);
    kv.close();
  }
}

ASYNC_TEST("bootstrap retry: materialized legacy charges rebuild once and preserve history") { co_await chargesRebuildOnce(false); }

ASYNC_TEST("bootstrap retry: existing charges rebuild once and preserve history") { co_await chargesRebuildOnce(true); }

ASYNC_TEST("bootstrap retry: a restarted sweep preserves bounded cursor continuation") {
  rt::MemoryKv kv(kvClock);
  std::vector<SentinelReplayManifest> manifests;
  for (double index = 0; index < BOOTSTRAP_BATCH_ENTRIES + 7; index += 1) manifests.push_back(manifestFor(index + 1));
  double expectedBytes = 0;
  for (const SentinelReplayManifest& manifest : manifests) expectedBytes += sentinelReplayManifestCharge(manifest);
  for (const SentinelReplayManifest& manifest : manifests) co_await seedManifest(kv, manifest, false);
  const rt::KvValue corrupt = objectValue({{"invalid", rt::KvValue(true)}});
  co_await kv.set(corruptKey(), corrupt);
  const Ledger failed = co_await finishSweep(kv);
  EXPECT_EQ(failed.bootstrap_complete, false);
  EXPECT_EQ(failed.records, static_cast<double>(manifests.size()));
  const Ledger partial = co_await runBatch(kv);
  EXPECT_EQ(partial.bootstrap_complete, false);
  EXPECT(partial.bootstrap_cursor.has_value());
  EXPECT_EQ(partial.records, BOOTSTRAP_BATCH_ENTRIES);
  const Ledger repeated = co_await finishSweep(kv);
  EXPECT_EQ(repeated.bootstrap_complete, false);
  EXPECT_EQ(repeated.records, static_cast<double>(manifests.size()));
  EXPECT_EQ(repeated.stored_bytes, expectedBytes);
  co_await kv.delete_(corruptKey());
  const Ledger recovered = co_await finishSweep(kv);
  EXPECT_EQ(recovered.bootstrap_complete, true);
  EXPECT_EQ(recovered.records, static_cast<double>(manifests.size()));
  EXPECT_EQ(recovered.stored_bytes, expectedBytes);
  kv.close();
}

ASYNC_TEST("bootstrap retry: stale ledger CAS cannot commit a rebuilt snapshot") {
  rt::MemoryKv kv(kvClock);
  const SentinelReplayManifest manifest = manifestFor(1);
  co_await seedManifest(kv, manifest, false);
  const rt::KvValue corrupt = objectValue({{"invalid", rt::KvValue(true)}});
  co_await kv.set(corruptKey(), corrupt);
  co_await finishSweep(kv);
  co_await kv.delete_(corruptKey());
  const LedgerState stale = co_await readLedger(kv, budget);
  if (stale.kind != LedgerStateKind::ok) throw rt::JsError("expected a persisted failed sweep");
  Ledger concurrent = *stale.ledger;
  concurrent.expired_records = stale.ledger->expired_records + 1;
  const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
  operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, stale.versionstamp)).set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(concurrent));
  const rt::KvCommitResult committed = co_await operation->commit();
  if (!committed.ok) throw rt::JsError("fixture CAS unexpectedly failed");
  auto running = runBootstrapBatch(kv, *stale.ledger, stale.versionstamp, budget, now);
  const Ledger conflict = co_await std::move(running);
  EXPECT(conflict == concurrent);
  const rt::KvEntryMaybe<> persisted = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT(persisted.value == ledgerValue(concurrent));
  EXPECT_EQ(persisted.versionstamp, std::optional<std::string>(committed.versionstamp));
  const Ledger recovered = co_await finishSweep(kv);
  EXPECT_EQ(recovered.bootstrap_complete, true);
  EXPECT_EQ(recovered.expired_records, concurrent.expired_records);
  EXPECT_EQ(recovered.stored_bytes, sentinelReplayManifestCharge(manifest));
  EXPECT_EQ(recovered.records, 1.0);
  kv.close();
}

ASYNC_TEST("bootstrap retry: chunk owner lookups stay bounded per list and linear overall") {
  rt::MemoryKv kv(kvClock);
  ListReads reads{.count = 0, .largest = 0};
  CountListReads gateway(kv, reads);
  // Half the owners are legacy manifests the sweep materializes later; the rest are already accounted.
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  // A capture still staging: its reserved accounting row is its only owner.
  SentinelReplayAccountingRow live = accountingFor(manifestFor(static_cast<double>(owned.size()) + 1));
  live.capture_id = "live-reserved";
  live.state = "reserved";
  live.bytes = 2'048;
  const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c"};
  const double ownerRows = static_cast<double>(owned.size()) + rt::Math::ceil(static_cast<double>(owned.size()) / 2) + 1;
  for (std::size_t index = 0; index < owned.size(); ++index) co_await seedManifest(kv, owned[index], index % 2 == 0);
  const rt::KvKey liveKey = sentinelReplayAccountingKey(live.created_at_ms, live.capture_id);
  const rt::KvValue liveValue = accountingValue(live);
  co_await kv.set(liveKey, liveValue);
  const rt::KvValue liveChunk = bytesValue(64);
  co_await kv.set(chunkKey(live.capture_id), liveChunk);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  EXPECT(ownerRows > 2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);  // "the fixture must hold more owner rows than two owner index slices"
  MaybeLedger ledger;
  for (int call = 0; call < 1'000 && !ledger; call += 1) ledger = co_await bootstrapBeforeMutation(gateway, budget, now);
  if (!ledger) throw rt::JsError("bounded bootstrap did not finish");
  // one owner list read `reads.largest` rows; the bound is BOOTSTRAP_OWNER_SCAN_ENTRIES
  EXPECT(reads.largest.value_or(0) <= BOOTSTRAP_OWNER_SCAN_ENTRIES);
  // A lone caller reads each owner row once in its own sweep phase and once for the chunk owner index, never once per chunk.
  EXPECT(reads.count <= 2 * ownerRows);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) {
      const bool absent = co_await isAbsent(kv, chunkKey(orphan, index));
      EXPECT(absent);  // `${orphan}/${index} is reclaimed`
    }
  }
  for (const SentinelReplayManifest& manifest : owned) {
    const bool absent = co_await isAbsent(kv, chunkKey(manifest.capture_id));
    EXPECT(!absent);  // `${manifest.capture_id} keeps its payload`
  }
  const bool liveAbsent = co_await isAbsent(kv, chunkKey(live.capture_id));
  EXPECT(!liveAbsent);  // "a live reservation keeps its staged chunk"
  EXPECT_EQ(liveCounters(*ledger), (LiveCounters{.stored_bytes = static_cast<double>(owned.size()) * 1'024,
                                                 .reserved_bytes = live.bytes,
                                                 .records = static_cast<double>(owned.size()),
                                                 .status_records = 0,
                                                 .metadata_bytes = 0}));
  const uos::sentinel::replay_retention::SentinelReplayCapacityInput input{
    .capture_id = "after-bootstrap",
    .request_id = "after-bootstrap",
    .plaintext_bytes = 128,
    .metadata_bytes = 64,
    .fingerprint = std::string(64, 'a'),
    .now_ms = now,
    .budget_bytes = budget,
  };
  auto admitting = uos::sentinel::replay_retention::reserveSentinelReplayCapacity(kv, input);
  const SentinelReplayAdmission admitted = co_await std::move(admitting);
  EXPECT(std::holds_alternative<SentinelReplayAdmissionOk>(admitted));  // "admissions resume once the bounded sweep completes"
  kv.close();
}

ASYNC_TEST("bootstrap retry: lost ledger commits keep the chunk owner index, so a contended sweep finishes") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads counting(kv, reads, isOwnerIndexSlice);
  InterposeLedgerReads handle(counting);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(3 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c", "orphan-d"};
  const double ownerRows = 2 * static_cast<double>(owned.size());
  const double sweptRows = ownerRows + static_cast<double>(owned.size()) + 2 * static_cast<double>(orphans.size());
  // A lost commit may cost its batch the page it scanned, never the owners already indexed.
  const double callBound = 2 * (rt::Math::ceil(sweptRows / BOOTSTRAP_BATCH_ENTRIES) + rt::Math::ceil(ownerRows / BOOTSTRAP_OWNER_SCAN_ENTRIES) + 8);
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  EXPECT(ownerRows > 2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);  // "the owner index must need more slices than one lost commit can cost"
  MaybeLedger ledger;
  double calls = 0;
  for (; !ledger && calls < callBound; calls += 1) {
    // Every other request loses its commit: a refused admission records its skip reason after the batch reads the ledger.
    if (rt::mod(calls, 2) == 0) {
      Step skip = [&kv]() { return writeSkipReason(kv); };
      handle.arm(std::move(skip));
    }
    ledger = co_await bootstrapBeforeMutation(handle, budget, now);
  }
  EXPECT(ledger && ledger->bootstrap_complete);  // `the contended sweep finished within ${callBound} bootstrap calls`
  EXPECT(reads.count <= ownerRows);  // `the owner index read ${reads.count} rows for ${ownerRows} owners across lost commits`
  co_await assertOrphansReclaimed(kv, orphans, owned);
  if (ledger) {
    EXPECT_EQ(liveCounters(*ledger), (LiveCounters{.stored_bytes = static_cast<double>(owned.size()) * 1'024,
                                                   .reserved_bytes = 0,
                                                   .records = static_cast<double>(owned.size()),
                                                   .status_records = 0,
                                                   .metadata_bytes = 0}));
  }
  kv.close();
}

ASYNC_TEST("bootstrap retry: a process that restarts every few calls still finishes the sweep") {
  rt::MemoryKv kv(kvClock);
  // Legacy manifests only, so the owner index needs more slices than one handle lives calls.
  const std::vector<SentinelReplayManifest> owned = ownedManifests(200);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b"};
  const double restartEvery = 3;
  // An uninterrupted sweep scans every chunk and every manifest once, a few entries per call.
  const double callBound = 2 * rt::Math::ceil((2 * static_cast<double>(owned.size()) + static_cast<double>(orphans.size())) / BOOTSTRAP_BATCH_ENTRIES);
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, false);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) co_await kv.set(chunkKey(orphan), orphanChunk);
  EXPECT(rt::Math::ceil(static_cast<double>(owned.size()) / BOOTSTRAP_OWNER_SCAN_ENTRIES) > restartEvery);
  MaybeLedger ledger;
  // A replaced handle stays alive, as the TS fill's closure keeps it, so its fill may still run.
  std::vector<std::unique_ptr<rt::ForwardingKv>> handles;
  rt::Kv* handle = &kv;
  for (double call = 0; call < callBound && !ledger; call += 1) {
    // A fresh handle holds none of the last one's in-memory state: a restarted process.
    if (rt::mod(call, restartEvery) == 0) {
      handles.push_back(std::make_unique<rt::ForwardingKv>(kv));
      handle = handles.back().get();
    }
    ledger = co_await bootstrapBeforeMutation(*handle, budget, now);
  }
  EXPECT(ledger && ledger->bootstrap_complete);  // `the sweep finished within ${callBound} calls across restarts`
  if (ledger) {
    EXPECT_EQ(ledger->accounting_error, std::nullopt);
    EXPECT_EQ(ledger->records, static_cast<double>(owned.size()));
  }
  co_await assertOrphansReclaimed(kv, orphans, owned);
  kv.close();
  co_await settleBackgroundFills();
}

ASYNC_TEST("bootstrap retry: a replacement handle finishes while the previous process's owner fill is still held") {
  rt::MemoryKv kv(kvClock);
  ListReads oldReads;
  ListReads newReads;
  CountListReads oldCounting(kv, oldReads, isOwnerIndexSlice);
  HoldNextList pausing(oldCounting, isOwnerIndexSlice);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b"};
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, false);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) co_await kv.set(chunkKey(orphan), orphanChunk);
  const ArmedHold hold = pausing.arm();
  const rt::Promise<MaybeLedger> first(bootstrapBeforeMutation(pausing, budget, now, 0));
  co_await hold.reached.wait();
  const MaybeLedger firstLedger = co_await first.wait();
  EXPECT(!firstLedger);
  const std::optional<std::string> cursor = co_await ledgerCursor(kv);
  EXPECT_EQ(cursor, std::optional<std::string>("1|"));
  EXPECT_EQ(oldReads.count, 0.0);  // "the old process has not finished even its first slice"
  // Do not release the old fill: the restarted process must build an independent index.
  rt::ForwardingKv restarted(kv);
  CountListReads replacement(restarted, newReads, isOwnerIndexSlice);
  const Ledger ledger = co_await finishSweep(replacement);
  EXPECT_EQ(ledger.bootstrap_complete, true);
  EXPECT_EQ(ledger.accounting_error, std::nullopt);
  EXPECT_EQ(ledger.records, static_cast<double>(owned.size()));
  EXPECT_EQ(newReads.count, static_cast<double>(owned.size()));  // "the replacement filled its own index"
  EXPECT_EQ(oldReads.count, 0.0);  // "the previous process's fill is still unfinished"
  co_await assertOrphansReclaimed(kv, orphans, owned);
  forgetChunkOwnerIndex(pausing);
  hold.release();
  kv.close();
  co_await settleBackgroundFills();
}

namespace {
  rt::Task<void> fillStopsWhenSweepLeft(bool complete) {
    rt::MemoryKv kv(kvClock);
    ListReads reads;
    CountListReads counting(kv, reads, isOwnerIndexSlice);
    InterposeLedgerReads interposing(counting);
    HoldNextList pausing(interposing, isOwnerIndexSlice);
    const std::vector<SentinelReplayManifest> owned = ownedManifests(3 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
    for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, false);
    const ArmedHold hold = pausing.arm();
    rt::ForwardingKv other(kv);
    const rt::Promise<MaybeLedger> pending(bootstrapBeforeMutation(pausing, budget, now));
    co_await hold.reached.wait();
    if (complete) {
      const Ledger finished = co_await finishSweep(other);
      EXPECT_EQ(finished.bootstrap_complete, true);
    } else {
      const LedgerState state = co_await readLedger(kv, budget);
      EXPECT(state.kind == LedgerStateKind::ok);
      Ledger replaced = *state.ledger;
      replaced.bootstrap_sweep = std::string("replacement-sweep");
      const rt::KvValue replacedValue = ledgerValue(replaced);
      co_await kv.set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, replacedValue);
    }
    const rt::Resolvers<void> rechecked = rt::withResolvers<void>();
    const rt::Resolve<void> recheck = rechecked.resolve;
    Step observe = [recheck]() -> rt::Task<void> {
      recheck();
      return rt::fulfilledTask();
    };
    interposing.arm(std::move(observe));
    hold.release();
    const rt::Promise<bool> recheckedFirst = rechecked.promise.then([] { return true; });
    const rt::Promise<bool> timedOut = rt::Promise<void>(rt::sleep(1'000)).then([] { return false; });
    std::vector<rt::Promise<bool>> racers{recheckedFirst, timedOut};
    const bool checked = co_await rt::race(std::move(racers)).wait();
    EXPECT_EQ(checked, true);  // "the fill rechecks the ledger without another request on this handle"
    co_await pending.wait();
    EXPECT_EQ(reads.count, BOOTSTRAP_OWNER_SCAN_ENTRIES);  // "only the in-flight slice was read, not the rest of the obsolete index"
    forgetChunkOwnerIndex(pausing);
    hold.release();
    kv.close();
  }
}

ASYNC_TEST("bootstrap retry: the background owner fill stops when another process leaves the sweep complete") { co_await fillStopsWhenSweepLeft(true); }

ASYNC_TEST("bootstrap retry: the background owner fill stops when another process leaves the sweep replaced") { co_await fillStopsWhenSweepLeft(false); }

ASYNC_TEST("bootstrap retry: the background owner fill yields between slices") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads counting(kv, reads, isOwnerIndexSlice);
  HoldNextList pausing(counting, isOwnerIndexSlice);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(3 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, false);
  const ArmedHold hold = pausing.arm();
  const rt::Promise<MaybeLedger> pending(bootstrapBeforeMutation(pausing, budget, now));
  co_await hold.reached.wait();
  ListReads* const observed = &reads;
  const rt::Promise<double> nextTurn = rt::Promise<void>(rt::sleep(0)).then([observed] { return observed->count; });
  hold.release();
  const double atNextTurn = co_await nextTurn.wait();
  EXPECT(atNextTurn <= BOOTSTRAP_OWNER_SCAN_ENTRIES);  // "other event-loop work runs before the second slice"
  co_await pending.wait();
  EXPECT_EQ(reads.count, static_cast<double>(owned.size()));  // "the paced fill still completes without further requests"
  forgetChunkOwnerIndex(pausing);
  hold.release();
  kv.close();
}

ASYNC_TEST("bootstrap retry: the owner index fills without further requests, and a request waits for it only briefly without writing the ledger") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads counting(kv, reads);
  HoldNextList handle(counting, isOwnerIndexSlice);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(3 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b"};
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, false);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) co_await kv.set(chunkKey(orphan), orphanChunk);
  // The first request starts the sweep, which reaches the chunk phase at once (there are no accounting
  // rows) and needs the owner index, whose first slice is held.
  const ArmedHold hold = handle.arm();
  const rt::Promise<MaybeLedger> request(bootstrapBeforeMutation(handle, budget, now));
  co_await hold.reached.wait();
  // The sweep's id is durable before the request waits on the fill, so a concurrently refused admission's
  // skip reason cannot cost the fill its sweep.
  const std::optional<std::string> stamped = co_await ledgerCursor(kv);
  EXPECT_EQ(stamped, std::optional<std::string>("1|"));  // "the sweep's id was committed before the wait"
  co_await writeSkipReason(kv);
  const rt::Promise<std::string> returned = request.then([](const MaybeLedger&) { return std::string("returned"); });
  const rt::Promise<std::string> stillWaiting = rt::Promise<void>(rt::sleep(5'000)).then([] { return std::string("still waiting"); });
  std::vector<rt::Promise<std::string>> racers{returned, stillWaiting};
  const std::string outcome = co_await rt::race(std::move(racers)).wait();
  EXPECT_EQ(outcome, std::string("returned"));  // "a request does not wait for a slow owner index fill"
  const MaybeLedger requested = co_await request.wait();
  EXPECT(!requested);
  const std::optional<std::string> waitingCursor = co_await ledgerCursor(kv);
  EXPECT_EQ(waitingCursor, std::optional<std::string>("1|"));  // "the sweep waits at its chunk phase"
  // A request that comes while the fill is still held judges no chunk and leaves the ledger alone.
  const rt::KvEntryMaybe<> before = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  const MaybeLedger whileHeld = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(!whileHeld);
  const rt::KvEntryMaybe<> after = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  EXPECT_EQ(after.versionstamp, before.versionstamp);  // "a request that judged no chunk leaves the ledger alone"
  hold.release();
  // No further request is made, yet the fill reads every owner row.
  for (const double deadline = rt::dateNow() + 5'000; reads.count < static_cast<double>(owned.size()) && rt::dateNow() < deadline;) co_await rt::sleep(5);
  EXPECT_EQ(reads.count, static_cast<double>(owned.size()));  // "the owner index filled without further requests"
  // The next request judges chunks at once.
  const MaybeLedger judging = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(!judging);
  const std::optional<std::string> judgedCursor = co_await ledgerCursor(kv);
  EXPECT(judged(judgedCursor));  // "the next request judged chunks with the filled index"
  MaybeLedger ledger;
  for (int call = 0; call < 1'000 && !ledger; call += 1) ledger = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(ledger && ledger->bootstrap_complete);
  co_await assertOrphansReclaimed(kv, orphans, owned);
  hold.release();
  kv.close();
}

ASYNC_TEST("bootstrap retry: concurrent requests share one owner index fill") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads gateway(kv, reads, isOwnerIndexSlice);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(3 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c", "orphan-d"};
  const double ownerRows = 2 * static_cast<double>(owned.size());
  const int concurrency = 4;
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  // Another handle brings the sweep to its chunk phase, so this handle starts with no owner index.
  for (int call = 0; call < 1'000; call += 1) {
    const std::optional<std::string> cursor = co_await ledgerCursor(kv);
    if (rt::startsWith(cursor.value_or(""), "1|")) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(kv, budget, now);
    EXPECT(!result);
  }
  const std::optional<std::string> phase = co_await ledgerCursor(kv);
  EXPECT(rt::startsWith(phase.value_or(""), "1|"));
  MaybeLedger ledger;
  for (int round = 0; round < 1'000 && !ledger; round += 1) {
    std::vector<rt::Promise<MaybeLedger>> requests;
    for (int index = 0; index < concurrency; index += 1) requests.push_back(rt::Promise<MaybeLedger>(bootstrapBeforeMutation(gateway, budget, now)));
    const std::vector<MaybeLedger> results = co_await rt::all(std::move(requests)).wait();
    for (const MaybeLedger& result : results) {
      if (result) {
        ledger = result;
        break;
      }
    }
  }
  EXPECT(ledger && ledger->bootstrap_complete);
  if (ledger) EXPECT_EQ(ledger->accounting_error, std::nullopt);
  EXPECT(reads.count <= ownerRows);  // `${concurrency} concurrent requests read ${reads.count} rows for an index of ${ownerRows} owners`
  co_await assertOrphansReclaimed(kv, orphans, owned);
  kv.close();
}

ASYNC_TEST("bootstrap retry: a batch left behind by the chunk phase starts no second owner index fill") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads counting(kv, reads, isOwnerIndexSlice);
  // One handle for every caller: the shape of one process with concurrent requests.
  InterposeLedgerReads handle(counting);
  // A last owned capture, sorting after every other chunk, leaves the final chunk page partial and ending in a live chunk.
  std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  SentinelReplayManifest last = manifestFor(9'999);
  last.capture_id = "zz-last";
  last.stored_bytes = 1'024;
  owned.push_back(last);
  rt::Kv& store = kv;
  const auto chunksLeft = [&store]() -> rt::Task<double> {
    const std::optional<std::string> cursor = co_await ledgerCursor(store);
    const std::string text = cursor.value_or("");
    if (!rt::startsWith(text, "1|")) co_return std::numeric_limits<double>::infinity();
    const std::string inner = rt::slice(text, 2);
    rt::KvListSelector selector;
    selector.prefix = SENTINEL_REPLAY_CHUNK_PREFIX;
    rt::KvListOptions options;
    if (!inner.empty()) options.cursor = inner;
    const std::unique_ptr<rt::KvListIterator> iterator = store.list(std::move(selector), std::move(options));
    const std::vector<rt::KvEntry<>> entries = co_await rt::kvListToArray(*iterator);
    co_return static_cast<double>(entries.size());
  };
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  // Run until the next batch judges the last chunk page and moves past the chunk phase.
  for (int call = 0; call < 1'000; call += 1) {
    auto measuring = rt::async(chunksLeft);
    const double remaining = co_await std::move(measuring);
    if (remaining < BOOTSTRAP_BATCH_ENTRIES) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
    EXPECT(!result);
  }
  auto measuring = rt::async(chunksLeft);
  const double left = co_await std::move(measuring);
  EXPECT(left > 0 && left < BOOTSTRAP_BATCH_ENTRIES);  // `the next page holds the last ${left} chunks`
  // Batch X reads the ledger; batch Y then judges that page and commits past the chunk phase before X resumes.
  InterposeLedgerReads* const shared = &handle;
  Step batchY = [shared]() -> rt::Task<void> {
    return rt::async([shared]() -> rt::Task<void> {
      const MaybeLedger result = co_await bootstrapBeforeMutation(*shared, budget, now);
      EXPECT(!result);
    });
  };
  handle.arm(std::move(batchY));
  reads.count = 0;
  const MaybeLedger x = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(!x);
  const std::optional<std::string> moved = co_await ledgerCursor(kv);
  EXPECT(!rt::startsWith(moved.value_or(""), "1|"));  // "Y moved the sweep past its chunk phase"
  EXPECT_EQ(reads.count, 0.0);  // "X, left at a chunk cursor, does not index the owners again"
  MaybeLedger ledger;
  for (int call = 0; call < 1'000 && !ledger; call += 1) ledger = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(ledger && ledger->bootstrap_complete);
  if (ledger) EXPECT_EQ(ledger->records, static_cast<double>(owned.size()));
  const std::vector<std::string> noOrphans;
  co_await assertOrphansReclaimed(kv, noOrphans, owned);
  kv.close();
}

ASYNC_TEST("bootstrap retry: a lost commit does not rewind the shared owner index, so the next batch reuses it") {
  rt::MemoryKv kv(kvClock);
  ListReads reads;
  CountListReads counting(kv, reads);
  // One handle for every caller: the shape of one process with concurrent requests.
  InterposeLedgerReads handle(counting);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  // Run until a committed batch has judged chunks, so the complete owner index is shared on this handle.
  for (int call = 0; call < 1'000; call += 1) {
    const std::optional<std::string> cursor = co_await ledgerCursor(kv);
    if (judged(cursor)) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
    EXPECT(!result);
  }
  const std::optional<std::string> start = co_await ledgerCursor(kv);
  EXPECT(judged(start));
  // Batch X reads the ledger, and a refused admission then records its skip reason, so X's commit loses. X
  // rereads the ledger, still at the same cursor, and batch Y commits the next page before X resumes.
  auto y = std::make_shared<std::optional<MaybeLedger>>();
  Step skip = [&kv]() { return writeSkipReason(kv); };
  InterposeLedgerReads* const shared = &handle;
  Step batchY = [shared, y]() -> rt::Task<void> {
    return rt::async([shared, y]() -> rt::Task<void> {
      MaybeLedger result = co_await bootstrapBeforeMutation(*shared, budget, now);
      *y = std::move(result);
    });
  };
  handle.arm(std::move(skip));
  handle.arm(std::move(batchY));
  const MaybeLedger x = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(!x);
  EXPECT(y->has_value() && !y->value());  // `y` is null
  const std::optional<std::string> moved = co_await ledgerCursor(kv);
  EXPECT(moved != start);  // "Y committed the next page"
  reads.count = 0;
  const MaybeLedger next = co_await bootstrapBeforeMutation(handle, budget, now);
  EXPECT(!next);
  const std::optional<std::string> after = co_await ledgerCursor(kv);
  EXPECT(after != moved);  // "the next batch judged its page"
  EXPECT_EQ(reads.count, 0.0);  // "the next batch reused the shared owner index instead of rebuilding it"
  kv.close();
}

ASYNC_TEST("bootstrap retry: a slow batch judging chunks shares the complete owner index, so concurrent batches still advance (bootstrapBeforeMutation)") {
  rt::MemoryKv kv(kvClock);
  HoldNextList handle(kv, [](const rt::KvKey& prefix, const rt::KvListOptions&) { return isChunkPrefix(prefix); });
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c", "orphan-d"};
  // Every round must commit at least one chunk page or later sweep batch.
  const double roundBound = rt::Math::ceil((static_cast<double>(owned.size()) + 2 * static_cast<double>(orphans.size())) / BOOTSTRAP_BATCH_ENTRIES) +
                            rt::Math::ceil(static_cast<double>(owned.size()) / BOOTSTRAP_BATCH_ENTRIES) + 4;
  /** One request through the real consumer; true once accounting is authoritative. */
  HoldNextList* const shared = &handle;
  const auto request = [shared]() -> rt::Task<bool> {
    const MaybeLedger result = co_await bootstrapBeforeMutation(*shared, budget, now);
    co_return result.has_value();
  };
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  // Run until a committed batch has judged chunks, so the complete owner index is held for the durable cursor.
  for (int call = 0; call < 1'000; call += 1) {
    const std::optional<std::string> cursor = co_await ledgerCursor(kv);
    if (judged(cursor)) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
    EXPECT(!result);
  }
  const std::optional<std::string> judgedCursor = co_await ledgerCursor(kv);
  EXPECT(judged(judgedCursor));
  bool finished = false;
  for (double round = 0; round < roundBound && !finished; round += 1) {
    const std::optional<std::string> before = co_await ledgerCursor(kv);
    // Request X reads the ledger first, takes the owner index and is held at its chunk page.
    const ArmedHold hold = handle.arm();
    const rt::Promise<bool> x(rt::async(request));
    const rt::Promise<void> xSettled = x.then([](const bool&) {});
    std::vector<rt::Promise<void>> racers{hold.reached, xSettled};
    co_await rt::race(std::move(racers)).wait();
    hold.disarm();
    // Request Y reads the same ledger and commits first; X then loses its commit.
    auto second = rt::async(request);
    const bool y = co_await std::move(second);
    hold.release();
    const bool xFinished = co_await x.wait();
    finished = xFinished || y;
    const std::optional<std::string> afterRound = co_await ledgerCursor(kv);
    EXPECT(afterRound != before);  // `round ${round} must move the durable sweep from ${before}`
  }
  EXPECT(finished);  // `the contended sweep finished within ${roundBound} rounds`
  const LedgerState state = co_await readLedger(kv, budget);
  if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
  EXPECT_EQ(state.ledger->bootstrap_complete, true);
  EXPECT_EQ(state.ledger->accounting_error, std::nullopt);
  EXPECT_EQ(state.ledger->stored_bytes, static_cast<double>(owned.size()) * 1'024);
  EXPECT_EQ(state.ledger->records, static_cast<double>(owned.size()));
  co_await assertOrphansReclaimed(kv, orphans, owned);
  kv.close();
}

ASYNC_TEST("bootstrap retry: a chunk another batch sharing the owner index reclaimed first is not an invalid row") {
  rt::MemoryKv kv(kvClock);
  HoldNextLedgerCommit committing(kv);
  HoldNextList reading(committing, [](const rt::KvKey& prefix, const rt::KvListOptions&) { return isChunkPrefix(prefix); }, true);
  // One handle for every caller: the shape of one process with concurrent requests.
  HoldNextList& handle = reading;
  const std::vector<SentinelReplayManifest> owned = ownedManifests(BOOTSTRAP_BATCH_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b"};
  rt::Kv& store = kv;
  const auto nextChunkOwner = [&store]() -> rt::Task<std::optional<std::string>> {
    const std::optional<std::string> cursor = co_await ledgerCursor(store);
    if (!judged(cursor)) co_return std::nullopt;
    rt::KvListSelector selector;
    selector.prefix = SENTINEL_REPLAY_CHUNK_PREFIX;
    rt::KvListOptions options;
    options.cursor = rt::slice(*cursor, 2);
    options.limit = 1;
    const std::unique_ptr<rt::KvListIterator> iterator = store.list(std::move(selector), std::move(options));
    const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
    if (entry) {
      co_await iterator->return_();
      co_return entry->key[4].toString();
    }
    co_return std::nullopt;
  };
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  // Run until a committed batch has judged chunks and the next chunk page starts at an orphan.
  for (int call = 0; call < 100; call += 1) {
    auto probing = rt::async(nextChunkOwner);
    const std::optional<std::string> owner = co_await std::move(probing);
    if (owner == std::optional<std::string>("orphan-a")) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
    EXPECT(!result);
  }
  auto probing = rt::async(nextChunkOwner);
  const std::optional<std::string> firstOwner = co_await std::move(probing);
  EXPECT_EQ(firstOwner, std::optional<std::string>("orphan-a"));
  // Batch X has read the orphan's row when it is held.
  const ArmedHold read = reading.arm();
  const rt::Promise<MaybeLedger> x(bootstrapBeforeMutation(handle, budget, now));
  co_await read.reached.wait();
  // Batch Y judges the same page from the same ledger, reclaims every orphan, and is held before its ledger commit.
  const ArmedHold commit = committing.arm();
  const rt::Promise<MaybeLedger> y(bootstrapBeforeMutation(handle, budget, now));
  co_await commit.reached.wait();
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) {
      const bool absent = co_await isAbsent(kv, chunkKey(orphan, index));
      EXPECT(absent);  // `Y reclaimed ${orphan}/${index}`
    }
  }
  // X's delete of the reclaimed row now fails while its ledger read is still current, so X commits first.
  read.release();
  const MaybeLedger xLedger = co_await x.wait();
  EXPECT(!xLedger);
  const LedgerState afterX = co_await readLedger(kv, budget);
  if (afterX.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
  EXPECT_EQ(afterX.ledger->accounting_error, std::nullopt);  // "a chunk already reclaimed is gone, not an invalid in-scope row"
  commit.release();
  const MaybeLedger yLedger = co_await y.wait();
  EXPECT(!yLedger);
  MaybeLedger ledger;
  for (int call = 0; call < 100 && !ledger; call += 1) {
    ledger = co_await bootstrapBeforeMutation(handle, budget, now);
    if (!ledger) {
      const std::optional<std::string> cursor = co_await ledgerCursor(kv);
      EXPECT(cursor.has_value());  // "the sweep never restarts from the beginning"
    }
  }
  if (!ledger) throw rt::JsError("bootstrap did not finish");
  EXPECT_EQ(ledger->bootstrap_complete, true);
  EXPECT_EQ(ledger->accounting_error, std::nullopt);
  EXPECT_EQ(ledger->records, static_cast<double>(owned.size()));
  co_await assertOrphansReclaimed(kv, orphans, owned);
  kv.close();
}

namespace {
  namespace replay_retention = uos::sentinel::replay_retention;
  using uos::sentinel::replay_observation::PersistDependencies;
  using uos::sentinel::replay_observation::SentinelReplayPersistResult;
  using uos::sentinel::replay_observation::SentinelReplayPersistResultStored;

  /** A deterministic, incompressible capture body that spans several stored chunks. */
  std::shared_ptr<rt::Bytes> multiChunkBody() {
    rt::Bytes noise(96 * 1'024);
    // `state` is a JS int32 updated by `^=`, `<<` and `>>>`: the same bits as uint32 arithmetic.
    std::uint32_t state = 0x2545f491;
    for (std::uint8_t& byte : noise) {
      state ^= state << 13;
      state ^= state >> 17;
      state ^= state << 5;
      byte = static_cast<std::uint8_t>(state & 0xff);
    }
    json::object body;
    body["model"] = "gpt-5.6-sol";
    body["blob"] = utils::base64UrlEncode(noise);
    return std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(body)))));
  }

  SentinelFailureObservation captureFailure() {
    return {
      .status = 502,
      .stream = false,
      .completed = false,
      .terminal_type = "http.error",
      .failure_kind = "upstream_timeout",
      .synthetic_terminal_type = std::nullopt,
      .provider_route = "chatgpt_codex",
    };
  }

  const rt::Bytes& replayKeyBytes() {
    static const rt::Bytes key(32, 3);
    return key;
  }

  /** A capture request whose body, and so its dedupe fingerprint, is distinct per request id unless given. */
  AcceptedSentinelReplayInput captureInput(const std::string& requestId, std::shared_ptr<rt::Bytes> body = nullptr) {
    if (!body) {
      json::object fallback;
      fallback["model"] = "gpt-5.6-sol";
      fallback["request"] = json::string(requestId);
      body = std::make_shared<rt::Bytes>(rt::encodeUtf8(rt::jsonStringify(json::value(std::move(fallback)))));
    }
    return {
      .endpoint = "/v1/responses",
      .method = "POST",
      .body = std::move(body),
      .content_type = "application/json",
      .compatibility_headers = {},
      .request_id = requestId,
      .git_sha = std::string(40, 'a'),
      .deno_revision = "bootstrap-retry",
    };
  }

  /** Admit, stage and publish one capture through the real store, as `kv`. */
  rt::Task<SentinelReplayPersistResult> publishCaptureTask(rt::Kv& kv, std::string captureId, AcceptedSentinelReplayInput input) {
    const PersistDependencies dependencies{
      .kv = std::shared_ptr<rt::Kv>(&kv, [](rt::Kv*) {}),
      .keyBytes = replayKeyBytes(),
      .now = [] { return now; },
      .randomUuid = [captureId] { return captureId; },
      .randomBytes = [](double length) { return rt::Bytes(static_cast<std::size_t>(length), 7); },
      .budgetBytes = budget,
    };
    const SentinelFailureObservation failure = captureFailure();
    auto persisting = uos::sentinel::replay_store::persistEncryptedSentinelReplay(input, failure, dependencies);
    SentinelReplayPersistResult result = co_await std::move(persisting);
    co_return result;
  }
  rt::Task<SentinelReplayPersistResult> publishCapture(rt::Kv& kv, const std::string& captureId, const AcceptedSentinelReplayInput& input) {
    return publishCaptureTask(kv, captureId, input);
  }

  bool isStored(const SentinelReplayPersistResult& result) { return std::holds_alternative<SentinelReplayPersistResultStored>(result); }

  /** Reserved accounting rows for captures that have staged nothing yet: owners without chunks. */
  std::vector<SentinelReplayAccountingRow> stagingOwners(double count) {
    std::vector<SentinelReplayAccountingRow> rows;
    for (double index = 0; index < count; index += 1) {
      SentinelReplayAccountingRow row = accountingFor(manifestFor(10'000 + index));
      row.capture_id = "staging-" + rt::numberToString(index);
      row.state = "reserved";
      row.bytes = 1'024;
      rows.push_back(std::move(row));
    }
    return rows;
  }
}

ASYNC_TEST("bootstrap retry: a recovered legacy charge expires and releases exactly once") {
  rt::MemoryKv kv(kvClock);
  const SentinelReplayManifest manifest = manifestFor(1);
  const double charge = sentinelReplayManifestCharge(manifest);
  const rt::KvKey accountingKey = sentinelReplayAccountingKey(manifest.captured_at_ms, manifest.capture_id);
  co_await seedManifest(kv, manifest, false);
  const rt::KvValue invalid = objectValue({{"invalid", rt::KvValue(true)}});
  co_await kv.set(corruptKey(), invalid);
  co_await finishSweep(kv);
  co_await finishSweep(kv);
  co_await kv.delete_(corruptKey());
  const Ledger recovered = co_await finishSweep(kv);
  EXPECT_EQ(recovered.stored_bytes, charge);
  EXPECT_EQ(recovered.records, 1.0);
  for (int attempt = 0; attempt < 2; attempt += 1) {
    const replay_retention::SentinelReplayReclaimOptions options{.now_ms = manifest.expires_at_ms + 1, .budget_bytes = budget};
    auto reclaiming = replay_retention::reclaimExpiredSentinelReplays(kv, options);
    co_await std::move(reclaiming);
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind != LedgerStateKind::ok) throw rt::JsError("expected a persisted reclamation ledger");
    const Ledger& reclaimed = *state.ledger;
    EXPECT_EQ(reclaimed.stored_bytes, 0.0);
    EXPECT_EQ(reclaimed.reserved_bytes, 0.0);
    EXPECT_EQ(reclaimed.records, 0.0);
    EXPECT_EQ(reclaimed.expired_bytes, charge);
    EXPECT_EQ(reclaimed.expired_records, 1.0);
    const bool accountingGone = co_await isAbsent(kv, accountingKey);
    EXPECT(accountingGone);
    const bool manifestGone = co_await isAbsent(kv, manifestKey(manifest));
    EXPECT(manifestGone);
    const bool chunkGone = co_await isAbsent(kv, chunkKey(manifest.capture_id));
    EXPECT(chunkGone);
  }
  kv.close();
}

ASYNC_TEST("bootstrap retry: an owner index never serves another sweep, even one reset to the same cursor") {
  rt::MemoryKv kv(kvClock);
  // Two handles on one store: the shape of two processes.
  rt::ForwardingKv first(kv);
  rt::ForwardingKv second(kv);
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  // As many captures as one batch scans, so their accounting rows leave the reset sweep's chunk pages where the first sweep's were.
  std::vector<std::string> admitted;
  for (double index = 0; index < BOOTSTRAP_BATCH_ENTRIES; index += 1) admitted.push_back("zz-admitted-" + rt::numberToString(index));
  rt::Kv* store = &kv;
  const auto sweepUntil = [store](rt::Kv* handle, std::function<bool(const std::optional<std::string>&)> reached) -> rt::Task<void> {
    for (int call = 0; call < 1'000; call += 1) {
      const std::optional<std::string> cursor = co_await ledgerCursor(*store);
      if (reached(cursor)) break;
      const MaybeLedger result = co_await bootstrapBeforeMutation(*handle, budget, now);
      EXPECT(!result);
    }
  };
  const auto finish = [](rt::Kv* handle) -> rt::Task<MaybeLedger> {
    MaybeLedger ledger;
    for (int call = 0; call < 1'000 && !ledger; call += 1) ledger = co_await bootstrapBeforeMutation(*handle, budget, now);
    co_return ledger;
  };
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  // The first handle indexes every owner and judges its first chunk page.
  const std::function<bool(const std::optional<std::string>&)> judgedPage = [](const std::optional<std::string>& cursor) { return judged(cursor); };
  auto sweepingFirst = sweepUntil(&first, judgedPage);
  co_await std::move(sweepingFirst);
  const std::optional<std::string> held = co_await ledgerCursor(kv);
  EXPECT(judged(held));
  // The second handle finishes that sweep, and captures are then admitted, staged and published.
  auto finishingSecond = finish(&second);
  const MaybeLedger completed = co_await std::move(finishingSecond);
  EXPECT(completed && completed->bootstrap_complete);
  for (const std::string& captureId : admitted) {
    const AcceptedSentinelReplayInput input = captureInput(captureId);
    auto publishing = publishCapture(second, captureId, input);
    const SentinelReplayPersistResult result = co_await std::move(publishing);
    EXPECT(isStored(result));
  }
  // The ledger is reset outside the normal path, and a new sweep reaches the cursor the first handle last judged at.
  co_await kv.delete_(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
  const std::function<bool(const std::optional<std::string>&)> atHeld = [held](const std::optional<std::string>& cursor) { return cursor == held; };
  auto sweepingSecond = sweepUntil(&second, atHeld);
  co_await std::move(sweepingSecond);
  const std::optional<std::string> reset = co_await ledgerCursor(kv);
  EXPECT_EQ(reset, held);  // "the reset sweep reached the same chunk cursor"
  // The first handle resumes the new sweep: the owners it indexed for the earlier one must not judge these chunks.
  auto finishingFirst = finish(&first);
  const MaybeLedger ledger = co_await std::move(finishingFirst);
  ASSERT(ledger.has_value());
  EXPECT_EQ(ledger->bootstrap_complete, true);
  EXPECT_EQ(ledger->accounting_error, std::nullopt);
  EXPECT_EQ(ledger->records, static_cast<double>(owned.size() + admitted.size()));
  for (const std::string& captureId : admitted) {
    const bool absent = co_await isAbsent(kv, chunkKey(captureId));
    EXPECT(!absent);  // `${captureId}, admitted after the earlier sweep, keeps its payload`
  }
  for (const SentinelReplayManifest& manifest : owned) {
    const bool absent = co_await isAbsent(kv, chunkKey(manifest.capture_id));
    EXPECT(!absent);  // `${manifest.capture_id} keeps its payload`
  }
  kv.close();
  co_await settleBackgroundFills();
}

namespace {
  rt::Task<void> batchOutlivesSweepCompletion(bool sameHandle) {
    rt::MemoryKv kv(kvClock);
    HoldNextList pausing(kv, [](const rt::KvKey& prefix, const rt::KvListOptions&) { return isChunkPrefix(prefix); });
    HoldNextList& handle = pausing;
    // The same handle is one process with concurrent requests; a second handle is another process.
    rt::ForwardingKv secondHandle(kv);
    rt::Kv& other = sameHandle ? static_cast<rt::Kv&>(handle) : static_cast<rt::Kv&>(secondHandle);
    const std::vector<SentinelReplayManifest> owned = ownedManifests(2);
    // Owners still staging fill the accounting prefix to one whole slice, so the owner index needs a second
    // slice for the manifests, and the accounting phase ends on a batch boundary.
    const std::vector<SentinelReplayAccountingRow> staging = stagingOwners(BOOTSTRAP_OWNER_SCAN_ENTRIES - static_cast<double>(owned.size()));
    const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c", "orphan-d"};
    const std::string requestId = "bootstrap-straddle";
    const std::string liveCaptureId = "zz-live-capture";
    const std::shared_ptr<rt::Bytes> body = multiChunkBody();
    for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
    for (const SentinelReplayAccountingRow& row : staging) {
      const rt::KvValue value = accountingValue(row);
      co_await kv.set(sentinelReplayAccountingKey(row.created_at_ms, row.capture_id), value);
    }
    const rt::KvValue orphanChunk = bytesValue(32);
    for (const std::string& orphan : orphans) co_await kv.set(chunkKey(orphan), orphanChunk);
    // Run until a committed batch has judged chunks, so the next batch judges with the complete owner index.
    for (int call = 0; call < 100; call += 1) {
      const std::optional<std::string> cursor = co_await ledgerCursor(kv);
      if (judged(cursor)) break;
      const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
      EXPECT(!result);
    }
    const std::optional<std::string> judgedCursor = co_await ledgerCursor(kv);
    EXPECT(judged(judgedCursor));
    // Batch A has read the incomplete ledger and is held at its next chunk page.
    const ArmedHold hold = pausing.arm();
    const rt::Promise<MaybeLedger> outlived(bootstrapBeforeMutation(handle, budget, now));
    co_await hold.reached.wait();
    // Meanwhile another request finishes the sweep, and a capture is admitted, staged and published.
    MaybeLedger completed;
    for (int call = 0; call < 100 && !completed; call += 1) completed = co_await bootstrapBeforeMutation(other, budget, now);
    EXPECT(completed && completed->bootstrap_complete);
    const AcceptedSentinelReplayInput input = captureInput(requestId, body);
    auto publishing = publishCapture(other, liveCaptureId, input);
    const SentinelReplayPersistResult stored = co_await std::move(publishing);
    ASSERT(isStored(stored));
    EXPECT(std::get<SentinelReplayPersistResultStored>(stored).manifest.chunk_count > 1);  // "the live capture spans several chunks"
    hold.release();
    const MaybeLedger outlivedLedger = co_await outlived.wait();
    EXPECT(outlivedLedger && outlivedLedger->bootstrap_complete);
    bool judgedLive = false;
    for (const rt::KvKey& key : pausing.released) judgedLive = judgedLive || (key.size() > 4 && key[4].isString() && key[4].string() == liveCaptureId);
    EXPECT(judgedLive);  // "the outlived batch judged the published capture's chunks against its pre-admission owner index"
    auto exporting = uos::sentinel::replay_read::listEncryptedSentinelReplaysByRequestId(kv, requestId, now);
    const uos::sentinel::replay_read::EncryptedSentinelRequestReplays exported = co_await std::move(exporting);
    ASSERT_EQ(exported.captures.size(), std::size_t{1});  // "the published capture stays exportable"
    auto decrypting = uos::sentinel::replay_read::decryptExportedSentinelReplay(exported.captures[0], replayKeyBytes());
    const SentinelReplayPlaintext plaintext = co_await std::move(decrypting);
    EXPECT(*plaintext.body == *body);  // "the published capture keeps its whole payload"
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
    EXPECT_EQ(state.ledger->bootstrap_complete, true);
    EXPECT_EQ(state.ledger->accounting_error, std::nullopt);  // "a refused delete never leaves an invalid-row error behind"
    EXPECT_EQ(state.ledger->records, static_cast<double>(owned.size()) + 1);
    for (const std::string& orphan : orphans) {
      const bool absent = co_await isAbsent(kv, chunkKey(orphan));
      EXPECT(absent);  // `${orphan} is reclaimed`
    }
    kv.close();
    co_await settleBackgroundFills();
  }
}

ASYNC_TEST("bootstrap retry: a batch that outlives sweep completion keeps a capture published meanwhile (finished on the same handle)") {
  auto running = batchOutlivesSweepCompletion(true);
  co_await std::move(running);
}

ASYNC_TEST("bootstrap retry: a batch that outlives sweep completion keeps a capture published meanwhile (finished on a second handle)") {
  auto running = batchOutlivesSweepCompletion(false);
  co_await std::move(running);
}

namespace {
  // `Promise.race([pending, sleep(200).then(() => "waited")])`: 0 false, 1 true, 2 "waited".
  rt::Task<int> raceAgainstWait(rt::Promise<bool> pending) {
    const rt::Promise<int> answered = pending.then([](const bool& value) { return value ? 1 : 0; });
    const rt::Promise<int> waited = rt::Promise<void>(rt::sleep(200)).then([] { return 2; });
    std::vector<rt::Promise<int>> racers{answered, waited};
    auto racing = rt::race(std::move(racers));
    const int result = co_await racing.wait();
    co_return result;
  }

  rt::Task<void> admissionStartsOwnerFillWithoutWaiting(bool status) {
    rt::MemoryKv kv(kvClock);
    // Reservation reaping also lists accounting rows in pages of 64 before bootstrap starts.
    // Hold the manifest slice so the pause can only belong to the background owner fill.
    HoldNextList pausing(kv, [](const rt::KvKey& prefix, const rt::KvListOptions& options) {
      return joined(prefix, "/") == joined(SENTINEL_REPLAY_MANIFEST_PREFIX, "/") && isOwnerIndexSlice(prefix, options);
    });
    HoldNextList* const handle = &pausing;
    const auto request = [handle, status]() -> rt::Task<bool> {
      if (status) {
        rt::KvObject row;
        row.set("version", rt::KvValue(1));
        row.set("request_id", rt::KvValue("no-wait"));
        row.set("status", rt::KvValue("skipped"));
        row.set("reason", rt::KvValue(SENTINEL_REPLAY_ACCOUNTING_REASON));
        row.set("captured_at_ms", rt::KvValue(now));
        row.set("manifest_key", rt::KvValue(nullptr));
        row.set("fingerprint", rt::KvValue(nullptr));
        row.set("expires_at_ms", rt::KvValue(nullptr));
        const replay_retention::SentinelReplayStatusMetadataInput input{
          .key = rt::KvKey{SENTINEL_REPLAY_REQUEST_PREFIX, "no-wait"},
          .row = rt::KvValue(std::move(row)),
          .now_ms = now,
          .ttl_ms = SENTINEL_REPLAY_STATUS_TTL_MS,
          .budget_bytes = budget,
        };
        auto admitting = replay_retention::admitSentinelReplayStatusMetadata(*handle, input);
        const bool admitted = co_await std::move(admitting);
        co_return admitted;
      }
      const replay_retention::SentinelReplayCapacityInput input{
        .capture_id = "no-wait",
        .request_id = "no-wait",
        .plaintext_bytes = 128,
        .metadata_bytes = 64,
        .fingerprint = std::string(64, 'a'),
        .now_ms = now,
        .budget_bytes = budget,
      };
      auto reserving = replay_retention::reserveSentinelReplayCapacity(*handle, input);
      const SentinelReplayAdmission admission = co_await std::move(reserving);
      co_return std::holds_alternative<SentinelReplayAdmissionOk>(admission);
    };
    co_await seedManifest(kv, manifestFor(1), false);
    const ArmedHold hold = pausing.arm();
    const rt::Promise<bool> first(rt::async(request));
    co_await hold.reached.wait();
    const std::optional<std::string> fillCursor = co_await ledgerCursor(kv);
    EXPECT_EQ(fillCursor, std::optional<std::string>("1|"));  // "the held list belongs to the owner fill after its sweep id is durable"
    // Both the caller starting the fill and later callers must return before the maintenance wait expires.
    std::vector<rt::Promise<bool>> pending;
    pending.push_back(first);
    pending.push_back(rt::Promise<bool>(rt::async(request)));
    pending.push_back(rt::Promise<bool>(rt::async(request)));
    for (const rt::Promise<bool>& each : pending) {
      auto racing = raceAgainstWait(each);
      const int result = co_await std::move(racing);
      EXPECT_EQ(result, 0);  // "inference-path admission refuses promptly while its owner fill is held"
    }
    const std::optional<std::string> afterCursor = co_await ledgerCursor(kv);
    EXPECT_EQ(afterCursor, std::optional<std::string>("1|"));  // "no chunk was judged while the fill was held"
    const bool chunkAbsent = co_await isAbsent(kv, chunkKey(manifestFor(1).capture_id));
    EXPECT(!chunkAbsent);
    const bool statusAbsent = co_await isAbsent(kv, rt::KvKey{SENTINEL_REPLAY_REQUEST_PREFIX, "no-wait"});
    EXPECT(statusAbsent);
    forgetChunkOwnerIndex(pausing);
    hold.release();
    kv.close();
    co_await settleBackgroundFills();
  }
}

ASYNC_TEST("bootstrap retry: reservation admission starts the owner fill without waiting, including concurrent refusals") {
  auto running = admissionStartsOwnerFillWithoutWaiting(false);
  co_await std::move(running);
}

ASYNC_TEST("bootstrap retry: status admission starts the owner fill without waiting, including concurrent refusals") {
  auto running = admissionStartsOwnerFillWithoutWaiting(true);
  co_await std::move(running);
}

ASYNC_TEST("bootstrap retry: a slow batch judging chunks shares the complete owner index, so concurrent batches still advance (reserveSentinelReplayCapacity)") {
  rt::MemoryKv kv(kvClock);
  HoldNextList handle(kv, [](const rt::KvKey& prefix, const rt::KvListOptions&) { return isChunkPrefix(prefix); });
  const std::vector<SentinelReplayManifest> owned = ownedManifests(2 * BOOTSTRAP_OWNER_SCAN_ENTRIES);
  const std::vector<std::string> orphans{"orphan-a", "orphan-b", "orphan-c", "orphan-d"};
  // Every round must commit at least one chunk page or later sweep batch.
  const double roundBound = rt::Math::ceil((static_cast<double>(owned.size()) + 2 * static_cast<double>(orphans.size())) / BOOTSTRAP_BATCH_ENTRIES) +
                            rt::Math::ceil(static_cast<double>(owned.size()) / BOOTSTRAP_BATCH_ENTRIES) + 4;
  auto admissions = std::make_shared<double>(0);
  /** One request through the real consumer; true once accounting is authoritative. */
  HoldNextList* const shared = &handle;
  const auto request = [shared, admissions]() -> rt::Task<bool> {
    *admissions += 1;
    const std::string captureId = "admission-" + rt::numberToString(*admissions);
    const replay_retention::SentinelReplayCapacityInput input{
      .capture_id = captureId,
      .request_id = captureId,
      .plaintext_bytes = 128,
      .metadata_bytes = 64,
      .fingerprint = std::string(64, 'a'),
      .now_ms = now,
      .budget_bytes = budget,
    };
    auto reserving = replay_retention::reserveSentinelReplayCapacity(*shared, input);
    const SentinelReplayAdmission admission = co_await std::move(reserving);
    co_return std::holds_alternative<SentinelReplayAdmissionOk>(admission);
  };
  for (const SentinelReplayManifest& manifest : owned) co_await seedManifest(kv, manifest, true);
  const rt::KvValue orphanChunk = bytesValue(32);
  for (const std::string& orphan : orphans) {
    for (const double index : CHUNK_INDEXES) co_await kv.set(chunkKey(orphan, index), orphanChunk);
  }
  // Run until a committed batch has judged chunks, so the complete owner index is held for the durable cursor.
  for (int call = 0; call < 1'000; call += 1) {
    const std::optional<std::string> cursor = co_await ledgerCursor(kv);
    if (judged(cursor)) break;
    const MaybeLedger result = co_await bootstrapBeforeMutation(handle, budget, now);
    EXPECT(!result);
  }
  const std::optional<std::string> judgedCursor = co_await ledgerCursor(kv);
  EXPECT(judged(judgedCursor));
  bool finished = false;
  for (double round = 0; round < roundBound && !finished; round += 1) {
    const std::optional<std::string> before = co_await ledgerCursor(kv);
    // Request X reads the ledger first, takes the owner index and is held at its chunk page.
    const ArmedHold hold = handle.arm();
    const rt::Promise<bool> x(rt::async(request));
    const rt::Promise<void> xSettled = x.then([](const bool&) {});
    std::vector<rt::Promise<void>> racers{hold.reached, xSettled};
    co_await rt::race(std::move(racers)).wait();
    hold.disarm();
    // Request Y reads the same ledger and commits first; X then loses its commit.
    auto second = rt::async(request);
    const bool y = co_await std::move(second);
    hold.release();
    const bool xFinished = co_await x.wait();
    finished = xFinished || y;
    const std::optional<std::string> afterRound = co_await ledgerCursor(kv);
    EXPECT(afterRound != before);  // `round ${round} must move the durable sweep from ${before}`
  }
  EXPECT(finished);  // `the contended sweep finished within ${roundBound} rounds`
  const LedgerState state = co_await readLedger(kv, budget);
  if (state.kind == LedgerStateKind::corrupt) throw rt::JsError("unexpected corrupt fixture ledger");
  EXPECT_EQ(state.ledger->bootstrap_complete, true);
  EXPECT_EQ(state.ledger->accounting_error, std::nullopt);
  EXPECT_EQ(state.ledger->stored_bytes, static_cast<double>(owned.size()) * 1'024);
  EXPECT_EQ(state.ledger->records, static_cast<double>(owned.size()));
  co_await assertOrphansReclaimed(kv, orphans, owned);
  kv.close();
}
