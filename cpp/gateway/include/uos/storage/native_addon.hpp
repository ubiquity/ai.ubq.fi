#pragma once
// Port of src/storage/native-addon.ts: verification of the native KV artifact's
// release binding, frozen build inputs and hashes, and the process-wide prepared
// backend.
//
// The TS loads the verified `native-kv.node` with `createRequire(import.meta.url)`, a
// Node-API module the C++ gateway cannot host (docs/rt/kv-durable-design.md, option C:
// the backend is linked natively). The load is the replaceable `nativeKvRequire()`; its
// default is the interface of the linked store (rt::SqliteKv, docs/rt/kv-durable.md),
// whose nine exports are present, so preparation still verifies the artifact and checks
// the pinned interface, while storage::kv::openNativeKv() opens the store directly.
#include "uos/rt/async.hpp"
#include "uos/rt/url.hpp"
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace uos::storage::native_addon {
  struct NativeKvArtifactBinding {
    // `string | URL`
    std::variant<std::string, rt::URL> artifactDirectory;
    std::string expectedProvenanceSha256;
    std::optional<std::variant<std::string, rt::URL>> inputsManifest;
  };

  struct VerifiedNativeKvArtifact {
    std::string path;
    std::string artifactSha256;
    std::string provenanceSha256;
    std::string inputsSha256;
  };

  // The `{ bytes, messageId }` that NapiInterface.dequeueNextMessage resolves with.
  struct NapiDequeuedMessage {
    std::vector<std::uint8_t> bytes;
    double messageId = 0;
  };

  // `NapiInterface` of @deno/kv 0.14.0 (esm/napi_based.d.ts): the nine exports of the
  // native backend. An empty function is an export that is not a function.
  struct NapiInterface {
    std::function<double(std::string path, std::optional<bool> inMemory, bool debug)> open;
    std::function<void(double db, bool debug)> close;
    std::function<rt::Promise<std::vector<std::uint8_t>>(double dbId, std::vector<std::uint8_t> snapshotReadBytes, bool debug)> snapshotRead;
    std::function<rt::Promise<std::vector<std::uint8_t>>(double dbId, std::vector<std::uint8_t> atomicWriteBytes, bool debug)> atomicWrite;
    std::function<rt::Promise<std::optional<NapiDequeuedMessage>>(double dbId, bool debug)> dequeueNextMessage;
    std::function<rt::Promise<void>(double dbId, double messageId, bool success, bool debug)> finishMessage;
    std::function<rt::Promise<double>(double dbId, std::vector<std::uint8_t> watchBytes, bool debug)> startWatch;
    std::function<rt::Promise<std::optional<std::vector<std::uint8_t>>>(double dbId, double watchId, bool debug)> dequeueNextWatchMessage;
    std::function<void(double dbId, double watchId, bool debug)> endWatch;
  };

  // Verify independent release binding and frozen build inputs before any native code loads.
  VerifiedNativeKvArtifact verifyNativeKvArtifact(const NativeKvArtifactBinding& binding);

  // The launcher supplies the provenance hash from its independently verified release manifest.
  void prepareNativeKvBackend(const NativeKvArtifactBinding& binding);

  bool nativeKvBackendIsPrepared();

  // Pass the same independently verified binding to task-owned child processes.
  NativeKvArtifactBinding preparedNativeKvArtifactBinding();

  NapiInterface preparedNativeKvBackend();

  // `createRequire(import.meta.url)(path)` as prepareNativeKvBackend uses it: the
  // module exports of the verified artifact at `path`, std::nullopt when they are not
  // an object. Process-wide and replaceable (a test assigns it); the default returns the
  // linked store's interface, whose functions throw rt::JsError if called (nothing in the
  // C++ runtime calls them: the SDK and the store are linked together).
  using NativeKvRequire = std::function<std::optional<NapiInterface>(const std::string& path)>;
  NativeKvRequire& nativeKvRequire();
}
