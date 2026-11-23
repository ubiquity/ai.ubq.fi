// MemoryKv: the guarded native store (src/storage/native-kv.ts over the SDK's
// ProtoBasedKv/BaseKv and the SQLite backend) in memory. The SDK layer is kv_sdk, shared
// with SqliteKv; this file holds the native store in memory (its limits, transaction and
// expiry sweep) and the guard's behaviour.
#include "kv_sdk.hpp"
#include <boost/asio/post.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <algorithm>
#include <cmath>
#include <map>

namespace uos::rt {
  namespace {
    using kv_sdk::VE_BYTES;
    using kv_sdk::VE_LE64;
    using kv_sdk::VE_V8;

    // The native addon resolves its promise from a completed native task: the
    // caller resumes one macrotask later.
    Task<void> nextMacrotask() {
      auto executor = co_await boost::asio::this_coro::executor;
      co_await boost::asio::post(executor, boost::asio::use_awaitable);
    }

    KvBytes versionstampBytes(std::uint64_t version) {
      KvBytes bytes(10, 0);
      for (int index = 0; index < 8; ++index) bytes[static_cast<std::size_t>(index)] = static_cast<std::uint8_t>(version >> (56 - 8 * index));
      return bytes;
    }

    // The guard's `/^Failed to perform '(sum|min|max)' mutation on a non-U64 value in the database$/`.
    bool isArithmeticTypeFailure(std::string_view message) {
      for (const std::string_view operation : {"sum", "min", "max"})
        if (message == "Failed to perform '" + std::string(operation) + "' mutation on a non-U64 value in the database") return true;
      return false;
    }
  }

  // ---- The database (one SQLite file) -------------------------------------------------------

  struct StoredRow {
    KvValue value;
    int encoding = VE_V8;
    std::uint64_t version = 0;
    double expirationMs = -1;
  };

  namespace {
    struct MemoryNative;
  }

  struct MemoryKvDatabase::State {
    std::function<double()> now;
    MemoryKvOptions options;
    std::map<KvBytes, StoredRow> rows;
    std::uint64_t version = 0;
    std::vector<std::weak_ptr<MemoryNative>> handles;

    void sweep();
    void deleteExpired(double at) {
      for (auto row = rows.begin(); row != rows.end();) {
        if (row->second.expirationMs >= 0 && row->second.expirationMs <= at) row = rows.erase(row);
        else ++row;
      }
    }
  };

  namespace {
    // One open database handle (napi.open): its rows, "db not found" once closed, and
    // the expiry sweep, behind the NapiInterface calls the SDK makes.
    struct MemoryNative final : kv_sdk::Native {
      std::shared_ptr<MemoryKvDatabase> database;
      // The database id is gone (napi.close).
      bool closed = false;
      double openedAtMs = 0;
      double nextSweepAtMs = 0;

      MemoryKvDatabase::State& state() { return database->state(); }

      // A native call on a closed database handle.
      void checkNative() {
        if (closed) throw JsError("db not found");
        state().sweep();
      }

      // napi.snapshotRead: validates, then reads every range in one snapshot.
      Task<std::vector<kv_sdk::RangeEntries>> snapshotRead(std::vector<kv_sdk::ReadRange> ranges) override {
        std::exception_ptr error;
        std::vector<kv_sdk::RangeEntries> results;
        try {
          checkNative();
          kv_sdk::validateSnapshotRead(ranges);
          auto& rows = state().rows;
          for (const kv_sdk::ReadRange& range : ranges) {
            kv_sdk::RangeEntries found;
            if (range.start < range.end) {
              const auto first = rows.lower_bound(range.start);
              const auto last = rows.lower_bound(range.end);
              if (range.reverse) {
                for (auto row = last; row != first && found.size() < range.limit;) {
                  --row;
                  found.push_back(kv_sdk::NativeEntry{row->first, row->second.value, row->second.version});
                }
              } else {
                for (auto row = first; row != last && found.size() < range.limit; ++row)
                  found.push_back(kv_sdk::NativeEntry{row->first, row->second.value, row->second.version});
              }
            }
            results.push_back(std::move(found));
          }
        } catch (...) {
          error = std::current_exception();
        }
        co_await nextMacrotask();
        if (error) std::rethrow_exception(error);
        co_return results;
      }

      // The SQLite backend's arithmetic mutation on the current row.
      static KvU64 arithmetic(KvMutationType type, const StoredRow* current, KvU64 operand) {
        if (!current) return operand;
        if (current->encoding != VE_LE64) throw JsError("Failed to perform '" + std::string(toString(type)) + "' mutation on a non-U64 value in the database");
        const KvU64 existing = current->value.asU64();
        if (type == KvMutationType::sum) return existing.sum(operand);
        if (type == KvMutationType::min) return existing.min(operand);
        return existing.max(operand);
      }

      // napi.atomicWrite: the limits in their native order, then one transaction that
      // checks, applies every mutation in order and rolls back entirely on an error.
      Task<std::optional<std::uint64_t>> atomicWrite(std::vector<kv_sdk::CheckMessage> checks, std::vector<kv_sdk::MutationMessage> mutations,
                                                     std::vector<kv_sdk::EnqueueMessage> enqueues) override {
        std::exception_ptr error;
        std::optional<std::uint64_t> committed;
        try {
          checkNative();
          kv_sdk::validateAtomicWrite(checks, mutations, enqueues, state().now(), [](const kv_sdk::MutationMessage& mutation) {
            if (mutation.type != KvMutationType::set && mutation.type != KvMutationType::delete_ && mutation.encoding != VE_LE64)
              throw JsError("Some of the parameters are not valid V8 values");
          });
          auto& current = state();
          bool checksPass = true;
          for (const kv_sdk::CheckMessage& check : checks) {
            const auto row = current.rows.find(check.key);
            const KvBytes stamp = row == current.rows.end() ? KvBytes() : versionstampBytes(row->second.version);
            if (stamp != check.versionstamp) {
              checksPass = false;
              break;
            }
          }
          if (checksPass) {
            const std::uint64_t increment = current.options.versionIncrement ? current.options.versionIncrement() : 1;
            const std::uint64_t version = current.version + increment;
            auto& rows = current.rows;
            // The transaction's undo log: each touched key's row before its first change.
            std::vector<std::pair<KvBytes, std::optional<StoredRow>>> undo;
            const auto remember = [&](const KvBytes& key) {
              if (std::any_of(undo.begin(), undo.end(), [&](const auto& entry) { return entry.first == key; })) return;
              const auto row = rows.find(key);
              undo.emplace_back(key, row == rows.end() ? std::nullopt : std::optional<StoredRow>(row->second));
            };
            try {
              for (const kv_sdk::MutationMessage& mutation : mutations) {
                remember(mutation.key);
                if (mutation.type == KvMutationType::delete_) {
                  rows.erase(mutation.key);
                } else if (mutation.type == KvMutationType::set) {
                  rows[mutation.key] = StoredRow{kvStoredValue(mutation.value), mutation.encoding, version, mutation.expireAtMs > 0 ? mutation.expireAtMs : -1};
                } else {
                  const auto row = rows.find(mutation.key);
                  const KvU64 next = arithmetic(mutation.type, row == rows.end() ? nullptr : &row->second, mutation.value.asU64());
                  rows[mutation.key] = StoredRow{KvValue(next), VE_LE64, version, -1};
                }
              }
            } catch (...) {
              for (auto entry = undo.rbegin(); entry != undo.rend(); ++entry) {
                if (entry->second) rows[entry->first] = *entry->second;
                else rows.erase(entry->first);
              }
              throw;
            }
            current.version = version;
            committed = version;
          }
        } catch (...) {
          error = std::current_exception();
        }
        co_await nextMacrotask();
        if (error) std::rethrow_exception(error);
        co_return committed;
      }
    };
  }

  void MemoryKvDatabase::State::sweep() {
    const double at = now();
    for (auto weak = handles.begin(); weak != handles.end();) {
      const std::shared_ptr<MemoryNative> handle = weak->lock();
      if (!handle || handle->closed) {
        weak = handles.erase(weak);
        continue;
      }
      if (options.sweepIntervalMs <= 0) {
        deleteExpired(at);
      } else if (at >= handle->nextSweepAtMs) {
        const double periods = std::floor((at - handle->openedAtMs) / options.sweepIntervalMs);
        const double sweepAt = handle->openedAtMs + periods * options.sweepIntervalMs;
        deleteExpired(sweepAt);
        handle->nextSweepAtMs = sweepAt + options.sweepIntervalMs;
      }
      ++weak;
    }
  }

  MemoryKvDatabase::MemoryKvDatabase(std::function<double()> now, MemoryKvOptions options) : state_(std::make_unique<State>()) {
    state_->now = std::move(now);
    state_->options = std::move(options);
  }

  MemoryKvDatabase::~MemoryKvDatabase() = default;

  std::optional<MemoryKvDatabase::Row> MemoryKvDatabase::row(const KvKey& key) const {
    const auto found = state_->rows.find(packKey(key));
    if (found == state_->rows.end()) return std::nullopt;
    return Row{unpackKey(found->first), found->second.value, found->second.encoding, found->second.version, found->second.expirationMs};
  }

  std::vector<MemoryKvDatabase::Row> MemoryKvDatabase::rows() const {
    std::vector<Row> out;
    for (const auto& [packed, stored] : state_->rows) out.push_back(Row{unpackKey(packed), stored.value, stored.encoding, stored.version, stored.expirationMs});
    return out;
  }

  std::uint64_t MemoryKvDatabase::version() const noexcept { return state_->version; }

  // The SDK store over one native handle.
  struct MemoryKv::Handle {
    std::shared_ptr<MemoryNative> native;
    kv_sdk::StorePtr store;
  };

  namespace {
    using HandlePtr = std::shared_ptr<MemoryKv::Handle>;

    // ProtoBasedKv.commit behind BaseKv.atomic's checkOpen("commit") and the guard's
    // checkedAtomic translation of arithmetic type failures.
    Task<KvCommitResult> commitChecked(kv_sdk::StorePtr store, std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations) {
      store->checkOpen("commit");
      try {
        std::vector<kv_sdk::Enqueue> enqueues;
        const KvCommitResult result = co_await kv_sdk::commit(store, std::move(checks), std::move(mutations), std::move(enqueues));
        co_return result;
      } catch (const JsError& error) {
        if (error.name == "Error" && isArithmeticTypeFailure(error.message))
          throw TypeError("Atomic arithmetic requires an absent key or KvU64 value", std::make_shared<JsError>(error));
        throw;
      }
    }

    std::unique_ptr<AtomicOperation> checkedAtomic(const HandlePtr& handle) {
      kv_sdk::StorePtr store = handle->store;
      return std::make_unique<GenericAtomicOperation>(
        [store](std::vector<AtomicCheck> checks, std::vector<KvMutation> mutations) { return commitChecked(store, std::move(checks), std::move(mutations)); });
    }

    // The guard's checkedList iterator.
    class CheckedListIterator final : public KvListIterator {
    public:
      CheckedListIterator(std::unique_ptr<kv_sdk::ListStream> iterator, KvListOptions options)
          : iterator_(std::move(iterator)), limit_(options.limit), cursor_(std::move(options.cursor)) {}

      Task<std::optional<KvEntry<>>> next() override {
        std::optional<KvEntry<>> result = co_await iterator_->next();
        if (!result) {
          // Hitting the requested page limit retains a continuation; exhausting
          // the underlying range clears it, including a scan with no rows.
          if (!limit_ || static_cast<double>(yielded_) < *limit_) cursor_ = "";
        } else {
          ++yielded_;
          cursor_ = iterator_->cursor();
        }
        co_return result;
      }

      std::string cursor() const override {
        if (!cursor_) throw JsError("Cannot get cursor before first iteration");
        return *cursor_;
      }

      Task<void> return_() override {
        iterator_->finish();
        co_return;
      }

    private:
      std::unique_ptr<kv_sdk::ListStream> iterator_;
      std::optional<double> limit_;
      std::optional<std::string> cursor_;
      std::uint64_t yielded_ = 0;
    };

    HandlePtr openHandle(std::shared_ptr<MemoryKvDatabase> database) {
      auto native = std::make_shared<MemoryNative>();
      native->database = std::move(database);
      auto& state = native->state();
      native->openedAtMs = state.now();
      native->nextSweepAtMs = native->openedAtMs + state.options.sweepIntervalMs;
      state.deleteExpired(native->openedAtMs);
      state.handles.push_back(native);
      auto store = std::make_shared<kv_sdk::Store>();
      store->native = native;
      store->encodeV8 = [](const KvValue& value) {
        assertPortableKvValue(value);
        return kvJscSerialize(value);
      };
      // MemoryKv keeps decoded rows, so readValue never decodes.
      store->decodeV8 = kvJscDeserialize;
      std::shared_ptr<MemoryKvDatabase> owner = native->database;
      store->now = [owner] { return owner->state().now(); };
      auto handle = std::make_shared<MemoryKv::Handle>();
      handle->native = std::move(native);
      handle->store = std::move(store);
      return handle;
    }
  }

  // ---- MemoryKv ---------------------------------------------------------------------------------

  MemoryKv::MemoryKv(std::function<double()> now, MemoryKvOptions options)
      : database_(std::make_shared<MemoryKvDatabase>(std::move(now), std::move(options))), handle_(openHandle(database_)) {}

  MemoryKv::MemoryKv(std::shared_ptr<MemoryKvDatabase> database) : database_(std::move(database)), handle_(openHandle(database_)) {}

  MemoryKv::~MemoryKv() = default;

  Task<KvEntryMaybe<>> MemoryKv::get(KvKey key, KvGetOptions) { return kv_sdk::get(handle_->store, std::move(key)); }

  Task<std::vector<KvEntryMaybe<>>> MemoryKv::getMany(std::vector<KvKey> keys, KvGetOptions) { return kv_sdk::getMany(handle_->store, std::move(keys)); }

  namespace {
    Task<KvCommitResult> unconditionalSet(HandlePtr handle, KvKey key, KvValue value, KvSetOptions options) {
      std::unique_ptr<AtomicOperation> operation = checkedAtomic(handle);
      operation->set(std::move(key), std::move(value), options);
      const KvCommitResult result = co_await operation->commit();
      if (!result.ok) throw JsError("Unconditional KV set failed");
      co_return result;
    }

    Task<void> unconditionalDelete(HandlePtr handle, KvKey key) {
      std::unique_ptr<AtomicOperation> operation = checkedAtomic(handle);
      operation->delete_(std::move(key));
      const KvCommitResult result = co_await operation->commit();
      if (!result.ok) throw JsError("Unconditional KV delete failed");
    }
  }

  Task<KvCommitResult> MemoryKv::set(KvKey key, KvValue value, KvSetOptions options) { return unconditionalSet(handle_, std::move(key), std::move(value), options); }

  Task<void> MemoryKv::delete_(KvKey key) { return unconditionalDelete(handle_, std::move(key)); }

  std::unique_ptr<KvListIterator> MemoryKv::list(KvListSelector selector, KvListOptions options) {
    // Retain the SDK's selector/option validation before adjusting its scan start.
    std::unique_ptr<kv_sdk::ListStream> validated = kv_sdk::list(handle_->store, selector, options);
    if (selector.prefix && !selector.start) {
      // An exact-prefix row can stall the SDK when it consumes the whole batch.
      // Empty bytes are the smallest legal extension part in the native tuple codec.
      KvKey start = kvKey(*selector.prefix, KvBytes{});
      KvListSelector native = selector.end ? KvListSelector{std::nullopt, std::move(start), selector.end} : KvListSelector{selector.prefix, std::move(start), std::nullopt};
      return std::make_unique<CheckedListIterator>(kv_sdk::list(handle_->store, native, options), options);
    }
    return std::make_unique<CheckedListIterator>(std::move(validated), options);
  }

  std::unique_ptr<AtomicOperation> MemoryKv::atomic() { return checkedAtomic(handle_); }

  void MemoryKv::close() {
    handle_->store->checkOpen("close");
    handle_->store->closed = true;
    handle_->native->closed = true;
  }
}
