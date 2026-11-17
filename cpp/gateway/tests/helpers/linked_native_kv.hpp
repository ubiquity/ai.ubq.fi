#pragma once
// The native KV release binding for tests that open the durable store through
// storage::kv::openKv: the C++ side of scripts/test-native-kv.ts, which prepares the native
// backend for bun test (`initializeTestNativeKv(binding)`) and hands the binding to child
// processes (`currentTestNativeKvBinding()`).
//
// src/storage/kv.ts opens a database only once the release artifact was verified and prepared
// (src/storage/native-addon.ts prepareNativeKvBackend). The C++ runtime links rt::SqliteKv
// instead of loading that artifact (docs/rt/kv-durable.md), so the binding here names a
// synthetic artifact that passes the same verification: inputs.json, native-kv.provenance.json
// and native-kv.node for this platform, in a temporary directory (TMPDIR) written on first use
// and removed when the process exits.
#include <string>

namespace uos::tests::helpers::linked_native_kv {
  // The binding as a child process receives it, `JSON.stringify(currentTestNativeKvBinding())`:
  // {"artifactDirectory":..., "expectedProvenanceSha256":..., "inputsManifest":...}.
  const std::string& bindingJson();

  // `initializeTestNativeKv(JSON.parse(text))`: prepareNativeKvBackend with that binding (a
  // parent's bindingJson(), or the pinned artifact's binding in a dev-time run).
  void prepare(const std::string& bindingText);

  // prepare(bindingJson()) unless this process already prepared a backend.
  void prepareOnce();
}
