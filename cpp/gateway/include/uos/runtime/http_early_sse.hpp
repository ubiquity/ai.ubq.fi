#pragma once
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include <functional>

namespace uos::runtime::http_early_sse {
  // Readonly<{ headers: Headers }>
  struct EarlySseHeaders {
    rt::Headers headers{};
  };

  // { commit: (value: EarlySseHeaders) => boolean; committed: boolean }
  struct Entry {
    std::function<bool(const EarlySseHeaders& value)> commit{};
    bool committed = false;
  };

  /** Physical transport registration only; native metadata never creates this capability. */
  std::function<void()> registerEarlySseHeaders(rt::Request request, decltype(Entry::commit) commit);

  /** Synchronous and failure-contained so the provider keeps its original admission receipt. */
  bool tryCommitEarlySseHeaders(const rt::Request& request, const EarlySseHeaders& value);

  bool earlySseHeadersCommitted(const rt::Request& request);
}
