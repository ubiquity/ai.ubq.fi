#include "uos/runtime/permissions_linux.hpp"

#include "uos/provider/gemini.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/process.hpp"
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
    // Include syscall numbers missing from older ARM64 distribution headers.
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

    std::vector<sock_filter> processFilter(bool allowChildProcess) {
      std::vector<sock_filter> filter;
      const auto statement = [&](std::uint16_t code, std::uint32_t value) { filter.push_back({code, 0, 0, value}); };
      const auto jump = [&](std::uint16_t code, std::uint32_t value, std::uint8_t yes, std::uint8_t no) { filter.push_back({code, yes, no, value}); };
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
      return filter;
    }

    int installProcessFilter(std::vector<sock_filter>& filter) {
      struct sock_fprog program {};
      program.len = static_cast<unsigned short>(filter.size());
      program.filter = filter.data();
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
      if (syscall(__NR_landlock_add_rule, ruleset, netPortRule, &rule, 0) != 0) failure("Landlock TCP port grant");
    }

    int makeFilesystemRuleset(const std::vector<NativeGrant>& grants, const detail::LandlockPolicy& policy) {
      if (grants.size() > 1024) throw rt::TypeError("Confinement requires a bounded grant array");
      int ruleset = static_cast<int>(syscall(__NR_landlock_create_ruleset, &policy.ruleset, policy.rulesetSize, 0));
      if (ruleset < 0) failure("Landlock ruleset creation");
      const auto closeRuleset = rt::scopeExit([&] { if (ruleset >= 0) close(ruleset); });
      for (const NativeGrant& grant : grants) {
        const std::uint32_t access = rt::toUint32(grant.access);
        if (access & ~policy.ruleset.handledAccessFs) throw rt::TypeError("Invalid filesystem grant");
        const int fd = open(boundedPath(grant.path).c_str(), O_PATH | O_CLOEXEC);
        if (fd < 0) failure("Landlock path opening");
        struct landlock_path_beneath_attr rule {};
        rule.allowed_access = access;
        rule.parent_fd = fd;
        const int status = static_cast<int>(syscall(__NR_landlock_add_rule, ruleset, LANDLOCK_RULE_PATH_BENEATH, &rule, 0));
        const int savedErrno = errno;
        close(fd);
        if (status != 0) { errno = savedErrno; failure("Landlock path grant"); }
      }
      return std::exchange(ruleset, -1);
    }

    bool nativeEnforce(const std::vector<NativeGrant>& grants, const detail::LandlockPolicy& policy, const std::optional<LinuxPermissionsTcpPorts>& ports,
                       bool allowChildProcess) {
      // All remaining operations up to restriction are synchronous and create no threads.
      if (policy.restrictFlags == 0) requireSingleThread();
      auto filter = processFilter(allowChildProcess);
      const int ruleset = makeFilesystemRuleset(grants, policy);
      const auto closeRuleset = rt::scopeExit([=] { close(ruleset); });
      if (ports && policy.ruleset.handledAccessNet != 0) {
        addTcpPortRule(ruleset, bindTcp, ports->bindPort);
        for (const std::uint16_t port : ports->connectPorts) addTcpPortRule(ruleset, connectTcp, port);
      }
      if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) failure("No-new-privileges enforcement");
      /* After this irreversible transition any failure terminates this dedicated child. */
      if (syscall(__NR_landlock_restrict_self, ruleset, policy.restrictFlags) != 0) {
        dprintf(STDERR_FILENO, "%s failed: errno %d\n", policy.restrictFlags != 0 ? "Landlock thread synchronization" : "Landlock enforcement", errno);
        _exit(78);
      }
      if (installProcessFilter(filter) != 0) {
        dprintf(STDERR_FILENO, "Seccomp thread synchronization failed: errno %d\n", errno);
        _exit(78);
      }
      return true;
    }
#endif

    NativePermissions linkedPermissions() {
#if defined(__linux__) && defined(__aarch64__)
      return NativePermissions{nativeSupport, nativeReadonlyMount, nativeEnforce};
#else
      throw rt::JsError("Linux confinement requires Linux ARM64");
#endif
    }

    bool beneath(const std::string& parent, const std::string& child) {
      const std::string path = rt::path::relative(parent, child);
      return path.empty() || (path != ".." && !rt::startsWith(path, "../") && !rt::path::isAbsolute(path));
    }

    LinuxPermissionsSupport reportedSupport(const NativeSupport& support) {
      return {std::string(rt::process::arch), rt::os::release(), static_cast<double>(support.landlockAbi), support.landlockAbi >= threadSyncAbi, support.seccomp};
    }

    std::string canonicalize(const std::string& path) {
      if (!rt::path::isAbsolute(path)) throw rt::JsError("Linux confinement paths must be absolute");
      return rt::fs::realpathSync(path);
    }

    std::vector<NativeGrant> filesystemGrants(const LinuxPermissions& permissions, const detail::LandlockPolicy& policy) {
      if (permissions.execute.empty() ? permissions.allowChildProcess : !permissions.allowChildProcess)
        throw rt::TypeError("Executable grants require explicit child-process opt-in");
      rt::OrderedMap<std::string, double> grants;
      const auto add = [&](const std::string& path, double access) {
        const double* granted = grants.get(path);
        grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, access));
      };
      for (const auto& input : permissions.read) {
        const auto path = canonicalize(input);
        const double directoryAccess = rt::fs::statSync(path).isDirectory() ? ((policy.ruleset.handledAccessFs & resolveUnixSocket) != 0 ? 0x10008 : 8) : 0;
        add(path, rt::bitOr(4, directoryAccess));
      }
      for (const auto& input : permissions.execute) {
        const auto path = canonicalize(input);
        if (!rt::fs::statSync(path).isFile()) throw rt::TypeError("Executable confinement grant must name a regular file");
        add(path, 5); // LANDLOCK_ACCESS_FS_EXECUTE | READ_FILE
      }
      for (const auto& input : permissions.write) {
        const auto path = canonicalize(input);
        add(path, rt::fs::statSync(path).isDirectory() ? rt::bitAnd(0x7fff, rt::bitNot(1 | 4 | 8 | 64 | 2048)) : rt::bitOr(2, 0x4000));
      }
      std::vector<NativeGrant> result;
      for (const auto& [path, access] : grants) result.push_back({path, access});
      return result;
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

  LinuxGatewayPermissions gatewayLinuxPermissions(const std::string& package, const std::string& state) {
    namespace fs = std::filesystem;
    LinuxGatewayPermissions result;
    auto& gateway = result.gateway;
    const std::string data = (fs::path(state) / ".data").string();
    gateway.read = {package, state};
    gateway.write = {data};
    gateway.immutable = std::vector<std::string>{package};
    const auto addExisting = [](LinuxPermissions& permissions, std::string_view name) {
      if (fs::exists(name)) permissions.read.push_back(fs::canonical(name).string());
    };
    for (const std::string_view name : {"/usr/lib", "/etc/ssl", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/localtime", "/dev/null",
                                         "/dev/urandom", "/dev/random", "/proc/self"}) {
      addExisting(gateway, name);
    }
    if (!provider::gemini::geminiCliConfigured()) return result;

    const fs::path home = *rt::env::get("HOME");
    const auto cliPath = [&](std::string_view relative) {
      const auto path = home / relative;
      if (fs::canonical(path) != path || beneath(data, path.string()) || beneath(path.string(), data))
        throw rt::JsError("Antigravity inputs must be canonical and separate from writable gateway state");
      return path.string();
    };
    const std::string executable = cliPath(".local/bin/agy");
    const std::string cliState = cliPath(".gemini/antigravity-cli");
    const auto loader = fs::canonical("/lib/ld-linux-aarch64.so.1");
    LinuxPermissions child;
    child.execute = {executable, loader.string()};
    child.allowChildProcess = true;
    child.read = {cliState};
    child.write = {cliState};
    for (const std::string_view name : {"libc.so.6", "libresolv.so.2", "libpthread.so.0", "libdl.so.2", "librt.so.1"}) {
      auto library = fs::path("/usr/lib/aarch64-linux-gnu") / name;
      if (!fs::is_regular_file(library)) library = fs::path("/usr/lib") / name;
      if (!fs::is_regular_file(library)) throw rt::JsError("Antigravity CLI host library is unavailable: " + std::string(name));
      child.read.push_back(fs::canonical(library).string());
    }
    for (const std::string_view name : {"/etc/ssl", "/etc/ca-certificates", "/etc/ld.so.cache", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf",
                                         "/etc/localtime", "/etc/passwd", "/etc/group", "/dev/null", "/dev/urandom", "/dev/random"}) {
      addExisting(child, name);
    }
    for (const std::string_view relative : {".gemini/config", ".config/antigravity"}) {
      const auto path = home / relative;
      if (!fs::exists(path)) continue;
      const auto info = rt::fs::lstatSync(path.string());
      if (info.isDirectory() && !info.isSymbolicLink() && info.uid == rt::process::getuid() && (info.mode & 077) == 0)
        child.read.push_back(cliPath(relative));
    }
    gateway.read.insert(gateway.read.end(), child.read.begin(), child.read.end());
    gateway.write.insert(gateway.write.end(), child.write.begin(), child.write.end());
    gateway.execute = child.execute;
    gateway.allowChildProcess = true;
    result.antigravity = std::move(child);
    return result;
  }

  void restrictLinuxChildThread(const LinuxPermissions& permissions) {
#if defined(__linux__) && defined(__aarch64__)
    auto policy = detail::landlockPolicy(landlockAbi(), true, true);
    // Keep the inherited network rights. Only tighten the child's filesystem,
    // device, signal and abstract socket scope on the supported ABIs.
    policy.ruleset.handledAccessNet = 0;
    const auto grants = filesystemGrants(permissions, policy);
    const int ruleset = makeFilesystemRuleset(grants, policy);
    const auto closeRuleset = rt::scopeExit([=] { close(ruleset); });
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) failure("Child no-new-privileges enforcement");
    if (syscall(__NR_landlock_restrict_self, ruleset, 0) != 0) failure("Child Landlock enforcement");
#else
    static_cast<void>(permissions);
    throw rt::JsError("Linux child confinement requires Linux ARM64");
#endif
  }

#if defined(__linux__) && defined(__aarch64__)
  std::vector<sock_filter> detail::seccompProcessFilter(bool allowChildProcess) { return processFilter(allowChildProcess); }
#endif

  LinuxPermissionsSupport getLinuxPermissionsSupport() {
    return reportedSupport(linkedPermissions().support());
  }

  static LinuxPermissionsReceipt enforcePermissions(const LinuxPermissions& permissions, std::optional<int> abiCap) {
    const NativePermissions native = linkedPermissions();
    const NativeSupport support = native.support();
    const int abi = abiCap ? std::min(*abiCap, support.landlockAbi) : support.landlockAbi;
    const auto policy = detail::landlockPolicy(abi, support.seccomp, permissions.allowReducedConfinement.has_value());
    const auto list = filesystemGrants(permissions, policy);
    std::vector<std::string> immutableMounts;
    for (const auto& input : permissions.immutable.value_or(std::vector<std::string>{})) {
      const auto path = canonicalize(input);
      const bool overlaps = std::any_of(permissions.write.begin(), permissions.write.end(), [&](const std::string& write) {
        const auto parent = canonicalize(write);
        return beneath(parent, path) || beneath(path, parent);
      });
      if (!overlaps) continue;
      if (!native.readonlyMount(path)) throw rt::JsError("Immutable path below writable parent requires a read-only mount");
      immutableMounts.push_back(path);
    }
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
    if (!native.enforce(list, policy, permissions.allowReducedConfinement, permissions.allowChildProcess))
      throw rt::JsError("Native confinement did not produce an enforcement receipt");
    return receipt;
  }

  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions) {
    return enforcePermissions(permissions, std::nullopt);
  }

  LinuxPermissionsReceipt detail::applyLinuxPermissionsWithAbiCap(const LinuxPermissions& permissions, int abiCap) {
    return enforcePermissions(permissions, abiCap);
  }
}
