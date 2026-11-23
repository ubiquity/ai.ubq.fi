// The @deno/kv 0.14 SDK layer shared by MemoryKv and SqliteKv (kv_sdk.hpp): check.js,
// kv_util.js (BaseKv, readValue, packKvValue, the list checks, CursorHolder,
// GenericKvListIterator), proto_based.js (get_, getMany_, commit, listStream) and the
// native addon's request conversion (denokv_proto convert.rs).
#include "kv_sdk.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace uos::rt::kv_sdk {
  namespace {
    bool startsWith(const KvBytes& bytes, const KvBytes& prefix) { return bytes.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), bytes.begin()); }

    // packKvu: 8 little-endian bytes.
    KvBytes packKvu(KvU64 value) {
      KvBytes out(8);
      for (std::size_t index = 0; index < 8; ++index) out[index] = static_cast<std::uint8_t>(value.value >> (8 * index));
      return out;
    }

    // unpackKvu: Error with an empty message unless exactly 8 bytes.
    KvU64 unpackKvu(std::span<const std::uint8_t> bytes) {
      if (bytes.size() != 8) throw JsError("");
      std::uint64_t value = 0;
      for (std::size_t index = 0; index < 8; ++index) value |= static_cast<std::uint64_t>(bytes[index]) << (8 * index);
      return KvU64(value);
    }

    // packKvValue: KvU64 -> LE64, Uint8Array -> BYTES, anything else -> encodeV8.
    void packKvValue(MutationMessage& message, const Store& store, const KvValue& value) {
      if (value.isU64()) {
        message.encoding = VE_LE64;
        message.data = packKvu(value.asU64());
      } else if (value.isBytes()) {
        message.encoding = VE_BYTES;
        message.data = value.asBytes();
      } else if (const auto* view = std::get_if<KvArrayBufferView>(&value.variant()); view && view->kind == KvViewKind::Uint8Array) {
        message.encoding = VE_BYTES;
        if (view->byteOffset > view->buffer.size() || view->byteLength > view->buffer.size() - view->byteOffset)
          throw RangeError("KV value has an ArrayBuffer view outside its buffer");
        const auto first = view->buffer.begin() + static_cast<std::ptrdiff_t>(view->byteOffset);
        message.data.assign(first, first + static_cast<std::ptrdiff_t>(view->byteLength));
      } else {
        message.encoding = VE_V8;
        message.data = store.encodeV8(value);
      }
    }

    // computeReadRangeForKey
    ReadRange readRangeForKey(const KvBytes& packedKey) { return ReadRange{packedKey, KvBytes{0xFF}, 1, false}; }

    void checkKeyInPrefixKeyspace(const KvKey& key, const KvKey& prefix, std::string_view which) {
      const KvBytes keyBytes = packKey(key);
      const KvBytes prefixBytes = packKey(prefix);
      if (!(keyBytes.size() > prefixBytes.size() && startsWith(keyBytes, prefixBytes)))
        throw TypeError(std::string(which) + " key is not in the keyspace defined by prefix");
    }

    ListStream::Selector checkListSelector(const KvListSelector& selector) {
      if (selector.prefix) {
        if (selector.start && selector.end) throw TypeError("Selector can not specify both 'start' and 'end' key when specifying 'prefix'");
        if (selector.start) {
          checkKeyInPrefixKeyspace(*selector.start, *selector.prefix, "Start");
          return ListStream::Selector{selector.prefix, selector.start, std::nullopt};
        }
        if (selector.end) {
          checkKeyInPrefixKeyspace(*selector.end, *selector.prefix, "End");
          return ListStream::Selector{selector.prefix, std::nullopt, selector.end};
        }
        return ListStream::Selector{selector.prefix, std::nullopt, std::nullopt};
      }
      if (selector.start && selector.end) {
        if (packKey(*selector.start) > packKey(*selector.end)) throw TypeError("Start key is greater than end key");
        return ListStream::Selector{std::nullopt, selector.start, selector.end};
      }
      throw TypeError("Selector must specify either 'prefix' or both 'start' and 'end' key");
    }

    void checkListOptions(const KvListOptions& options) {
      if (options.limit && !(*options.limit > 0 && Number::isSafeInteger(*options.limit))) throw TypeError("Bad 'limit': " + numberToString(*options.limit));
      if (options.batchSize && !(*options.batchSize > 0 && Number::isSafeInteger(*options.batchSize) && *options.batchSize <= 1000))
        throw TypeError("Bad 'batchSize': " + numberToString(*options.batchSize));
    }
  }

  // ---- Request conversion (convert.rs) -------------------------------------------------------

  void validateSnapshotRead(const std::vector<ReadRange>& ranges) {
    if (ranges.size() > MAX_READ_RANGES) throw JsError("TooManyReadRanges");
    std::size_t totalLimit = 0;
    for (const ReadRange& range : ranges) {
      if (range.limit == 0 || range.limit > 0x7FFFFFFF) throw JsError("InvalidReadRangeLimit");
      if (range.start.size() > MAX_READ_KEY_SIZE_BYTES) throw JsError("KeyTooLong");
      if (range.end.size() > MAX_READ_KEY_SIZE_BYTES) throw JsError("KeyTooLong");
      totalLimit += range.limit;
    }
    if (totalLimit > MAX_READ_ENTRIES) throw JsError("ReadRangeTooLarge");
  }

  void validateAtomicWrite(const std::vector<CheckMessage>& checks, const std::vector<MutationMessage>& mutations, const std::vector<EnqueueMessage>& enqueues,
                           double nowMs, const std::function<void(const MutationMessage&)>& afterMutation) {
    if (checks.size() > MAX_CHECKS) throw JsError("TooManyChecks");
    if (mutations.size() + enqueues.size() > MAX_MUTATIONS) throw JsError("TooManyMutations");
    std::size_t total = 0;
    for (const CheckMessage& check : checks) {
      if (check.key.size() > MAX_READ_KEY_SIZE_BYTES) throw JsError("KeyTooLong");
      total += check.key.size();
      if (!check.versionstamp.empty() && check.versionstamp.size() != 10) throw JsError("InvalidVersionstamp");
    }
    for (const MutationMessage& mutation : mutations) {
      if (mutation.key.size() > MAX_WRITE_KEY_SIZE_BYTES) throw JsError("KeyTooLong");
      total += mutation.key.size();
      if (mutation.data.size() > MAX_VALUE_SIZE_BYTES) throw JsError("ValueTooLong");
      total += mutation.data.size();
      if (afterMutation) afterMutation(mutation);
    }
    for (const EnqueueMessage& enqueue : enqueues) {
      if (enqueue.payload.size() > MAX_VALUE_SIZE_BYTES) throw JsError("ValueTooLong");
      total += enqueue.payload.size();
      if (enqueue.keysIfUndelivered.size() > MAX_QUEUE_UNDELIVERED_KEYS) throw JsError("TooManyQueueUndeliveredKeys");
      for (const KvBytes& key : enqueue.keysIfUndelivered) {
        if (key.size() > MAX_WRITE_KEY_SIZE_BYTES) throw JsError("KeyTooLong");
        total += key.size();
      }
      if (enqueue.backoffSchedule.size() > MAX_QUEUE_BACKOFF_INTERVALS) throw JsError("TooManyQueueBackoffIntervals");
      for (const std::uint32_t interval : enqueue.backoffSchedule) {
        if (interval > MAX_QUEUE_BACKOFF_MS) throw JsError("QueueBackoffIntervalTooLarge");
        total += 4;
      }
      if (!(enqueue.deadlineMs >= 0) || nowMs - enqueue.deadlineMs > MAX_QUEUE_DELAY_MS) throw JsError("InvalidMutationEnqueueDeadline");
    }
    if (total > MAX_TOTAL_MUTATION_SIZE_BYTES) throw JsError("AtomicWriteTooLarge");
  }

  // ---- BaseKv / ProtoBasedKv -----------------------------------------------------------------

  void Store::checkOpen(std::string_view method) const {
    if (closed) throw JsError("Cannot call '." + std::string(method) + "' after '.close' is called");
  }

  void checkKeyNotEmpty(const KvKey& key) {
    if (key.empty()) throw TypeError("Key cannot be empty");
  }

  KvValue readValue(const Store& store, const NativeEntry& entry) {
    if (const auto* decoded = std::get_if<KvValue>(&entry.value)) return *decoded;
    const EncodedValue& encoded = std::get<EncodedValue>(entry.value);
    if (encoded.encoding == VE_V8) return store.decodeV8(encoded.data);
    if (encoded.encoding == VE_LE64) return KvValue(unpackKvu(encoded.data));
    if (encoded.encoding == VE_BYTES) return KvValue(encoded.data);
    std::string bytes;
    for (const std::uint8_t byte : encoded.data) bytes += (bytes.empty() ? "" : ", ") + std::to_string(byte);
    throw JsError("Unsupported encoding: " + std::to_string(encoded.encoding) + " [" + bytes + "]");
  }

  Task<KvEntryMaybe<>> get(StorePtr store, KvKey key) {
    store->checkOpen("get");
    checkKeyNotEmpty(key);
    const KvBytes packed = packKey(key);
    std::vector<ReadRange> ranges{readRangeForKey(packed)};
    const std::vector<RangeEntries> results = co_await store->native->snapshotRead(std::move(ranges));
    for (const RangeEntries& range : results)
      for (const NativeEntry& entry : range)
        if (entry.key == packed) co_return KvEntryMaybe<>{std::move(key), readValue(*store, entry), formatVersionstamp(entry.version)};
    co_return KvEntryMaybe<>{std::move(key), nullptr, std::nullopt};
  }

  Task<std::vector<KvEntryMaybe<>>> getMany(StorePtr store, std::vector<KvKey> keys) {
    store->checkOpen("getMany");
    for (const KvKey& key : keys) checkKeyNotEmpty(key);
    if (keys.empty()) co_return std::vector<KvEntryMaybe<>>{};
    std::vector<KvBytes> packedKeys;
    std::vector<ReadRange> ranges;
    for (const KvKey& key : keys) {
      packedKeys.push_back(packKey(key));
      ranges.push_back(readRangeForKey(packedKeys.back()));
    }
    const std::vector<RangeEntries> results = co_await store->native->snapshotRead(std::move(ranges));
    // rowMap: every entry a range returned (the next key's row for an absent key included).
    struct Row {
      KvValue value;
      std::string versionstamp;
    };
    std::map<KvBytes, Row> rowMap;
    for (const RangeEntries& range : results)
      for (const NativeEntry& entry : range) {
        (void)unpackKey(entry.key);
        rowMap.insert_or_assign(entry.key, Row{readValue(*store, entry), formatVersionstamp(entry.version)});
      }
    std::vector<KvEntryMaybe<>> entries;
    entries.reserve(keys.size());
    for (std::size_t index = 0; index < keys.size(); ++index) {
      const auto found = rowMap.find(packedKeys[index]);
      if (found != rowMap.end()) entries.push_back(KvEntryMaybe<>{std::move(keys[index]), found->second.value, found->second.versionstamp});
      else entries.push_back(KvEntryMaybe<>{std::move(keys[index]), nullptr, std::nullopt});
    }
    co_return entries;
  }

  Task<KvCommitResult> commit(StorePtr store, std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations, std::vector<Enqueue> enqueues) {
    std::vector<CheckMessage> checkMessages;
    checkMessages.reserve(checks.size());
    for (const AtomicCheck& check : checks)
      checkMessages.push_back(CheckMessage{packKey(check.key), check.versionstamp ? kv_internal::decodeHex(*check.versionstamp) : KvBytes()});
    std::vector<MutationMessage> mutationMessages;
    mutationMessages.reserve(mutations.size());
    for (KvMutation& mutation : mutations) {
      MutationMessage message;
      message.key = packKey(mutation.key);
      message.type = mutation.type;
      if (mutation.type != KvMutationType::delete_) {
        packKvValue(message, *store, mutation.value);
        message.value = std::move(mutation.value);
      }
      if (mutation.type == KvMutationType::set && mutation.expireIn) message.expireAtMs = store->now() + *mutation.expireIn;
      mutationMessages.push_back(std::move(message));
    }
    std::vector<EnqueueMessage> enqueueMessages;
    enqueueMessages.reserve(enqueues.size());
    for (const Enqueue& enqueue : enqueues) {
      EnqueueMessage message;
      message.backoffSchedule.assign(std::begin(ENQUEUE_BACKOFF_SCHEDULE), std::end(ENQUEUE_BACKOFF_SCHEDULE));
      message.deadlineMs = store->now() + enqueue.options.delay.value_or(0);
      if (enqueue.options.keysIfUndelivered)
        for (const KvKey& key : *enqueue.options.keysIfUndelivered) message.keysIfUndelivered.push_back(packKey(key));
      message.payload = store->encodeV8(enqueue.value);
      enqueueMessages.push_back(std::move(message));
    }
    const std::optional<std::uint64_t> version = co_await store->native->atomicWrite(std::move(checkMessages), std::move(mutationMessages), std::move(enqueueMessages));
    if (!version) co_return KvCommitResult{false, ""};
    co_return KvCommitResult{true, formatVersionstamp(*version)};
  }

  // ---- listStream --------------------------------------------------------------------------

  ListStream::ListStream(StorePtr store, Selector selector, KvListOptions options) : store_(std::move(store)), selector_(std::move(selector)), options_(std::move(options)) {}

  std::string ListStream::cursor() const {
    if (!outCursor_) throw JsError("Cannot get cursor before first iteration");
    return *outCursor_;
  }

  void ListStream::finish() noexcept { status_ = Status::done; }

  Task<std::optional<KvEntry<>>> ListStream::next() {
    if (status_ == Status::done) co_return std::nullopt;
    try {
      bool fetch = false;
      if (status_ == Status::initial) {
        status_ = Status::running;
        if (options_.cursor) lastYieldedKeyBytes_ = unpackCursor(*options_.cursor);
        if (selector_.prefix) prefixBytes_ = packKey(*selector_.prefix);
        fetch = true;
      } else if (suspendedAtYield_) {
        suspendedAtYield_ = false;
        ++yielded_;
        if (options_.limit && static_cast<double>(yielded_) >= *options_.limit) {
          status_ = Status::done;
          co_return std::nullopt;
        }
      }
      for (;;) {
        if (fetch) {
          co_await fetchBatch();
          fetch = false;
        }
        while (index_ < batch_.size()) {
          const NativeEntry& entry = batch_[index_++];
          if (entries_++ == 0 && ((lastYieldedKeyBytes_ && *lastYieldedKeyBytes_ == entry.key) || (prefixBytes_ && *prefixBytes_ == entry.key))) continue;
          KvKey key = unpackKey(entry.key);
          KvValue value = readValue(*store_, entry);
          KvEntry<> yieldedEntry{std::move(key), std::move(value), formatVersionstamp(entry.version)};
          lastYieldedKeyBytes_ = entry.key;
          outCursor_ = packCursor(entry.key);
          suspendedAtYield_ = true;
          co_return yieldedEntry;
        }
        if (entries_ < batchLimit_) {
          status_ = Status::done;
          co_return std::nullopt;
        }
        fetch = true;
      }
    } catch (...) {
      status_ = Status::done;
      throw;
    }
  }

  Task<void> ListStream::fetchBatch() {
    KvBytes start, end;
    if (selector_.prefix) {
      start = selector_.start ? packKey(*selector_.start) : *prefixBytes_;
      if (selector_.end) {
        end = packKey(*selector_.end);
      } else {
        end = *prefixBytes_;
        end.push_back(0xFF);
      }
    } else {
      start = packKey(*selector_.start);
      end = packKey(*selector_.end);
    }
    if (options_.reverse) {
      if (lastYieldedKeyBytes_) end = *lastYieldedKeyBytes_;
    } else if (lastYieldedKeyBytes_) {
      start = *lastYieldedKeyBytes_;
    }
    double batch = std::min(options_.batchSize.value_or(100), 500.0);
    batch = std::min(batch, options_.limit.value_or(Number::MAX_SAFE_INTEGER));
    batchLimit_ = static_cast<std::size_t>(batch) + (lastYieldedKeyBytes_ ? 1 : 0);
    std::vector<ReadRange> ranges{ReadRange{std::move(start), std::move(end), batchLimit_, options_.reverse}};
    std::vector<RangeEntries> results = co_await store_->native->snapshotRead(std::move(ranges));
    batch_ = std::move(results.front());
    index_ = 0;
    entries_ = 0;
  }

  std::unique_ptr<ListStream> list(const StorePtr& store, const KvListSelector& selector, const KvListOptions& options) {
    store->checkOpen("list");
    ListStream::Selector checked = checkListSelector(selector);
    checkListOptions(options);
    return std::make_unique<ListStream>(store, std::move(checked), options);
  }

  Task<std::optional<KvEntry<>>> GenericListIterator::next() { return stream_->next(); }

  std::string GenericListIterator::cursor() const { return stream_->cursor(); }

  Task<void> GenericListIterator::return_() {
    stream_->finish();
    co_return;
  }
}
