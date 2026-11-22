#pragma once
// Built-in Linux ARM64 Landlock and seccomp confinement.
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__linux__) && defined(__aarch64__)
#include <linux/filter.h>
#endif

namespace uos::runtime::permissions_linux {
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

  struct ServiceManagerPaths {
    std::string package;
    std::string state;
  };

  struct LinuxPermissions {
    std::vector<std::string> read = {};
    std::vector<std::string> write = {};
    /** An overlapping writable parent requires an actual read-only mount, not chmod or unit text. */
    std::optional<std::vector<std::string>> immutable = std::nullopt;
    /** Exact executable paths. Empty keeps the original no-child-process boundary. */
    std::vector<std::string> execute = {};
    /** Explicitly permits child process creation. Executable grants require this opt-in. */
    bool allowChildProcess = false;
    /** Operator opt-in with TCP grants for kernels that support TCP port restrictions. */
    std::optional<LinuxPermissionsTcpPorts> allowReducedConfinement = std::nullopt;
    /** Operator opt-in with verified mount paths, considered only when Landlock is absent. */
    std::optional<ServiceManagerPaths> allowNoLandlock = std::nullopt;
    /** Read files below these directories, including files created after startup. */
    std::vector<std::string> readFilesBelow = {};
  };

  struct LinuxPermissionsReceipt : LinuxPermissionsSupport {
    bool filesystemThreadSync = false;
    bool processThreadSync = false;
    std::vector<std::string> immutableMounts = {};
    std::vector<std::string> activeProtections = {};
    std::vector<std::string> missingProtections = {};
    std::string_view mode = "landlock";
  };

  struct LinuxGatewayPermissions {
    LinuxPermissions gateway;
    std::optional<LinuxPermissions> antigravity;
  };

  // Compute the service policy without applying it. The optional profile is used
  // to apply an additional restriction to each Antigravity child.
  LinuxGatewayPermissions gatewayLinuxPermissions(const std::string& package, const std::string& state);
  void restrictLinuxChildThread(const LinuxPermissions& permissions);

  /** Query only: this does not restrict the caller or change host configuration. */
  LinuxPermissionsSupport getLinuxPermissionsSupport();

  /**
   * Irreversible, Linux ARM64 application-child boundary. Invoke before product imports or
   * unrelated file opens; Landlock cannot revoke descriptors opened before enforcement or
   * hide file metadata. ABI 8 is required by default. The opt-in admits ABI 3–7 only from
   * single-threaded bootstrap, so every subsequent thread inherits the ruleset.
   * No compiler, dependency installation, FFI fallback, host configuration, or service change
   * runs here. Enforcement failure after an irreversible transition terminates this child.
   */
  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions);

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

#if defined(__linux__) && defined(__aarch64__)
    struct ServiceManagerChecks {
      bool nonRoot;
      bool emptyCapabilities;
      bool noNewPrivileges;
      std::optional<std::string> writableSystemPath = std::nullopt;
      std::optional<std::string> exposedHome = std::nullopt;
      std::optional<std::string> readableHomeSecret = std::nullopt;
    };

    LinuxPermissionsReceipt applyServiceManagerPermissions(const LinuxPermissionsSupport& support, bool allowChildProcess,
                                                          const ServiceManagerChecks& checks);
    std::vector<sock_filter> seccompProcessFilter(bool allowChildProcess);
#endif

    // Lower the real kernel's ABI for a test child. There is no launch-time ABI override.
    LinuxPermissionsReceipt applyLinuxPermissionsWithAbiCap(const LinuxPermissions& permissions, int abiCap);
  }
}
