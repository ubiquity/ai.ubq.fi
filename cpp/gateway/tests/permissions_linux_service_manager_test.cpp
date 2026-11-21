// Links: src/runtime/permissions_linux.cpp
#include "uos/runtime/permissions_linux.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test.hpp"
#include "helpers/force_landlock_probe.hpp"

#if defined(__linux__) && defined(__aarch64__)
#include <cerrno>
#include <cstdio>
#include <latch>
#include <string>
#include <thread>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

namespace rt = uos::rt;
using namespace uos::runtime::permissions_linux;

namespace {
  template <class Body> void inChild(const Body& body) {
    const int before = rt::test::failures();
    std::fflush(nullptr);
    const pid_t child = ::fork();
    ASSERT(child >= 0);
    if (child == 0) {
      try {
        body();
        ::_exit(rt::test::failures() == before ? 0 : 1);
      } catch (const std::exception& error) {
        std::fprintf(stderr, "Confinement test child: %s\n", error.what());
      } catch (...) {}
      ::_exit(1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

  constexpr auto reason = "Service-manager confinement check failed: ";
  const LinuxPermissionsSupport absent{"arm64", "", 0, false, true};
  const detail::ServiceManagerChecks confined{true, true, true};
}

TEST("Landlock absence still refuses without the opt-in") {
  for (const int error : {ENOSYS, EOPNOTSUPP}) {
    inChild([&] {
      ASSERT(uos::tests::forceLandlockProbeError(error));
      const auto expected = "Landlock ABI query failed: errno " + std::to_string(error);
      EXPECT_THROWS_MESSAGE(applyLinuxPermissions({}), rt::JsError, expected);
      LinuxPermissions reduced;
      reduced.allowReducedConfinement = LinuxPermissionsTcpPorts{0};
      EXPECT_THROWS_MESSAGE(applyLinuxPermissions(reduced), rt::JsError, expected);
    });
  }
}

TEST("an unsupported probe reaches service-manager checks while other errors still refuse") {
  LinuxPermissions optIn;
  optIn.allowNoLandlock = ServiceManagerPaths{"/unused-package", "/unused-state"};
  for (const int error : {ENOSYS, EOPNOTSUPP, EPERM, EINVAL}) {
    inChild([&] {
      ASSERT(uos::tests::forceLandlockProbeError(error));
      try {
        applyLinuxPermissions(optIn);
        EXPECT(false);
      } catch (const rt::JsError& refusal) {
        if (error == ENOSYS || error == EOPNOTSUPP) {
          EXPECT(refusal.message.starts_with(reason));
        } else {
          EXPECT_EQ(refusal.message, "Landlock ABI query failed: errno " + std::to_string(error));
        }
      }
    });
  }
}

TEST("the no-Landlock opt-in is ignored on a kernel with Landlock") {
  const auto root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-permissions-service-");
  const auto remove = rt::scopeExit([&] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  LinuxPermissions permissions{{root}, {root}};
  permissions.allowNoLandlock = ServiceManagerPaths{"/unused-package", "/unused-state"};
  inChild([&] {
    const auto receipt = applyLinuxPermissions(permissions);
    EXPECT_EQ(receipt.mode, "landlock");
    EXPECT(receipt.filesystemThreadSync && receipt.processThreadSync);
    const int descriptor = ::open("/etc/hostname", O_RDONLY | O_CLOEXEC);
    const int result = errno;
    if (descriptor >= 0) ::close(descriptor);
    EXPECT_EQ(descriptor, -1);
    EXPECT_EQ(result, EACCES);
  });
  inChild([&] {
    EXPECT_THROWS_MESSAGE(detail::applyLinuxPermissionsWithAbiCap(permissions, 3), rt::JsError,
                          "Linux confinement requires Landlock ABI8+ and seccomp thread synchronization");
    permissions.allowReducedConfinement = LinuxPermissionsTcpPorts{0};
    const auto receipt = detail::applyLinuxPermissionsWithAbiCap(permissions, 3);
    EXPECT_EQ(receipt.mode, "landlock-reduced");
    EXPECT(!receipt.filesystemThreadSync && receipt.processThreadSync);
  });
}

TEST("service-manager mode identifies each failed confinement check") {
  const auto rejects = [](const detail::ServiceManagerChecks& checks, std::string_view failure) {
    EXPECT_THROWS_MESSAGE(detail::applyServiceManagerPermissions(absent, false, checks), rt::JsError, reason + std::string(failure));
  };
  rejects({false, true, true}, "the gateway must run as a non-root user");
  rejects({true, false, true}, "the capability sets, including the bounding set, must be empty");
  rejects({true, true, false}, "NoNewPrivileges is required");
  rejects({true, true, true, "/etc"}, "read-only system mount: /etc");
  rejects({true, true, true, std::nullopt, "/home"}, "masked home directories: /home");
  rejects({true, true, true, std::nullopt, std::nullopt, "/some-home/.codex"}, "unreadable home secrets: /some-home/.codex");
  auto noSeccomp = absent;
  noSeccomp.seccomp = false;
  EXPECT_THROWS_MESSAGE(detail::applyServiceManagerPermissions(noSeccomp, false, confined), rt::JsError,
                        "Service-manager confinement check failed: seccomp thread synchronization is required");
  EXPECT_THROWS_MESSAGE(detail::applyServiceManagerPermissions(absent, true, confined), rt::JsError,
                        "Service-manager confinement check failed: the Antigravity child-process profile requires Landlock");
}

TEST("verified service-manager mode installs the default seccomp filter on every thread") {
  inChild([] {
    ASSERT(uos::tests::forceLandlockProbeError(ENOSYS));
    ASSERT_EQ(::prctl(PR_GET_NO_NEW_PRIVS, 0, 0, 0, 0), 1);
    const auto execError = [] {
      char* const argv[] = {nullptr};
      return ::execve("/does-not-exist", argv, argv) == -1 ? errno : 0;
    };
    EXPECT_EQ(execError(), ENOENT);
    std::latch apply(1);
    int workerExecError = 0;
    std::thread worker([&] {
      apply.wait();
      workerExecError = execError();
    });
    const auto receipt = detail::applyServiceManagerPermissions(absent, false, confined);
    apply.count_down();
    worker.join();
    EXPECT_EQ(receipt.mode, "service-manager");
    EXPECT_EQ(receipt.landlockAbi, 0);
    EXPECT(!receipt.filesystemThreadSync && receipt.processThreadSync);
    EXPECT_EQ(execError(), EPERM);
    EXPECT_EQ(workerExecError, EPERM);
    EXPECT_EQ(::fchmodat(AT_FDCWD, "/does-not-exist", 0600, 0), -1);
    EXPECT_EQ(errno, EPERM);
  });
}
#else
TEST("service-manager confinement still requires Linux ARM64") {
  using namespace uos::runtime::permissions_linux;
  LinuxPermissions permissions;
  permissions.allowNoLandlock = ServiceManagerPaths{"/unused-package", "/unused-state"};
  EXPECT_THROWS_MESSAGE(applyLinuxPermissions(permissions), uos::rt::JsError, "Linux confinement requires Linux ARM64");
}
#endif
