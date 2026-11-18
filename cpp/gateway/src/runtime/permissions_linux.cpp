#include "uos/runtime/permissions_linux.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <utility>

#if defined(__linux__) && defined(__aarch64__)
#include <filesystem>
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/sched.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace uos::runtime::permissions_linux {
  namespace {
    constexpr int minimumLandlockAbi = 3;
    constexpr int threadSyncAbi = 8;
    // Landlock UAPI values, including fields absent from older distribution headers.
    constexpr std::uint64_t baseFilesystemAccess = 0x7fffULL;
    constexpr std::uint64_t deviceIoctl = 1ULL << 15;
    constexpr std::uint64_t resolveUnixSocket = 1ULL << 16;
    constexpr std::uint64_t bindTcp = 1ULL << 0;
    constexpr std::uint64_t connectTcp = 1ULL << 1;
    constexpr std::uint64_t scopeAbstractUnixSocket = 1ULL << 0;
    constexpr std::uint64_t scopeSignal = 1ULL << 1;
    constexpr std::uint32_t restrictThreadSync = 1U << 3;

    // The Node-API error the addon throws: an Error whose `code` is LINUX_CONFINEMENT_UNAVAILABLE.
    struct NativeConfinementError : rt::JsError {
      std::string code;
      NativeConfinementError(std::string message_, std::string code_) : rt::JsError(std::move(message_)), code(std::move(code_)) {}
    };

    struct NativeSupport {
      int landlockAbi = 0;
      bool seccomp = false;
    };
    struct NativeGrant {
      std::string path;
      double access = 0;
    };
    struct NativePermissions {
      std::function<NativeSupport()> support;
      std::function<bool(const std::string& path)> readonlyMount;
      std::function<bool(const std::vector<NativeGrant>&, const detail::LandlockPolicy&, const std::optional<LinuxPermissionsTcpPorts>&, bool allowChildProcess)>
        enforce;
    };

#if defined(__linux__) && defined(__aarch64__)
    // src/runtime/permissions-native.c, without its Node-API glue.
    // Primary ABI: docs.kernel.org/userspace-api/{landlock,seccomp_filter}.html.
    // The asm-generic numbers the release artifact compiles in from current kernel headers.
#ifndef __NR_fchmodat2
#define __NR_fchmodat2 452
#endif
#ifndef __NR_setxattrat
#define __NR_setxattrat 463
#endif
#ifndef __NR_removexattrat
#define __NR_removexattrat 466
#endif
#ifndef __NR_file_setattr
#define __NR_file_setattr 469
#endif

    [[noreturn]] void failure(const char* operation) {
      char message[192];
      std::snprintf(message, sizeof(message), "%s failed: errno %d", operation, errno);
      throw NativeConfinementError(message, "LINUX_CONFINEMENT_UNAVAILABLE");
    }

    const std::string& boundedPath(const std::string& path) {
      if (path.size() > 4096) throw rt::TypeError("Confinement path must be a bounded string");
      return path;
    }

    int landlockAbi() { return static_cast<int>(syscall(__NR_landlock_create_ruleset, nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION)); }

    NativeSupport nativeSupport() {
      const int abi = landlockAbi();
      if (abi < 0) failure("Landlock ABI query");
      std::uint32_t action = SECCOMP_RET_ERRNO;
      const bool available = syscall(__NR_seccomp, SECCOMP_GET_ACTION_AVAIL, 0, &action) == 0;
      return NativeSupport{abi, available};
    }

    bool nativeReadonlyMount(const std::string& path) {
      const std::string& bounded = boundedPath(path);
      struct statfs filesystem {};
      const int status = statfs(bounded.c_str(), &filesystem);
      if (status != 0) failure("Immutable mount inspection");
      return (filesystem.f_flags & 1) != 0;
    }

    int installProcessFilter(bool allowChildProcess) {
      struct sock_filter filter[128];
      std::size_t length = 0;
      const auto statement = [&](std::uint16_t code, std::uint32_t value) { filter[length++] = sock_filter{code, 0, 0, value}; };
      const auto jump = [&](std::uint16_t code, std::uint32_t value, std::uint8_t yes, std::uint8_t no) { filter[length++] = sock_filter{code, yes, no, value}; };
      const auto deny = [&](long number) {
        jump(BPF_JMP | BPF_JEQ | BPF_K, static_cast<std::uint32_t>(number), 0, 1);
        statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
      };
      statement(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, arch));
      jump(BPF_JMP | BPF_JEQ | BPF_K, AUDIT_ARCH_AARCH64, 1, 0);
      statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
      statement(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr));
      /* clone3's pointer cannot be inspected by BPF. glibc's ENOSYS fallback uses clone. */
      jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone3, 0, 1);
      statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | ENOSYS);
      if (!allowChildProcess) {
        deny(__NR_execve);
        deny(__NR_execveat);
      }
      deny(__NR_fchmod);
      deny(__NR_fchmodat);
      deny(__NR_fchownat);
      deny(__NR_fchown);
      deny(__NR_utimensat);
      deny(__NR_setxattr);
      deny(__NR_lsetxattr);
      deny(__NR_fsetxattr);
      deny(__NR_removexattr);
      deny(__NR_lremovexattr);
      deny(__NR_fremovexattr);
      deny(__NR_ptrace);
      deny(__NR_process_vm_readv);
      deny(__NR_process_vm_writev);
      deny(__NR_fchmodat2);
      deny(__NR_setxattrat);
      deny(__NR_removexattrat);
      deny(__NR_file_setattr);
      if (!allowChildProcess) {
        jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 3);
        statement(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
        jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0);
        statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
      }
      statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
      struct sock_fprog program {};
      program.len = static_cast<unsigned short>(length);
      program.filter = filter;
      return static_cast<int>(syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program));
    }

    void requireSingleThread() {
      const pid_t pid = getpid();
      auto thread = std::filesystem::directory_iterator("/proc/self/task");
      const auto end = std::filesystem::directory_iterator();
      if (syscall(__NR_gettid) != pid || thread == end || thread->path().filename().native() != std::to_string(pid) || ++thread != end)
        throw rt::JsError("Reduced Landlock confinement requires exactly one thread during bootstrap");
    }

    void addTcpPortRule(int ruleset, std::uint64_t access, std::uint16_t port) {
      constexpr int netPortRule = 2; // LANDLOCK_RULE_NET_PORT
      const struct {
        std::uint64_t allowedAccess;
        std::uint64_t port;
      } rule{access, port};
      if (syscall(__NR_landlock_add_rule, ruleset, netPortRule, &rule, 0) != 0) {
        const int savedErrno = errno;
        close(ruleset);
        errno = savedErrno;
        failure("Landlock TCP port grant");
      }
    }

    bool nativeEnforce(const std::vector<NativeGrant>& grants, const detail::LandlockPolicy& policy, const std::optional<LinuxPermissionsTcpPorts>& ports,
                       bool allowChildProcess) {
      if (grants.size() > 1024) throw rt::TypeError("Confinement requires a bounded grant array");
      // All remaining operations up to restriction are synchronous and create no threads.
      if (policy.restrictFlags == 0) requireSingleThread();
      const int ruleset = static_cast<int>(syscall(__NR_landlock_create_ruleset, &policy.ruleset, policy.rulesetSize, 0));
      if (ruleset < 0) failure("Landlock ruleset creation");
      for (const NativeGrant& grant : grants) {
        // napi_get_value_uint32: the access number as a 32-bit unsigned integer.
        const std::uint32_t access = rt::toUint32(grant.access);
        if (access & ~policy.ruleset.handledAccessFs) {
          close(ruleset);
          throw rt::TypeError("Invalid filesystem grant");
        }
        if (grant.path.size() > 4096) {
          close(ruleset);
          throw rt::TypeError("Confinement path must be a bounded string");
        }
        const int fd = open(grant.path.c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) {
          close(ruleset);
          failure("Landlock path opening");
        }
        struct landlock_path_beneath_attr rule {};
        rule.allowed_access = access;
        rule.parent_fd = fd;
        const int status = static_cast<int>(syscall(__NR_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
        const int savedErrno = errno;
        close(fd);
        if (status != 0) {
          close(ruleset);
          errno = savedErrno;
          failure("Landlock path grant");
        }
      }
      if (ports && policy.ruleset.handledAccessNet != 0) {
        addTcpPortRule(ruleset, bindTcp, ports->bindPort);
        for (const std::uint16_t port : ports->connectPorts) addTcpPortRule(ruleset, connectTcp, port);
      }
      if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        close(ruleset);
        failure("No-new-privileges enforcement");
      }
      /* After this irreversible transition any failure terminates this dedicated child. */
      if (syscall(__NR_landlock_restrict_self, ruleset, policy.restrictFlags) != 0) {
        dprintf(STDERR_FILENO, "%s failed: errno %d\n", policy.restrictFlags != 0 ? "Landlock thread synchronization" : "Landlock enforcement", errno);
        _exit(78);
      }
      close(ruleset);
      if (installProcessFilter(allowChildProcess) != 0) {
        dprintf(STDERR_FILENO, "Seccomp thread synchronization failed: errno %d\n", errno);
        _exit(78);
      }
      return true;
    }
#endif

    // The addon's exports as `process.dlopen` would expose them.
    NativePermissions builtInNativePermissions() {
#if defined(__linux__) && defined(__aarch64__)
      return NativePermissions{nativeSupport, nativeReadonlyMount, nativeEnforce};
#else
      return NativePermissions{};
#endif
    }

    rt::OrderedMap<std::string, NativePermissions>& loaded() {
      static rt::OrderedMap<std::string, NativePermissions> cache;
      return cache;
    }

    LinuxPermissionsArtifact defaultArtifact() {
      const auto directory = std::filesystem::path(rt::process::execPath()).parent_path() / ".uos-permissions";
      const auto manifest = (directory / "manifest.json").native();
      const rt::fs::Stats info = rt::fs::lstatSync(manifest);
      if (!info.isFile() || info.isSymbolicLink()) throw rt::JsError("Native confinement manifest is not a regular file");
      const json::value document = rt::jsonParse(rt::fs::readFileSync(manifest, "utf8"));
      const json::object* object = document.if_object();
      const json::value* path = object != nullptr ? rt::get(*object, "path") : nullptr;
      const json::value* architecture = object != nullptr ? rt::get(*object, "architecture") : nullptr;
      const json::value* sha256 = object != nullptr ? rt::get(*object, "sha256") : nullptr;
      if (document.is_null() || !(document.is_object() || document.is_array()) || path == nullptr || !path->is_string() || path->get_string() != "linux-arm64.node" ||
          architecture == nullptr || !architecture->is_string() || architecture->get_string() != "arm64" || sha256 == nullptr || !sha256->is_string())
        throw rt::JsError("Native confinement manifest is invalid");
      return LinuxPermissionsArtifact{(directory / "linux-arm64.node").native(), std::string(sha256->get_string())};
    }

    NativePermissions nativeLibrary(const std::optional<LinuxPermissionsArtifact>& artifact) {
      if (rt::process::platform != "linux" || rt::process::arch != "arm64") throw rt::JsError("Linux confinement requires Linux ARM64");
      const LinuxPermissionsArtifact selected = artifact ? *artifact : defaultArtifact();
      static const rt::Regex SHA256("^[0-9a-f]{64}$");
      const rt::fs::Stats info = rt::fs::lstatSync(selected.path);
      if (!info.isFile() || info.isSymbolicLink() || !SHA256.test(selected.sha256)) throw rt::JsError("Native confinement artifact is invalid");
      if (rt::crypto::createHash("sha256").update(rt::fs::readFileSync(selected.path)).digest("hex") != selected.sha256)
        throw rt::JsError("Native confinement artifact integrity check failed");
      const std::string key = rt::fs::realpathSync(selected.path) + ":" + selected.sha256;
      if (const NativePermissions* cached = loaded().get(key)) return *cached;
      const NativePermissions api = builtInNativePermissions();
      if (!api.support || !api.readonlyMount || !api.enforce) throw rt::JsError("Native confinement interface is invalid");
      loaded().set(key, api);
      return api;
    }

    bool beneath(const std::string& parent, const std::string& child) {
      const std::string path = rt::path::relative(parent, child);
      return path.empty() || (path != ".." && !rt::startsWith(path, "../") && !rt::path::isAbsolute(path));
    }

    LinuxPermissionsSupport reportedSupport(const NativeSupport& support) {
      return {std::string(rt::process::arch), rt::os::release(), static_cast<double>(support.landlockAbi), support.landlockAbi >= threadSyncAbi, support.seccomp};
    }
  }

  detail::LandlockPolicy detail::landlockPolicy(int abi, bool seccomp, bool allowReduced) {
    if (!seccomp || abi < (allowReduced ? minimumLandlockAbi : threadSyncAbi))
      throw rt::JsError(allowReduced ? "Reduced Linux confinement requires Landlock ABI3+ and seccomp thread synchronization"
                                     : "Linux confinement requires Landlock ABI8+ and seccomp thread synchronization");
    LandlockPolicy policy{{baseFilesystemAccess, 0, 0}, sizeof(std::uint64_t), abi >= threadSyncAbi ? restrictThreadSync : 0};
    if (abi >= 9) policy.ruleset.handledAccessFs |= resolveUnixSocket;
    if (allowReduced && abi >= 4) {
      policy.ruleset.handledAccessNet = bindTcp | connectTcp;
      policy.rulesetSize = offsetof(LandlockRuleset, scoped);
    }
    if (allowReduced && abi >= 5) policy.ruleset.handledAccessFs |= deviceIoctl;
    if (allowReduced && abi >= 6) {
      policy.ruleset.scoped = scopeAbstractUnixSocket | scopeSignal;
      policy.rulesetSize = sizeof(LandlockRuleset);
    }
    return policy;
  }

  LinuxPermissionsSupport getLinuxPermissionsSupport(const std::optional<LinuxPermissionsArtifact>& artifact) {
    return reportedSupport(nativeLibrary(artifact).support());
  }

  static LinuxPermissionsReceipt apply(const LinuxPermissions& permissions, const std::optional<LinuxPermissionsArtifact>& artifact, std::optional<int> abiCap) {
    const NativePermissions native = nativeLibrary(artifact);
    const NativeSupport support = native.support();
    const int abi = abiCap ? std::min(*abiCap, support.landlockAbi) : support.landlockAbi;
    const auto policy = detail::landlockPolicy(abi, support.seccomp, permissions.allowReducedConfinement.has_value());
    const auto canonicalize = [](const std::string& path) -> std::string {
      if (!rt::path::isAbsolute(path)) throw rt::JsError("Linux confinement paths must be absolute");
      return rt::fs::realpathSync(path);
    };
    std::vector<std::string> readPaths;
    for (const std::string& path : permissions.read) readPaths.push_back(canonicalize(path));
    std::vector<std::string> writePaths;
    for (const std::string& path : permissions.write) writePaths.push_back(canonicalize(path));
    std::vector<std::string> executePaths;
    for (const std::string& path : permissions.execute) executePaths.push_back(canonicalize(path));
    std::vector<std::string> immutablePaths;
    for (const std::string& path : permissions.immutable.value_or(std::vector<std::string>{})) immutablePaths.push_back(canonicalize(path));
    std::vector<std::string> immutableMounts;
    for (const std::string& path : immutablePaths) {
      bool overlaps = false;
      for (const std::string& parent : writePaths) overlaps = overlaps || beneath(parent, path) || beneath(path, parent);
      if (!overlaps) continue;
      if (!native.readonlyMount(path)) throw rt::JsError("Immutable path below writable parent requires a read-only mount");
      immutableMounts.push_back(path);
    }
    rt::OrderedMap<std::string, double> grants;
    for (const std::string& path : readPaths) {
      double directoryAccess = 0;
      if (rt::fs::statSync(path).isDirectory()) directoryAccess = rt::bitOr(8, (policy.ruleset.handledAccessFs & resolveUnixSocket) != 0 ? 0x10000 : 0);
      const double readable = rt::bitOr(4, directoryAccess);
      const double* granted = grants.get(path);
      grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, readable));
    }
    for (const std::string& path : executePaths) {
      if (rt::fs::statSync(path).isDirectory()) throw rt::TypeError("Executable confinement grant must name a file");
      const double* granted = grants.get(path);
      grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, 5)); // LANDLOCK_ACCESS_FS_EXECUTE | READ_FILE
    }
    for (const std::string& path : writePaths) {
      const double writable = rt::fs::statSync(path).isDirectory() ? rt::bitAnd(0x7fff, rt::bitNot(1 | 4 | 8 | 64 | 2048)) : rt::bitOr(2, 0x4000);
      const double* granted = grants.get(path);
      grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, writable));
    }
    std::vector<NativeGrant> list;
    for (const auto& [path, access] : grants) list.push_back(NativeGrant{path, access});
    LinuxPermissionsReceipt receipt;
    static_cast<LinuxPermissionsSupport&>(receipt) = reportedSupport(support);
    receipt.filesystemThreadSync = policy.restrictFlags != 0;
    receipt.processThreadSync = true;
    receipt.immutableMounts = std::move(immutableMounts);
    receipt.activeProtections = {"landlock-filesystem", "seccomp-tsync"};
    if (!receipt.filesystemThreadSync) receipt.activeProtections.emplace_back("landlock-thread-inheritance");
    const auto report = [&](bool active, const char* name) { (active ? receipt.activeProtections : receipt.missingProtections).emplace_back(name); };
    report(receipt.filesystemThreadSync, "landlock-tsync");
    report(policy.ruleset.handledAccessNet != 0, "tcp-ports");
    report((policy.ruleset.handledAccessFs & deviceIoctl) != 0, "device-ioctl");
    report((policy.ruleset.scoped & scopeAbstractUnixSocket) != 0, "abstract-unix-sockets");
    report((policy.ruleset.scoped & scopeSignal) != 0, "signal-scope");
    report((policy.ruleset.handledAccessFs & resolveUnixSocket) != 0, "pathname-unix-sockets");
    if (!native.enforce(list, policy, permissions.allowReducedConfinement, !executePaths.empty()))
      throw rt::JsError("Native confinement did not produce an enforcement receipt");
    return receipt;
  }

  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions, const std::optional<LinuxPermissionsArtifact>& artifact) {
    return apply(permissions, artifact, std::nullopt);
  }

  LinuxPermissionsReceipt detail::applyLinuxPermissionsWithAbiCap(const LinuxPermissions& permissions, const LinuxPermissionsArtifact& artifact, int abiCap) {
    return apply(permissions, artifact, abiCap);
  }
}
