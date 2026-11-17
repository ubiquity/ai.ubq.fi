#include "uos/runtime/http_early_sse.hpp"

#include <memory>
#include <optional>
#include <utility>

namespace uos::runtime::http_early_sse {
  namespace {
    // Entries are shared like the JS objects: a commit callback that re-registers or
    // unregisters its request still updates the entry it was called through.
    rt::WeakMap<rt::Request, std::shared_ptr<Entry>> readiness;
  }

  std::function<void()> registerEarlySseHeaders(rt::Request request, decltype(Entry::commit) commit) {
    readiness.set(request, std::make_shared<Entry>(Entry{std::move(commit), false}));
    return [request] { readiness.delete_(request); };
  }

  bool tryCommitEarlySseHeaders(const rt::Request& request, const EarlySseHeaders& value) {
    const std::optional<std::shared_ptr<Entry>> entry = readiness.get(request);
    if (!entry) return false;
    if ((*entry)->committed) return true;
    try {
      (*entry)->committed = (*entry)->commit(value);
    } catch (...) {
      return false;
    }
    return (*entry)->committed;
  }

  bool earlySseHeadersCommitted(const rt::Request& request) {
    const std::shared_ptr<Entry>* entry = readiness.find(request);
    return entry != nullptr && (*entry)->committed;
  }
}
