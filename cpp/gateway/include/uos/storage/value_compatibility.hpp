#pragma once
#include "uos/rt/kv.hpp"

namespace uos::storage::value_compatibility {
  /** Inspect supported data graphs once, without another serialization or getter call. */
  // rt::KvValue is the stored value model, and rt::assertPortableKvValue is this
  // walk over it (the store's own encode path calls it), so this module exports
  // that function: one entity, which unqualified calls also find through ADL.
  using rt::assertPortableKvValue;
}
