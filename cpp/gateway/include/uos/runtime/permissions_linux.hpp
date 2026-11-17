#pragma once
// Port of src/runtime/permissions-linux.ts.
//
// The TS loads the Landlock/seccomp addon built from src/runtime/permissions-native.c with
// process.dlopen after verifying it against .uos-permissions/manifest.json. The port keeps every
// check on the manifest and the artifact file, and binds the addon's three functions (support,
// readonlyMount, enforce) to their translation compiled into this binary instead of loading the
// Node-API module. `import.meta.url` is the module's place in the release: the directory of the
// running executable stands for the release root that holds serve.ts.
#include <optional>
#include <string>
#include <vector>

namespace uos::runtime::permissions_linux {
  struct LinuxPermissionsArtifact {
    std::string path = {};
    std::string sha256 = {};
  };

  struct LinuxPermissionsSupport {
    std::string architecture = {};
    std::string kernel = {};
    double landlockAbi = 0;
    bool landlockThreadSync = false;
    bool seccomp = false;
  };

  struct LinuxPermissions {
    std::vector<std::string> read = {};
    std::vector<std::string> write = {};
    /** An overlapping writable parent requires an actual read-only mount, not chmod or unit text. */
    std::optional<std::vector<std::string>> immutable = std::nullopt;
  };

  // `LinuxPermissionsSupport & { filesystemThreadSync: true; processThreadSync: true; immutableMounts: readonly string[] }`
  struct LinuxPermissionsReceipt : LinuxPermissionsSupport {
    bool filesystemThreadSync = true;
    bool processThreadSync = true;
    std::vector<std::string> immutableMounts = {};
  };

  /** Query only: this does not restrict the caller or change host configuration. */
  LinuxPermissionsSupport getLinuxPermissionsSupport(const std::optional<LinuxPermissionsArtifact>& artifact = std::nullopt);

  /**
   * Irreversible, Linux ARM64 application-child boundary. Invoke before product imports or
   * unrelated file opens; Landlock cannot revoke descriptors opened before enforcement or
   * hide file metadata. ABI8 and newer are accepted; actual ARM64/JSC/native-KV proof records the runner ABI.
   * The native artifact must already be built, hashed and packaged in the immutable release.
   * No compiler, dependency installation, FFI fallback, host configuration, or service change
   * runs here. Enforcement failure after an irreversible transition terminates this child.
   */
  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions, const std::optional<LinuxPermissionsArtifact>& artifact = std::nullopt);
}
