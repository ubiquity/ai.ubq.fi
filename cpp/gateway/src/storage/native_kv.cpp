#include "uos/storage/native_kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/regex.hpp"
#include <vector>

namespace uos::storage::native_kv {
  namespace {
    // `{ cause: error }`: the Error a reason holds.
    std::shared_ptr<const rt::JsError> causeOf(const rt::Reason& reason) {
      try {
        std::rethrow_exception(reason);
      } catch (const rt::JsError& error) {
        return std::make_shared<const rt::JsError>(error);
      } catch (const std::exception& error) {
        return std::make_shared<const rt::JsError>(error.what());
      } catch (...) {
        return nullptr;
      }
    }

    /** The verified backend rolls back each failed write; preserve caller budgets exactly. */
    class CheckedAtomicOperation final : public rt::ForwardingAtomicOperation {
    public:
      CheckedAtomicOperation(std::shared_ptr<rt::Kv> native, std::unique_ptr<rt::AtomicOperation> operation)
          : rt::ForwardingAtomicOperation(std::move(operation)), native_(std::move(native)) {}

      rt::Task<rt::KvCommitResult> commit() override {
        static const rt::Regex NON_U64_ARITHMETIC("^Failed to perform '(sum|min|max)' mutation on a non-U64 value in the database$");
        rt::Reason failure;
        try {
          std::function<rt::Task<rt::KvCommitResult>()> commitOnce = [this] { return rt::ForwardingAtomicOperation::commit(); };
          rt::Task<rt::KvCommitResult> retried = retrySqliteBusy<rt::KvCommitResult>(std::move(commitOnce));
          rt::KvCommitResult result = co_await std::move(retried);
          co_return result;
        } catch (...) {
          failure = std::current_exception();
        }
        const std::optional<std::string> message = rt::errorMessage(failure);
        if (rt::isError(failure) && message && NON_U64_ARITHMETIC.test(*message))
          throw rt::TypeError("Atomic arithmetic requires an absent key or KvU64 value", causeOf(failure));
        std::rethrow_exception(failure);
      }

    private:
      std::shared_ptr<rt::Kv> native_;
    };

    std::unique_ptr<rt::AtomicOperation> checkedAtomic(const std::shared_ptr<rt::Kv>& kv) {
      return std::make_unique<CheckedAtomicOperation>(kv, kv->atomic());
    }

    class CheckedListIterator final : public rt::KvListIterator {
    public:
      CheckedListIterator(std::shared_ptr<rt::Kv> native, std::unique_ptr<rt::KvListIterator> iterator, rt::KvListOptions options)
          : native_(std::move(native)), iterator_(std::move(iterator)), limit_(options.limit), cursor_(std::move(options.cursor)) {}

      std::string cursor() const override {
        if (!cursor_) throw rt::JsError("Cannot get cursor before first iteration");
        return *cursor_;
      }

      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        std::function<rt::Task<std::optional<rt::KvEntry<>>>()> step = [this] { return iterator_->next(); };
        rt::Task<std::optional<rt::KvEntry<>>> retried = retrySqliteBusy<std::optional<rt::KvEntry<>>>(std::move(step));
        std::optional<rt::KvEntry<>> result = co_await std::move(retried);
        if (!result) {
          // Hitting the requested page limit retains a continuation; exhausting
          // the underlying range clears it, including a scan with no rows.
          if (!limit_ || static_cast<double>(yielded_) < *limit_) cursor_ = "";
        } else {
          yielded_ += 1;
          cursor_ = iterator_->cursor();
        }
        co_return result;
      }

      rt::Task<void> return_() override { co_await iterator_->return_(); }

    private:
      std::shared_ptr<rt::Kv> native_;
      std::unique_ptr<rt::KvListIterator> iterator_;
      std::optional<double> limit_;
      std::optional<std::string> cursor_;
      std::size_t yielded_ = 0;
    };

    std::unique_ptr<rt::KvListIterator> checkedList(const std::shared_ptr<rt::Kv>& kv, rt::KvListSelector selector, rt::KvListOptions options) {
      // Retain the SDK's selector/option validation before adjusting its scan start.
      std::unique_ptr<rt::KvListIterator> validated = kv->list(selector, options);
      std::optional<rt::KvListSelector> nativeSelector;
      if (selector.prefix && !selector.start) {
        // An exact-prefix row can stall the SDK when it consumes the whole batch.
        // Empty bytes are the smallest legal extension part in the native tuple codec.
        rt::KvKey start{*selector.prefix, rt::KvBytes{}};
        nativeSelector = rt::KvListSelector{};
        if (selector.end) {
          nativeSelector->start = std::move(start);
          nativeSelector->end = selector.end;
        } else {
          nativeSelector->prefix = selector.prefix;
          nativeSelector->start = std::move(start);
        }
      }
      std::unique_ptr<rt::KvListIterator> iterator = nativeSelector ? kv->list(*nativeSelector, options) : std::move(validated);
      return std::make_unique<CheckedListIterator>(kv, std::move(iterator), std::move(options));
    }

    // The guarded store: `{ ...native, atomic, set, delete, get, getMany, list }`.
    class GuardedKv : public rt::Kv {
    public:
      // Capture methods before a caller can instrument the writable public facade.
      explicit GuardedKv(std::shared_ptr<rt::Kv> native) : native_(std::move(native)) {}

      using rt::Kv::get;
      using rt::Kv::getMany;

      rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
        std::function<rt::Task<rt::KvEntryMaybe<>>()> read = [native = native_, key = std::move(key), options] { return native->get(key, options); };
        return retrySqliteBusy<rt::KvEntryMaybe<>>(std::move(read));
      }

      rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions options) override {
        std::function<rt::Task<std::vector<rt::KvEntryMaybe<>>>()> read = [native = native_, keys = std::move(keys), options] { return native->getMany(keys, options); };
        return retrySqliteBusy<std::vector<rt::KvEntryMaybe<>>>(std::move(read));
      }

      rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options) override {
        std::unique_ptr<rt::AtomicOperation> operation = checkedAtomic(native_);
        operation->set(std::move(key), std::move(value), options);
        const rt::KvCommitResult result = co_await operation->commit();
        if (!result.ok) throw rt::JsError("Unconditional KV set failed");
        co_return result;
      }

      rt::Task<void> delete_(rt::KvKey key) override {
        std::unique_ptr<rt::AtomicOperation> operation = checkedAtomic(native_);
        operation->delete_(std::move(key));
        const rt::KvCommitResult result = co_await operation->commit();
        if (!result.ok) throw rt::JsError("Unconditional KV delete failed");
      }

      std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options) override {
        return checkedList(native_, std::move(selector), std::move(options));
      }

      std::unique_ptr<rt::AtomicOperation> atomic() override { return checkedAtomic(native_); }

      void close() override { native_->close(); }

    private:
      std::shared_ptr<rt::Kv> native_;
    };

    // `enqueue: kv.enqueue.bind(kv)`, spread from `native` unchanged (no busy retry): the
    // guarded store of a native store that has a queue.
    class GuardedQueueKv final : public GuardedKv, public rt::KvEnqueue {
    public:
      GuardedQueueKv(std::shared_ptr<rt::Kv> native, rt::KvEnqueue& queue) : GuardedKv(std::move(native)), queue_(queue) {}

      rt::Task<void> enqueue(rt::KvValue message, rt::KvEnqueueOptions options) override { return queue_.enqueue(std::move(message), std::move(options)); }

    private:
      rt::KvEnqueue& queue_;
    };
  }

  std::unique_ptr<rt::Kv> guardNativeKv(std::shared_ptr<rt::Kv> kv) {
    if (auto* queue = dynamic_cast<rt::KvEnqueue*>(kv.get())) return std::make_unique<GuardedQueueKv>(std::move(kv), *queue);
    return std::make_unique<GuardedKv>(std::move(kv));
  }
}
