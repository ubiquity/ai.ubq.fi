#pragma once
// Port of src/runtime/permissions-linux.ts.
//
// The TS loads the Landlock/seccomp addon built from src/runtime/permissions-native.c with
// process.dlopen after verifying it against .uos-permissions/manifest.json. The port keeps every
// check on the manifest and the artifact file, and binds the addon's three functions (support,
// readonlyMount, enforce) to their translation compiled into this binary instead of loading the
// Node-API module. `import.meta.url` is the module's place in the release: the directory of the
// running executable stands for the release root that holds serve.ts.
#include <cstddef>
#include <cstdint>
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

  struct LinuxPermissionsTcpPorts {
    std::uint16_t bindPort = 0;
    std::vector<std::uint16_t> connectPorts = {};
  };

  struct LinuxPermissions {
    std::vector<std::string> read = {};
    std::vector<std::string> write = {};
    /** An overlapping writable parent requires an actual read-only mount, not chmod or unit text. */
    std::optional<std::vector<std::string>> immutable = std::nullopt;
    /** Exact executable paths. Empty keeps the original no-child-process boundary. */
    std::vector<std::string> execute = {};
    /** Operator opt-in with TCP grants for kernels that support TCP port restrictions. */
    std::optional<LinuxPermissionsTcpPorts> allowReducedConfinement = std::nullopt;
  };

  struct LinuxPermissionsReceipt : LinuxPermissionsSupport {
    bool filesystemThreadSync = false;
    bool processThreadSync = false;
    std::vector<std::string> immutableMounts = {};
    std::vector<std::string> activeProtections = {};
    std::vector<std::string> missingProtections = {};
  };

  /** Query only: this does not restrict the caller or change host configuration. */
  LinuxPermissionsSupport getLinuxPermissionsSupport(const std::optional<LinuxPermissionsArtifact>& artifact = std::nullopt);

  /**
   * Irreversible, Linux ARM64 application-child boundary. Invoke before product imports or
   * unrelated file opens; Landlock cannot revoke descriptors opened before enforcement or
   * hide file metadata. ABI 8 is required by default. The opt-in admits ABI 3–7 only from
   * single-threaded bootstrap, so every subsequent thread inherits the ruleset.
   * The native artifact must already be built, hashed and packaged in the immutable release.
   * No compiler, dependency installation, FFI fallback, host configuration, or service change
   * runs here. Enforcement failure after an irreversible transition terminates this child.
   */
  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions, const std::optional<LinuxPermissionsArtifact>& artifact = std::nullopt);

  namespace detail {
    struct LandlockRuleset {
      std::uint64_t handledAccessFs;
      std::uint64_t handledAccessNet;
      std::uint64_t scoped;
    };

    struct LandlockPolicy {
      LandlockRuleset ruleset;
      std::size_t rulesetSize;
      std::uint32_t restrictFlags;
    };

    LandlockPolicy landlockPolicy(int abi, bool seccomp, bool allowReduced);

    // Lower the real kernel's ABI for a test child. There is no launch-time ABI override.
    LinuxPermissionsReceipt applyLinuxPermissionsWithAbiCap(const LinuxPermissions& permissions, const LinuxPermissionsArtifact& artifact, int abiCap);
  }
}
