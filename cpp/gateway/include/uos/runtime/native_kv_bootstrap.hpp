#pragma once
// Port of src/runtime/native-kv-bootstrap.ts: development startup reads an explicit,
// owner-private operator receipt that binds the native KV artifact beneath the
// repository's .data, verifies it, and prepares the backend.
#include "uos/rt/async.hpp"
#include "uos/storage/native_addon.hpp"
#include <optional>
#include <string>

namespace uos::runtime::native_kv_bootstrap {
  // What nativeKvDevelopmentPaths returns.
  struct NativeKvDevelopmentPaths {
    std::string root;
    std::string data;
    std::string receipt;
    std::string inputs;
  };

  // Resolve from this source module, including when a caller starts in another cwd.
  // std::nullopt is the default, the repository root of this source module.
  NativeKvDevelopmentPaths nativeKvDevelopmentPaths(const std::optional<std::string>& repositoryRoot = std::nullopt);

  // Permissions protect local operator custody, not hostile changes by the same uid.
  void verifyNativeKvDevelopmentDirectory(const std::string& path);

  // Read a trusted local operator receipt; never infer trust from adjacent provenance.
  storage::native_addon::NativeKvArtifactBinding readNativeKvDevelopmentBinding(const std::optional<std::string>& repositoryRoot = std::nullopt);

  // Development startup only verifies and loads; explicit preparation happens separately.
  [[nodiscard]] rt::Promise<void> prepareNativeKvForDevelopment(std::optional<std::string> repositoryRoot = std::nullopt);
}
