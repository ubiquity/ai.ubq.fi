// The SDK's GenericAtomicOperation (kv_util.js), Array.fromAsync over a list,
// and the forwarding wrappers tests subclass in place of `new Proxy(kv, ...)`.
#include "kv_internal.hpp"

namespace uos::rt {
  namespace {
    void checkKeyNotEmpty(const KvKey& key) {
      if (key.empty()) throw TypeError("Key cannot be empty");
    }

    void checkExpireIn(const std::optional<double>& expireIn) {
      if (expireIn && !(*expireIn > 0 && Number::isSafeInteger(*expireIn)))
        throw TypeError("Bad 'expireIn', expected optional positive integer, found " + numberToString(*expireIn));
    }
  }

  // ---- GenericAtomicOperation ---------------------------------------------------------------

  AtomicOperation& GenericAtomicOperation::check(std::vector<AtomicCheck> items) {
    checks.insert(checks.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    return *this;
  }

  AtomicOperation& GenericAtomicOperation::mutate(std::vector<KvMutation> items) {
    for (const KvMutation& mutation : items) checkKeyNotEmpty(mutation.key);
    for (const KvMutation& mutation : items)
      if (mutation.type == KvMutationType::set) checkExpireIn(mutation.expireIn);
    mutations.insert(mutations.end(), std::make_move_iterator(items.begin()), std::make_move_iterator(items.end()));
    return *this;
  }

  AtomicOperation& GenericAtomicOperation::sum(KvKey key, const BigInt& n) {
    checkKeyNotEmpty(key);
    KvMutation mutation{std::move(key), KvMutationType::sum, KvU64(n), std::nullopt};
    std::vector<KvMutation> items;
    items.push_back(std::move(mutation));
    return mutate(std::move(items));
  }

  AtomicOperation& GenericAtomicOperation::min(KvKey key, const BigInt& n) {
    checkKeyNotEmpty(key);
    KvMutation mutation{std::move(key), KvMutationType::min, KvU64(n), std::nullopt};
    std::vector<KvMutation> items;
    items.push_back(std::move(mutation));
    return mutate(std::move(items));
  }

  AtomicOperation& GenericAtomicOperation::max(KvKey key, const BigInt& n) {
    checkKeyNotEmpty(key);
    KvMutation mutation{std::move(key), KvMutationType::max, KvU64(n), std::nullopt};
    std::vector<KvMutation> items;
    items.push_back(std::move(mutation));
    return mutate(std::move(items));
  }

  AtomicOperation& GenericAtomicOperation::set(KvKey key, KvValue value, KvSetOptions options) {
    checkExpireIn(options.expireIn);
    checkKeyNotEmpty(key);
    KvMutation mutation{std::move(key), KvMutationType::set, std::move(value), options.expireIn};
    std::vector<KvMutation> items;
    items.push_back(std::move(mutation));
    return mutate(std::move(items));
  }

  AtomicOperation& GenericAtomicOperation::delete_(KvKey key) {
    checkKeyNotEmpty(key);
    KvMutation mutation{std::move(key), KvMutationType::delete_, KvValue(), std::nullopt};
    std::vector<KvMutation> items;
    items.push_back(std::move(mutation));
    return mutate(std::move(items));
  }

  Task<KvCommitResult> GenericAtomicOperation::commit() { co_return co_await commitFn(checks, mutations); }

  // ---- Array.fromAsync ----------------------------------------------------------------------

  Task<std::vector<KvEntry<>>> kvListToArray(KvListIterator& iterator) {
    std::vector<KvEntry<>> entries;
    while (std::optional<KvEntry<>> entry = co_await iterator.next()) entries.push_back(std::move(*entry));
    co_return entries;
  }

  // ---- Forwarding wrappers ------------------------------------------------------------------

  Task<KvEntryMaybe<>> ForwardingKv::get(KvKey key, KvGetOptions options) { return inner.get(std::move(key), options); }

  Task<std::vector<KvEntryMaybe<>>> ForwardingKv::getMany(std::vector<KvKey> keys, KvGetOptions options) { return inner.getMany(std::move(keys), options); }

  Task<KvCommitResult> ForwardingKv::set(KvKey key, KvValue value, KvSetOptions options) { return inner.set(std::move(key), std::move(value), options); }

  Task<void> ForwardingKv::delete_(KvKey key) { return inner.delete_(std::move(key)); }

  std::unique_ptr<KvListIterator> ForwardingKv::list(KvListSelector selector, KvListOptions options) { return inner.list(std::move(selector), std::move(options)); }

  std::unique_ptr<AtomicOperation> ForwardingKv::atomic() { return inner.atomic(); }

  void ForwardingKv::close() { inner.close(); }

  AtomicOperation& ForwardingAtomicOperation::check(std::vector<AtomicCheck> items) {
    inner->check(std::move(items));
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::mutate(std::vector<KvMutation> items) {
    inner->mutate(std::move(items));
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::sum(KvKey key, const BigInt& n) {
    inner->sum(std::move(key), n);
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::min(KvKey key, const BigInt& n) {
    inner->min(std::move(key), n);
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::max(KvKey key, const BigInt& n) {
    inner->max(std::move(key), n);
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::set(KvKey key, KvValue value, KvSetOptions options) {
    inner->set(std::move(key), std::move(value), options);
    return *this;
  }

  AtomicOperation& ForwardingAtomicOperation::delete_(KvKey key) {
    inner->delete_(std::move(key));
    return *this;
  }

  Task<KvCommitResult> ForwardingAtomicOperation::commit() { return inner->commit(); }
}
