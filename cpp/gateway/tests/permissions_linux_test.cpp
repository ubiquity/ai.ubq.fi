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

#if defined(__linux__) && defined(__aarch64__)
#include <sys/socket.h>
#include <sys/un.h>
#include <cstring>
#endif

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

#if defined(__linux__) && defined(__aarch64__)
namespace {
  struct UnixSocket {
    int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_un address{};

    explicit UnixSocket(const std::string& path) {
      if (path.size() >= sizeof(address.sun_path)) throw std::invalid_argument("Fixture Unix socket path is too long");
      address.sun_family = AF_UNIX;
      std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    }

    ~UnixSocket() { if (fd >= 0) ::close(fd); }

    int listen() const {
      if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) return errno;
      return ::listen(fd, 2) == 0 ? 0 : errno;
    }

    int connect() const {
      return ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 ? 0 : errno;
    }
  };

  int openError(const std::string& path, int flags) {
    const int fd = ::open(path.c_str(), flags | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    ::close(fd);
    return 0;
  }
}

TEST("owner file reads and socket resolution survive atomic replacement without granting writes") {
  if (getLinuxPermissionsSupport().landlockAbi < 9) return;
  const Fixture fixture;
  const std::string home = fixture.root + "/home";
  const std::string control = fixture.root + "/control";
  const std::string other = fixture.root + "/other";
  for (const auto& directory : {home, control, other}) rt::fs::mkdirSync(directory);
  const std::string auth = home + "/auth.json";
  const std::string replacement = home + "/next";
  const std::string socket = control + "/owner.sock";
  const std::string alias = home + "/owner.sock";
  rt::fs::writeFileSync(auth, std::string_view("first"));
  rt::fs::writeFileSync(replacement, std::string_view("second"));
  rt::fs::writeFileSync(control + "/private", std::string_view("fixture"));
  rt::fs::writeFileSync(other + "/auth.json", std::string_view("fixture"));
  ASSERT_EQ(::symlink(socket.c_str(), alias.c_str()), 0);
  const UnixSocket listener(socket);
  const UnixSocket otherListener(other + "/other.sock");
  ASSERT_EQ(listener.listen(), 0);
  ASSERT_EQ(otherListener.listen(), 0);

  const LinuxPermissions permissions{.readFilesBelow = {home}, .resolveUnixSocketsBelow = {control}};
  EXPECT_THROWS_MESSAGE(detail::applyLinuxPermissionsWithAbiCap(permissions, 8), rt::JsError,
                        "Unix socket resolution requires Landlock ABI9+");
  int sync[2];
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sync), 0);
  const int failures = rt::test::failures();
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    ::close(sync[0]);
    ::close(listener.fd);
    ::close(otherListener.fd);
    try {
      const auto receipt = applyLinuxPermissions(permissions);
      EXPECT(receipt.filesystemThreadSync && receipt.processThreadSync);
      const auto readAuth = [&] {
        const auto content = rt::fs::readFileSync(auth);
        return std::string(content.begin(), content.end());
      };
      EXPECT_EQ(readAuth(), "first");
      EXPECT_EQ(UnixSocket(alias).connect(), 0);
      EXPECT_EQ(openError(auth, O_WRONLY | O_TRUNC), EACCES);
      EXPECT_EQ(openError(home + "/new", O_WRONLY | O_CREAT), EACCES);
      EXPECT_EQ(openError(control + "/private", O_RDONLY), EACCES);
      EXPECT_EQ(openError(control + "/new", O_WRONLY | O_CREAT), EACCES);
      EXPECT_EQ(UnixSocket(control + "/new.sock").listen(), EACCES);
      EXPECT_EQ(openError(other + "/auth.json", O_RDONLY), EACCES);
      EXPECT_EQ(UnixSocket(other + "/other.sock").connect(), EACCES);
      char ready = 'r';
      ASSERT_EQ(::write(sync[1], &ready, 1), 1);
      ASSERT_EQ(::read(sync[1], &ready, 1), 1);
      EXPECT_EQ(ready, 'r');
      EXPECT_EQ(readAuth(), "second");
      EXPECT_EQ(UnixSocket(alias).connect(), 0);
      ::_exit(rt::test::failures() == failures ? 0 : 1);
    } catch (const std::exception& error) {
      std::fprintf(stderr, "Confined child: %s\n", error.what());
    } catch (...) {}
    ::_exit(1);
  }

  ::close(sync[1]);
  char ready = 0;
  const bool childReady = ::read(sync[0], &ready, 1) == 1 && ready == 'r';
  if (childReady) {
    EXPECT_EQ(::rename(replacement.c_str(), auth.c_str()), 0);
    EXPECT_EQ(::unlink(socket.c_str()), 0);
    const UnixSocket rebound(socket);
    EXPECT_EQ(rebound.listen(), 0);
    EXPECT_EQ(::send(sync[0], &ready, 1, MSG_NOSIGNAL), 1);
    // Keep the replacement listener alive until the child observes it.
    EXPECT_EQ(::read(sync[0], &ready, 1), 0);
  }
  ::close(sync[0]);
  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  EXPECT(childReady);
  ASSERT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}
#endif
