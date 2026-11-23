#pragma once
// The @deno/kv 0.14 SDK layer the rt stores share (BaseKv's checks, ProtoBasedKv's reads,
// commit messages and list generator, GenericKvListIterator) over the NapiInterface calls
// a native store answers, plus the native addon's request conversion (denokv_proto's
// limits, in its order). MemoryKv answers the calls from memory, SqliteKv from SQLite.
// Not a public header.
#include "kv_internal.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <variant>
#include <vector>

namespace uos::rt::kv_sdk {
  // denokv_proto::limits
  inline constexpr std::size_t MAX_WRITE_KEY_SIZE_BYTES = 2048;
  inline constexpr std::size_t MAX_READ_KEY_SIZE_BYTES = MAX_WRITE_KEY_SIZE_BYTES + 1;
  inline constexpr std::size_t MAX_VALUE_SIZE_BYTES = 65536;
  inline constexpr std::size_t MAX_READ_RANGES = 10;
  inline constexpr std::size_t MAX_READ_ENTRIES = 1000;
  inline constexpr std::size_t MAX_CHECKS = 10;
  inline constexpr std::size_t MAX_MUTATIONS = 1000;
  inline constexpr std::size_t MAX_TOTAL_MUTATION_SIZE_BYTES = 819200;
  inline constexpr double MAX_QUEUE_DELAY_MS = 30.0 * 24 * 60 * 60 * 1000;
  inline constexpr std::size_t MAX_QUEUE_UNDELIVERED_KEYS = 10;
  inline constexpr std::size_t MAX_QUEUE_BACKOFF_INTERVALS = 10;
  inline constexpr std::uint32_t MAX_QUEUE_BACKOFF_MS = 3600000;

  // `v_encoding`: how a row's value is stored.
  inline constexpr int VE_V8 = 1;
  inline constexpr int VE_LE64 = 2;
  inline constexpr int VE_BYTES = 3;

  // The backoff schedule ProtoBasedKv sends with every enqueue.
  inline constexpr std::uint32_t ENQUEUE_BACKOFF_SCHEDULE[] = {100, 200, 400, 800};

  struct ReadRange {
    KvBytes start;
    KvBytes end;
    std::size_t limit = 1;
    bool reverse = false;
  };

  // A row's value as the native store hands it to the SDK: the stored bytes and their
  // encoding (SQLite), or the value reading them yields (MemoryKv keeps decoded rows).
  struct EncodedValue {
    KvBytes data;
    int encoding = VE_V8;
  };
  struct NativeEntry {
    KvBytes key;
    std::variant<KvValue, EncodedValue> value;
    std::uint64_t version = 0;
  };
  using RangeEntries = std::vector<NativeEntry>;

  // computeKvCheckMessage: an empty versionstamp means the key must be absent.
  struct CheckMessage {
    KvBytes key;
    KvBytes versionstamp;
  };

  // computeKvMutationMessage with packKvValue.
  struct MutationMessage {
    KvBytes key;
    KvMutationType type = KvMutationType::set;
    int encoding = VE_V8;
    // The encoded value, whose size the limits measure (empty for delete).
    KvBytes data;
    // The caller's value: a store keeping decoded rows keeps kvStoredValue(value).
    KvValue value;
    // `expireAtMs`: Date.now() + expireIn at commit, 0 without expiry.
    double expireAtMs = 0;
  };

  // computeEnqueueMessage.
  struct EnqueueMessage {
    KvBytes payload;
    double deadlineMs = 0;
    std::vector<KvBytes> keysIfUndelivered;
    std::vector<std::uint32_t> backoffSchedule;
  };

  // The native addon's conversion of a snapshot read: TooManyReadRanges, KeyTooLong,
  // ReadRangeTooLarge (as Error).
  void validateSnapshotRead(const std::vector<ReadRange>& ranges);
  // The native addon's conversion of an atomic write, in its order: TooManyChecks,
  // TooManyMutations, per check KeyTooLong and InvalidVersionstamp, per mutation KeyTooLong
  // and ValueTooLong (then `afterMutation`, where MemoryKv checks arithmetic operands), per
  // enqueue ValueTooLong, TooManyQueueUndeliveredKeys, KeyTooLong,
  // TooManyQueueBackoffIntervals, QueueBackoffIntervalTooLarge and
  // InvalidMutationEnqueueDeadline (a deadline more than 30 days before `nowMs`), then
  // AtomicWriteTooLarge.
  void validateAtomicWrite(const std::vector<CheckMessage>& checks, const std::vector<MutationMessage>& mutations, const std::vector<EnqueueMessage>& enqueues,
                           double nowMs, const std::function<void(const MutationMessage&)>& afterMutation);

  // The NapiInterface calls the SDK makes for one open database.
  class Native {
  public:
    Native() = default;
    Native(const Native&) = delete;
    Native& operator=(const Native&) = delete;
    virtual ~Native() = default;
    // napi.snapshotRead: the entries of every range, read in one snapshot.
    [[nodiscard]] virtual Task<std::vector<RangeEntries>> snapshotRead(std::vector<ReadRange> ranges) = 0;
    // napi.atomicWrite: the committed version, or std::nullopt when a check failed.
    [[nodiscard]] virtual Task<std::optional<std::uint64_t>> atomicWrite(std::vector<CheckMessage> checks, std::vector<MutationMessage> mutations,
                                                                       std::vector<EnqueueMessage> enqueues) = 0;
  };

  // The state of one SDK store (BaseKv + ProtoBasedKv).
  struct Store {
    std::shared_ptr<Native> native;
    // ProtoBasedKv.encodeV8 / decodeV8.
    std::function<KvBytes(const KvValue& value)> encodeV8;
    std::function<KvValue(std::span<const std::uint8_t> bytes)> decodeV8;
    // Date.now(), read for expireIn and enqueue deadlines.
    std::function<double()> now;
    // BaseKv.closed
    bool closed = false;
    // BaseKv.checkOpen: Error "Cannot call '.<method>' after '.close' is called".
    void checkOpen(std::string_view method) const;
  };
  using StorePtr = std::shared_ptr<Store>;

  // check.js checkKeyNotEmpty: TypeError "Key cannot be empty".
  void checkKeyNotEmpty(const KvKey& key);

  // readValue(bytes, encoding, decodeV8).
  KvValue readValue(const Store& store, const NativeEntry& entry);

  // BaseKv.get + ProtoBasedKv.get_ (the caller has checked the store is open).
  Task<KvEntryMaybe<>> get(StorePtr store, KvKey key);
  // BaseKv.getMany + ProtoBasedKv.getMany_.
  Task<std::vector<KvEntryMaybe<>>> getMany(StorePtr store, std::vector<KvKey> keys);

  // `atomic().enqueue(value, opts)`'s recorded entry.
  struct Enqueue {
    KvValue value;
    KvEnqueueOptions options;
  };
  // ProtoBasedKv.commit: the messages, napi.atomicWrite, `{ ok }`.
  Task<KvCommitResult> commit(StorePtr store, std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations, std::vector<Enqueue> enqueues);

  // BaseKv.list's validation (TypeError, synchronously) and the listStream generator with
  // its CursorHolder, as a state machine that next() resumes.
  class ListStream {
  public:
    struct Selector {
      std::optional<KvKey> prefix;
      std::optional<KvKey> start;
      std::optional<KvKey> end;
    };
    ListStream(StorePtr store, Selector selector, KvListOptions options);
    // CursorHolder.get(): Error "Cannot get cursor before first iteration".
    std::string cursor() const;
    [[nodiscard]] Task<std::optional<KvEntry<>>> next();
    // generator.return()
    void finish() noexcept;

  private:
    enum class Status { initial, running, done };
    Task<void> fetchBatch();

    StorePtr store_;
    Selector selector_;
    KvListOptions options_;
    Status status_ = Status::initial;
    bool suspendedAtYield_ = false;
    std::uint64_t yielded_ = 0;
    std::optional<KvBytes> lastYieldedKeyBytes_;
    std::optional<KvBytes> prefixBytes_;
    std::optional<std::string> outCursor_;
    RangeEntries batch_;
    std::size_t index_ = 0;
    std::size_t entries_ = 0;
    std::size_t batchLimit_ = 0;
  };
  // BaseKv.list: checkOpen("list"), checkListSelector, checkListOptions.
  std::unique_ptr<ListStream> list(const StorePtr& store, const KvListSelector& selector, const KvListOptions& options);

  // GenericKvListIterator: the SDK's iterator over a ListStream.
  class GenericListIterator final : public KvListIterator {
  public:
    explicit GenericListIterator(std::unique_ptr<ListStream> stream) : stream_(std::move(stream)) {}
    Task<std::optional<KvEntry<>>> next() override;
    std::string cursor() const override;
    Task<void> return_() override;

  private:
    std::unique_ptr<ListStream> stream_;
  };
}
