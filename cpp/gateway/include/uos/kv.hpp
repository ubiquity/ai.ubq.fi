#pragma once
// Port of src/kv.ts: the process-wide KV handle, opened lazily with bounded reconnects.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"

#include <functional>
#include <memory>

namespace uos::kv {
  // `() => Promise<Kv | null>`: a null shared_ptr is null.
  using KvOpener = std::function<rt::Task<std::shared_ptr<rt::Kv>>()>;

  /** Install the persistent database before the VPS starts accepting requests. */
  void initializeKv(std::shared_ptr<rt::Kv> kv);

  // `getKv(): Promise<Kv | null>`; concurrent callers share one open attempt (the same promise).
  rt::Promise<std::shared_ptr<rt::Kv>> getKv();

  /** A null fixture disables disk fallback; a supplied opener exercises reconnects. */
  // `setKvForTest(kv, opener?)`: the one-argument overload, or an empty opener, is an absent opener.
  void setKvForTest(std::shared_ptr<rt::Kv> kv);
  void setKvForTest(std::shared_ptr<rt::Kv> kv, KvOpener opener);

  /** Restore runtime opening after a scoped test has finished. */
  void resetKvForTest();
}
