// Links: src/runtime/permissions_linux.cpp
#include "uos/runtime/permissions_linux.hpp"

#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <thread>

#if defined(__linux__) && defined(__aarch64__)
#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace rt = uos::rt;
using namespace uos::runtime::permissions_linux;

namespace {
  constexpr const char* strictRefusal = "Linux confinement requires Landlock ABI8+ and seccomp thread synchronization";
  constexpr const char* reducedRefusal = "Reduced Linux confinement requires Landlock ABI3+ and seccomp thread synchronization";
}

TEST("the opt-in selects every available Landlock right and the correct kernel structure size") {
  struct Expected {
    int abi;
    std::uint64_t filesystem;
    std::uint64_t network;
    std::uint64_t scope;
    std::size_t size;
    std::uint32_t flags;
  };
  constexpr std::array cases{
      Expected{3, 0x7fff, 0, 0, 8, 0},
      Expected{4, 0x7fff, 3, 0, 16, 0},
      Expected{5, 0xffff, 3, 0, 16, 0},
      Expected{6, 0xffff, 3, 3, 24, 0},
      Expected{7, 0xffff, 3, 3, 24, 0},
      Expected{8, 0xffff, 3, 3, 24, 8},
      Expected{9, 0x1ffff, 3, 3, 24, 8},
  };
  for (const auto& expected : cases) {
    const auto policy = detail::landlockPolicy(expected.abi, true, true);
    EXPECT_EQ(policy.ruleset.handledAccessFs, expected.filesystem);
    EXPECT_EQ(policy.ruleset.handledAccessNet, expected.network);
    EXPECT_EQ(policy.ruleset.scoped, expected.scope);
    EXPECT_EQ(policy.rulesetSize, expected.size);
    EXPECT_EQ(policy.restrictFlags, expected.flags);
  }
  const auto full = detail::landlockPolicy(8, true, false);
  EXPECT_EQ(full.ruleset.handledAccessFs, 0x7fffU);
  EXPECT_EQ(full.ruleset.handledAccessNet, 0U);
  EXPECT_EQ(full.ruleset.scoped, 0U);
  EXPECT_EQ(full.rulesetSize, 8U);
  EXPECT_EQ(full.restrictFlags, 8U);
  EXPECT_EQ(detail::landlockPolicy(9, true, false).ruleset.handledAccessFs, 0x17fffU);
}

TEST("the ABI floor and seccomp requirement fail closed") {
  for (int abi = 2; abi < 8; ++abi)
    EXPECT_THROWS_MESSAGE(detail::landlockPolicy(abi, true, false), rt::JsError, strictRefusal);
  EXPECT_THROWS_MESSAGE(detail::landlockPolicy(2, true, true), rt::JsError, reducedRefusal);
  EXPECT_THROWS_MESSAGE(detail::landlockPolicy(3, false, true), rt::JsError, reducedRefusal);
  EXPECT_THROWS_MESSAGE(detail::landlockPolicy(8, false, false), rt::JsError, strictRefusal);
}

#if defined(__linux__) && defined(__aarch64__)
namespace {
  struct Fixture {
    std::string root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-permissions-reduced-");
    std::string writable = root + "/writable";
    std::string outside = root + "/outside";

    Fixture() {
      rt::fs::mkdirSync(writable);
      rt::fs::mkdirSync(outside);
    }
    ~Fixture() { rt::fs::rmSync(root, {.recursive = true, .force = true}); }

    LinuxPermissions permissions() const { return {{writable}, {writable}}; }
  };

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
        std::fprintf(stderr, "Confined child: %s\n", error.what());
      } catch (...) {}
      ::_exit(1);
    }
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }

  int tryWrite(const std::string& file) {
    const int fd = ::open(file.c_str(), O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return errno;
    ::close(fd);
    return 0;
  }

  struct LoopbackListener {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    ~LoopbackListener() { if (fd >= 0) ::close(fd); }
    void listen() {
      ASSERT(fd >= 0);
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      ASSERT_EQ(::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)), 0);
      ASSERT_EQ(::listen(fd, 1), 0);
      socklen_t size = sizeof(address);
      ASSERT_EQ(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size), 0);
    }
    int tryConnect() const {
      const int client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
      ASSERT(client >= 0);
      const int result = ::connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0 ? 0 : errno;
      ::close(client);
      return result;
    }
  };
}

TEST("a capped kernel refuses the default policy and an opt-in below the floor before enforcement") {
  const Fixture fixture;
  inChild([&] {
    auto permissions = fixture.permissions();
    EXPECT_THROWS_MESSAGE(detail::applyLinuxPermissionsWithAbiCap(permissions, 3), rt::JsError, strictRefusal);
    permissions.allowReducedConfinement = LinuxPermissionsTcpPorts{0};
    EXPECT_THROWS_MESSAGE(detail::applyLinuxPermissionsWithAbiCap(permissions, 2), rt::JsError, reducedRefusal);
    EXPECT_EQ(tryWrite(fixture.outside + "/unconfined"), 0);
  });
}

TEST("reduced enforcement refuses to leave an existing thread outside Landlock") {
  const Fixture fixture;
  inChild([&] {
    const std::jthread sibling([](std::stop_token stop) {
      while (!stop.stop_requested()) std::this_thread::yield();
    });
    auto permissions = fixture.permissions();
    permissions.allowReducedConfinement = LinuxPermissionsTcpPorts{0};
    EXPECT_THROWS_MESSAGE(detail::applyLinuxPermissionsWithAbiCap(permissions, 3), rt::JsError,
                          "Reduced Landlock confinement requires exactly one thread during bootstrap");
    EXPECT_EQ(tryWrite(fixture.outside + "/unconfined"), 0);
  });
}

TEST("capped children enforce filesystem rights, seccomp and available TCP controls") {
  const Fixture fixture;
  const auto support = getLinuxPermissionsSupport();
  ASSERT(support.seccomp && support.landlockAbi >= 3);
  for (int abi = 3; abi <= std::min(8, static_cast<int>(support.landlockAbi)); ++abi) {
    LoopbackListener allowed, denied;
    allowed.listen();
    denied.listen();
    inChild([&] {
      auto permissions = fixture.permissions();
      permissions.allowReducedConfinement = LinuxPermissionsTcpPorts{0, {ntohs(allowed.address.sin_port)}};
      const auto receipt = detail::applyLinuxPermissionsWithAbiCap(permissions, abi);
      EXPECT_EQ(receipt.landlockAbi, support.landlockAbi);
      EXPECT(receipt.processThreadSync);
      EXPECT_EQ(receipt.filesystemThreadSync, abi >= 8);
      const auto active = [&](std::string_view control) { return std::ranges::find(receipt.activeProtections, control) != receipt.activeProtections.end(); };
      EXPECT(active("landlock-filesystem"));
      EXPECT_EQ(active("landlock-thread-inheritance"), abi < 8);
      EXPECT_EQ(active("landlock-tsync"), abi >= 8);
      EXPECT_EQ(active("tcp-ports"), abi >= 4);
      EXPECT_EQ(active("device-ioctl"), abi >= 5);
      EXPECT_EQ(active("abstract-unix-sockets"), abi >= 6);
      EXPECT_EQ(active("signal-scope"), abi >= 6);
      EXPECT_EQ(std::ranges::find(receipt.missingProtections, "tcp-ports") != receipt.missingProtections.end(), abi < 4);
      const std::string granted = fixture.writable + "/granted";
      EXPECT_EQ(tryWrite(granted), 0);
      EXPECT_EQ(tryWrite(fixture.outside + "/denied"), EACCES);
      EXPECT_EQ(::fchmodat(AT_FDCWD, granted.c_str(), 0644, 0), -1);
      EXPECT_EQ(errno, EPERM);
      if (abi == 3) {
        int workerWrite = 0;
        std::thread worker([&] { workerWrite = tryWrite(fixture.outside + "/worker-denied"); });
        worker.join();
        EXPECT_EQ(workerWrite, EACCES);
      }
      LoopbackListener confined;
      confined.listen(); // The listener's permitted port 0 still allocates an ephemeral port.
      EXPECT_EQ(allowed.tryConnect(), 0);
      EXPECT_EQ(denied.tryConnect(), abi >= 4 ? EACCES : 0);
    });
  }
}
#endif
