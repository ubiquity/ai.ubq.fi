// Query the kernel and exercise fail-closed confinement in a forked child.
#include "uos/runtime/permissions_linux.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>

namespace rt = uos::rt;
using namespace uos::runtime::permissions_linux;

namespace {
  struct Fixture {
    std::string root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-permissions-");
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

TEST("the support query reports the kernel's Landlock ABI and seccomp availability") {
#if defined(__APPLE__)
  EXPECT_EQ(messageOf([&] { getLinuxPermissionsSupport(); }), "Linux confinement requires Linux ARM64");
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport();
  EXPECT_EQ(support.architecture, "arm64");
  EXPECT_EQ(support.kernel, rt::os::release());
  EXPECT(support.landlockAbi >= 1);
  EXPECT_EQ(support.landlockThreadSync, support.landlockAbi >= 8);
  EXPECT(support.seccomp);
  std::printf("     host: landlockAbi %g, seccomp %s\n", support.landlockAbi, support.seccomp ? "true" : "false");
  const LinuxPermissionsSupport again = getLinuxPermissionsSupport();
  EXPECT_EQ(again.landlockAbi, support.landlockAbi);
#endif
}

TEST("refusals before the irreversible step leave the process unconfined") {
  const Fixture fixture;
#if defined(__APPLE__)
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{"relative/path"}, {fixture.root}}); }), "Linux confinement requires Linux ARM64");
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}, std::vector<std::string>{fixture.root}}); }),
            "Linux confinement requires Linux ARM64");
  rt::fs::writeFileSync(fixture.root + "/unconfined", std::string_view("unchanged"));
  const auto bytes = rt::fs::readFileSync(fixture.root + "/unconfined");
  EXPECT_EQ(std::string(bytes.begin(), bytes.end()), "unchanged");
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport();
  if (!support.landlockThreadSync || !support.seccomp) {
    EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}}); }),
              "Linux confinement requires Landlock ABI8+ and seccomp thread synchronization");
    return;
  }
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{"relative/path"}, {fixture.root}}); }), "Linux confinement paths must be absolute");
  rt::fs::mkdirSync(fixture.root + "/immutable");
  EXPECT_EQ(messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}, std::vector<std::string>{fixture.root + "/immutable"}}); }),
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
    const auto refusal = messageOf([&] { applyLinuxPermissions(LinuxPermissions{{fixture.root}, {fixture.root}}); });
    ::_exit(refusal == "Linux confinement requires Linux ARM64" ? 0 : 1);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  EXPECT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#else
  const LinuxPermissionsSupport support = getLinuxPermissionsSupport();
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
      const LinuxPermissionsReceipt receipt = applyLinuxPermissions(LinuxPermissions{{readable}, {writable}});
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
