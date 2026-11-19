// src/runtime/permissions-linux.ts has no bun test of its own (tests/codex-supervisor-inventory-coverage.test.ts
// only applies it as the fixture of unrelated cases). These cases cover its checks on the manifest and
// the artifact, the support query, the refusals before the irreversible step, and the confinement
// itself in a forked child (the parent is never confined).
#include "uos/runtime/permissions_linux.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace rt = uos::rt;
using namespace uos::runtime::permissions_linux;

namespace {
  struct Fixture {
    std::string root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-permissions-");
    std::string artifactPath = root + "/linux-arm64.node";
    LinuxPermissionsArtifact artifact;
    Fixture() {
      rt::fs::writeFileSync(artifactPath, std::string_view("fixture artifact bytes"));
      artifact = {artifactPath, rt::crypto::createHash("sha256").update(rt::fs::readFileSync(artifactPath)).digest("hex")};
    }
    ~Fixture() { rt::fs::rmSync(root, {.recursive = true, .force = true}); }
  };

  std::string messageOf(const std::function<void()>& call) {
    try {
      call();
    } catch (const rt::JsError& error) {
      return error.message;
    }
    return "<no error>";
  }
}

TEST("the default artifact comes from the release manifest beside the executable") {
#if defined(__APPLE__)
  EXPECT_EQ(messageOf([] { getLinuxPermissionsSupport(); }), "Linux confinement requires Linux ARM64");
#else
  // Under tools/unit.sh the executable is the SDK's dynamic loader, whose directory has no manifest.
  bool threw = false;
  try {
    getLinuxPermissionsSupport();
  } catch (const rt::fs::ErrnoError& error) {
    threw = true;
    EXPECT_EQ(error.code, "ENOENT");
    EXPECT_EQ(error.syscall, "lstat");
    EXPECT(error.path && error.path->ends_with("/.uos-permissions/manifest.json"));
  }
  EXPECT(threw);
#endif
}

TEST("default confinement inputs resolve beside an executable under a reserved-byte directory") {
#if defined(__linux__) && defined(__aarch64__)
  const std::string self = rt::process::execPath();
  const std::string name = std::filesystem::path(self).filename().string();
  if (name == "permissions-path-probe") {
    EXPECT_EQ(getLinuxPermissionsSupport().architecture, "arm64");
    return;
  }
  // A direct executable re-launch requires the native fixture profile.
  if (name.starts_with("ld-linux")) return;

  const Fixture fixture;
  const std::string bin = fixture.root + "/bin %?#";
  const std::string directory = bin + "/.uos-permissions";
  rt::fs::mkdirSync(directory, {.recursive = true});
  rt::fs::copyFileSync(fixture.artifactPath, directory + "/linux-arm64.node");
  rt::fs::writeFileSync(directory + "/manifest.json",
                        R"({"path":"linux-arm64.node","architecture":"arm64","sha256":")" + fixture.artifact.sha256 + "\"}");
  const std::string executable = bin + "/permissions-path-probe";
  rt::fs::copyFileSync(self, executable);
  rt::fs::chmodSync(executable, 0700);
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    ::execl(executable.c_str(), executable.c_str(), "default confinement inputs resolve", static_cast<char*>(nullptr));
    ::_exit(127);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  ASSERT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

TEST("the artifact must be a regular file whose sha256 matches") {
  const Fixture fixture;
#if defined(__APPLE__)
  for (const std::string& path : {fixture.root + "/absent.node", fixture.root, fixture.artifactPath}) {
    EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{path, fixture.artifact.sha256}); }), "Linux confinement requires Linux ARM64");
    EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{path, std::string(64, '0')}); }), "Linux confinement requires Linux ARM64");
  }
#else
  bool missing = false;
  try {
    getLinuxPermissionsSupport(LinuxPermissionsArtifact{fixture.root + "/absent.node", fixture.artifact.sha256});
  } catch (const rt::fs::ErrnoError& error) {
    missing = error.code == "ENOENT";
  }
  EXPECT(missing);
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{fixture.root, fixture.artifact.sha256}); }), "Native confinement artifact is invalid");
  ASSERT(::symlink(fixture.artifactPath.c_str(), (fixture.root + "/link.node").c_str()) == 0);
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{fixture.root + "/link.node", fixture.artifact.sha256}); }),
            "Native confinement artifact is invalid");
  std::string upper = fixture.artifact.sha256;
  for (char& character : upper) character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{fixture.artifactPath, upper}); }), "Native confinement artifact is invalid");
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(LinuxPermissionsArtifact{fixture.artifactPath, std::string(64, '0')}); }),
            "Native confinement artifact integrity check failed");
#endif
}

TEST("the support query reports the kernel's Landlock ABI and seccomp availability") {
  const Fixture fixture;
#if defined(__APPLE__)
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(fixture.artifact); }), "Linux confinement requires Linux ARM64");
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(fixture.artifact); }), "Linux confinement requires Linux ARM64");
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport(fixture.artifact);
  EXPECT_EQ(support.architecture, "arm64");
  EXPECT_EQ(support.kernel, rt::os::release());
  EXPECT(support.landlockAbi >= 1);
  EXPECT_EQ(support.landlockThreadSync, support.landlockAbi >= 8);
  EXPECT(support.seccomp);
  std::printf("     host: landlockAbi %g, seccomp %s\n", support.landlockAbi, support.seccomp ? "true" : "false");
  const LinuxPermissionsSupport again = getLinuxPermissionsSupport(fixture.artifact);
  EXPECT_EQ(again.landlockAbi, support.landlockAbi);
#endif
}

TEST("refusals before the irreversible step leave the process unconfined") {
  const Fixture fixture;
#if defined(__APPLE__)
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{"relative/path"}, {fixture.root}}, fixture.artifact); }), "Linux confinement requires Linux ARM64");
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}, std::vector<std::string>{fixture.root}}, fixture.artifact); }),
            "Linux confinement requires Linux ARM64");
  rt::fs::writeFileSync(fixture.root + "/unconfined", std::string_view("unchanged"));
  const auto bytes = rt::fs::readFileSync(fixture.root + "/unconfined");
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "unchanged");
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport(fixture.artifact);
  if (!support.landlockThreadSync || !support.seccomp) {
    EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}}, fixture.artifact); }),
              "Linux confinement requires Landlock ABI8+ and seccomp thread synchronization");
    return;
  }
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{"relative/path"}, {fixture.root}}, fixture.artifact); }), "Linux confinement paths must be absolute");
  rt::fs::mkdirSync(fixture.root + "/immutable");
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}, std::vector<std::string>{fixture.root + "/immutable"}}, fixture.artifact); }),
            "Immutable path below writable parent requires a read-only mount");
  // Still unconfined: a file outside every grant opens.
  const int fd = ::open("/etc/hostname", O_RDONLY);
  EXPECT(fd >= 0);
  if (fd >= 0) ::close(fd);
#endif
}

TEST("a confined child reads and writes only its grants and cannot change modes") {
  const Fixture fixture;
#if defined(__APPLE__)
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    const auto refusal = messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}}, fixture.artifact); });
    ::_exit(refusal == "Linux confinement requires Linux ARM64" ? 0 : 1);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  EXPECT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport(fixture.artifact);
  if (!support.landlockThreadSync || !support.seccomp) return;
  const std::string readable = fixture.root + "/readable";
  const std::string writable = fixture.root + "/writable";
  const std::string outside = fixture.root + "/outside";
  rt::fs::mkdirSync(readable);
  rt::fs::mkdirSync(writable);
  rt::fs::mkdirSync(outside);
  rt::fs::writeFileSync(readable + "/granted.txt", std::string_view("granted"));
  rt::fs::writeFileSync(outside + "/secret.txt", std::string_view("secret"));
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    int code = 0;
    try {
      const LinuxPermissionsReceipt receipt = applyLinuxPermissions(LinuxPermissions{{readable, fixture.artifactPath}, {writable}}, fixture.artifact);
      if (!receipt.filesystemThreadSync || !receipt.processThreadSync || !receipt.immutableMounts.empty() || receipt.landlockAbi != support.landlockAbi) code |= 1;
      int fd = ::open((outside + "/secret.txt").c_str(), O_RDONLY);
      if (fd >= 0 || errno != EACCES) code |= 2;
      fd = ::open((readable + "/granted.txt").c_str(), O_RDONLY);
      if (fd < 0) code |= 4;
      fd = ::open((readable + "/new.txt").c_str(), O_CREAT | O_WRONLY, 0600);
      if (fd >= 0 || errno != EACCES) code |= 8;
      fd = ::open((writable + "/new.txt").c_str(), O_CREAT | O_WRONLY, 0600);
      if (fd < 0) code |= 16;
      if (::fchmodat(AT_FDCWD, (writable + "/new.txt").c_str(), 0644, 0) == 0 || errno != EPERM) code |= 32;
    } catch (...) {
      code |= 64;
    }
    ::_exit(code);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  EXPECT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
  // The parent stays unconfined.
  const int fd = ::open((outside + "/secret.txt").c_str(), O_RDONLY);
  EXPECT(fd >= 0);
  if (fd >= 0) ::close(fd);
#endif
}

TEST("explicit executable grants cannot be broadened through the dynamic loader") {
  const Fixture fixture;
#if defined(__APPLE__)
  return;
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport(fixture.artifact);
  if (!support.landlockThreadSync || !support.seccomp) return;
  const std::string writable = fixture.root + "/writable";
  rt::fs::mkdirSync(writable);
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    int code = 0;
    try {
      LinuxPermissions permissions;
      permissions.read = {"/bin/true", "/usr/lib/libc.so.6", "/etc/ld.so.cache", "/lib/ld-linux-aarch64.so.1"};
      permissions.write = {writable};
      permissions.execute = {"/bin/true", "/lib/ld-linux-aarch64.so.1"};
      permissions.allowChildProcess = true;
      const auto receipt = applyLinuxPermissions(permissions, fixture.artifact);
      if (!receipt.processThreadSync || !receipt.filesystemThreadSync) code |= 1;
      pid_t allowed = -1;
      char* const allowedArgv[] = {const_cast<char*>("/bin/true"), nullptr};
      const int allowedStatus = ::posix_spawn(&allowed, "/bin/true", nullptr, nullptr, allowedArgv, environ);
      if (allowedStatus != 0 || allowed <= 0) code |= 2;
      else {
        int waited = 0;
        if (::waitpid(allowed, &waited, 0) != allowed || !WIFEXITED(waited) || WEXITSTATUS(waited) != 0) code |= 4;
      }
      pid_t denied = -1;
      char* const deniedArgv[] = {const_cast<char*>("/bin/false"), nullptr};
      const int deniedStatus = ::posix_spawn(&denied, "/bin/false", nullptr, nullptr, deniedArgv, environ);
      if (deniedStatus != EACCES) code |= 8;
      pid_t loaderAllowed = -1;
      char* const loaderAllowedArgv[] = {const_cast<char*>("/lib/ld-linux-aarch64.so.1"), const_cast<char*>("/bin/true"), nullptr};
      const int loaderAllowedStatus = ::posix_spawn(&loaderAllowed, "/lib/ld-linux-aarch64.so.1", nullptr, nullptr, loaderAllowedArgv, environ);
      if (loaderAllowedStatus != 0 || loaderAllowed <= 0) code |= 32;
      else {
        int waited = 0;
        if (::waitpid(loaderAllowed, &waited, 0) != loaderAllowed || !WIFEXITED(waited) || WEXITSTATUS(waited) != 0) code |= 64;
      }
      pid_t loaderDenied = -1;
      char* const loaderDeniedArgv[] = {const_cast<char*>("/lib/ld-linux-aarch64.so.1"), const_cast<char*>("/bin/false"), nullptr};
      const int loaderDeniedStatus = ::posix_spawn(&loaderDenied, "/lib/ld-linux-aarch64.so.1", nullptr, nullptr, loaderDeniedArgv, environ);
      if (loaderDeniedStatus == 0 && loaderDenied > 0) {
        int waited = 0;
        if (::waitpid(loaderDenied, &waited, 0) != loaderDenied || !WIFEXITED(waited) || WEXITSTATUS(waited) == 0) code |= 128;
      } else if (loaderDeniedStatus != EACCES) {
        code |= 128;
      }
    } catch (...) {
      code |= 16;
    }
    ::_exit(code);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  EXPECT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}

TEST("executable grants require an explicit child-process opt-in") {
#if defined(__APPLE__)
  return;
#else
  const Fixture fixture;
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    int code = 1;
    try {
      LinuxPermissions permissions;
      permissions.read = {"/bin/true", "/usr/lib/libc.so.6", "/lib/ld-linux-aarch64.so.1"};
      permissions.execute = {"/bin/true"};
      static_cast<void>(applyLinuxPermissions(permissions, fixture.artifact));
    } catch (const rt::TypeError&) {
      code = 0;
    } catch (...) {
      code = 2;
    }
    ::_exit(code);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  EXPECT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}
