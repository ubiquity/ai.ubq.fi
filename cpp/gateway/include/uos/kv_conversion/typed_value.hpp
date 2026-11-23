#pragma once
/** Runtime-independent graph interchange. No V8/JSC bytes are ever confused. */
// The JavaScript values are rt::KvValue trees (the stored-value model): a graph
// whose `ref` atoms revisit a finished node decodes to a copy of it, a graph
// with a cycle cannot be decoded (TypeError), and a native capture Buffer
// decodes to a plain Uint8Array view because KvValue has no Buffer brand.
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

namespace uos::kv_conversion::typed_value {
  // `{ root: Atom; nodes: GraphNode[] }` as its JSON:
  //   Atom      = { type: string; value?: string | number | boolean | null; ref?: number }
  //   GraphNode = { type: string; value?: string; items?: Atom[]; entries?: [string | Atom, Atom][];
  //                 buffer?: Atom; offset?: number; length?: number }
  // Decoding reads it with JavaScript's loose property semantics, so any JSON is accepted.
  using TypedValue = json::value;

  TypedValue encodeTypedValue(const rt::KvValue& value);
  /** Source-native capture only. The ordinary profile/legacy codec still refuses Buffer. */
  TypedValue encodeNativeCaptureValue(const rt::KvValue& value);
  rt::KvValue decodeTypedValue(const TypedValue& graph);
  /** Only the existing native SourceGraphV1 Buffer tag is added, never arbitrary classes. */
  rt::KvValue decodeNativeCaptureValue(const TypedValue& graph);
}
