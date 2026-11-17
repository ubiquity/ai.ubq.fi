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
#include "uos/rt/url.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
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
    // The Node-API error the addon throws: an Error whose `code` is LINUX_CONFINEMENT_UNAVAILABLE.
    struct NativeConfinementError : rt::JsError {
      std::string code;
      NativeConfinementError(std::string message_, std::string code_) : rt::JsError(std::move(message_)), code(std::move(code_)) {}
    };

    struct NativeSupport {
      double landlockAbi = 0;
      bool seccomp = false;
    };
    struct NativeGrant {
      std::string path;
      double access = 0;
    };
    struct NativePermissions {
      std::function<NativeSupport()> support;
      std::function<bool(const std::string& path)> readonlyMount;
      std::function<bool(const std::vector<NativeGrant>& grants)> enforce;
    };

#if defined(__linux__) && defined(__aarch64__)
    // src/runtime/permissions-native.c, without its Node-API glue.
    // Primary ABI: docs.kernel.org/userspace-api/{landlock,seccomp_filter}.html.
#ifndef LANDLOCK_RESTRICT_SELF_TSYNC
#define LANDLOCK_RESTRICT_SELF_TSYNC (1U << 3)
#endif
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
      return NativeSupport{static_cast<double>(abi), available};
    }

    bool nativeReadonlyMount(const std::string& path) {
      const std::string& bounded = boundedPath(path);
      struct statfs filesystem {};
      const int status = statfs(bounded.c_str(), &filesystem);
      if (status != 0) failure("Immutable mount inspection");
      return (filesystem.f_flags & 1) != 0;
    }

    int installProcessFilter() {
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
      deny(__NR_execve);
      deny(__NR_execveat);
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
      jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_clone, 0, 3);
      statement(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, args[0]));
      jump(BPF_JMP | BPF_JSET | BPF_K, CLONE_THREAD, 1, 0);
      statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM);
      statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
      struct sock_fprog program {};
      program.len = static_cast<unsigned short>(length);
      program.filter = filter;
      return static_cast<int>(syscall(__NR_seccomp, SECCOMP_SET_MODE_FILTER, SECCOMP_FILTER_FLAG_TSYNC, &program));
    }

    bool nativeEnforce(const std::vector<NativeGrant>& grants) {
      if (grants.size() > 1024) throw rt::TypeError("Confinement requires a bounded grant array");
      const int abi = landlockAbi();
      if (abi < 8) {
        errno = ENOTSUP;
        failure("Supported Landlock ABI8+");
      }
      struct landlock_ruleset_attr attributes {};
      attributes.handled_access_fs = 0x7fffULL | (abi >= 9 ? (1ULL << 16) : 0);
      const int ruleset = static_cast<int>(syscall(__NR_landlock_create_ruleset, &attributes, sizeof(std::uint64_t), 0));
      if (ruleset < 0) failure("Landlock ruleset creation");
      for (const NativeGrant& grant : grants) {
        // napi_get_value_uint32: the access number as a 32-bit unsigned integer.
        const std::uint32_t access = rt::toUint32(grant.access);
        if ((access & 1) || (access & ~attributes.handled_access_fs)) {
          close(ruleset);
          throw rt::TypeError("Invalid non-executable filesystem grant");
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
      if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        close(ruleset);
        failure("No-new-privileges enforcement");
      }
      /* After this irreversible transition any failure terminates this dedicated child. */
      if (syscall(__NR_landlock_restrict_self, ruleset, LANDLOCK_RESTRICT_SELF_TSYNC) != 0) {
        dprintf(STDERR_FILENO, "Landlock thread synchronization failed: errno %d\n", errno);
        _exit(78);
      }
      close(ruleset);
      if (installProcessFilter() != 0) {
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

    std::string moduleUrl() { return rt::pathToFileURL(rt::path::dirname(rt::process::execPath()) + "/src/runtime/permissions-linux.ts").href(); }

    LinuxPermissionsArtifact defaultArtifact() {
      const std::string manifest = rt::fileURLToPath(rt::URL("../../.uos-permissions/manifest.json", moduleUrl()));
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
      return LinuxPermissionsArtifact{rt::path::join(rt::path::dirname(manifest), std::string_view(path->get_string())), std::string(sha256->get_string())};
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

    void requireConfinementSupport(const LinuxPermissionsSupport& support) {
      if (!support.landlockThreadSync || !support.seccomp) {
        throw rt::JsError("Linux confinement requires Landlock ABI8+ and seccomp thread synchronization");
      }
    }
  }

  LinuxPermissionsSupport getLinuxPermissionsSupport(const std::optional<LinuxPermissionsArtifact>& artifact) {
    const NativeSupport support = nativeLibrary(artifact).support();
    return LinuxPermissionsSupport{std::string(rt::process::arch), rt::os::release(), support.landlockAbi, support.landlockAbi >= 8, support.seccomp};
  }

  LinuxPermissionsReceipt applyLinuxPermissions(const LinuxPermissions& permissions, const std::optional<LinuxPermissionsArtifact>& artifact) {
    const NativePermissions native = nativeLibrary(artifact);
    const LinuxPermissionsSupport support = getLinuxPermissionsSupport(artifact);
    requireConfinementSupport(support);
    const auto canonicalize = [](const std::string& path) -> std::string {
      if (!rt::path::isAbsolute(path)) throw rt::JsError("Linux confinement paths must be absolute");
      return rt::fs::realpathSync(path);
    };
    std::vector<std::string> readPaths;
    for (const std::string& path : permissions.read) readPaths.push_back(canonicalize(path));
    std::vector<std::string> writePaths;
    for (const std::string& path : permissions.write) writePaths.push_back(canonicalize(path));
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
      if (rt::fs::statSync(path).isDirectory()) directoryAccess = rt::bitOr(8, support.landlockAbi >= 9 ? 0x10000 : 0);
      const double readable = rt::bitOr(4, directoryAccess);
      const double* granted = grants.get(path);
      grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, readable));
    }
    for (const std::string& path : writePaths) {
      const double writable = rt::fs::statSync(path).isDirectory() ? rt::bitAnd(0x7fff, rt::bitNot(1 | 4 | 8 | 64 | 2048)) : rt::bitOr(2, 0x4000);
      const double* granted = grants.get(path);
      grants.set(path, rt::bitOr(granted != nullptr ? *granted : 0, writable));
    }
    std::vector<NativeGrant> list;
    for (const auto& [path, access] : grants) list.push_back(NativeGrant{path, access});
    if (!native.enforce(list)) throw rt::JsError("Native confinement did not produce an enforcement receipt");
    LinuxPermissionsReceipt receipt;
    static_cast<LinuxPermissionsSupport&>(receipt) = support;
    receipt.filesystemThreadSync = true;
    receipt.processThreadSync = true;
    receipt.immutableMounts = std::move(immutableMounts);
    return receipt;
  }
}
