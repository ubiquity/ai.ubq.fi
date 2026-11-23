// Links: src/runtime/permissions_linux.cpp
#include "uos/runtime/permissions_linux.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"

#include <cerrno>
#include <filesystem>
#include <stdexcept>

#if defined(__linux__) && defined(__aarch64__)
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;
namespace rt = uos::rt;
using namespace uos::runtime::permissions_linux;

namespace {
  struct Fixture {
    std::string root = rt::fs::mkdtempSync(rt::os::tmpdir() + "/uos-gemini-permission-");
    std::string package = root + "/package";
    std::string state = root + "/state";
    std::string data = state + "/.data";
    std::string workspace = data + "/turn";
    std::string other = data + "/fixture-elf";
    std::string home = root + "/home";
    std::string agy = home + "/.local/bin/agy";
    std::string cliState = home + "/.gemini/antigravity-cli";

    Fixture() {
      for (const auto& dir : {package, workspace, home + "/.local/bin", cliState}) rt::fs::mkdirSync(dir, {.recursive = true, .mode = 0700});
      for (const auto& executable : {agy, other}) {
        fs::copy_file("/usr/bin/true", executable);
        rt::fs::chmodSync(executable, 0700);
      }
      // The integration checks only the metadata of the CLI-owned file.
      rt::fs::writeFileSync(cliState + "/antigravity-oauth-token", "", {.flag = "wx", .mode = 0600});
    }
    ~Fixture() { rt::fs::rmSync(root, {.recursive = true, .force = true}); }
  };

  void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }

  rt::Task<int> spawnAndWait(const std::string& file, const std::optional<std::string>& verified,
                            std::function<void()> confine = {}) {
    std::vector<std::string> args{file};
    if (verified) args.insert(args.end(), {"--verify", *verified});
    rt::process::SpawnOptions options{.env = std::vector<std::pair<std::string, std::string>>{}, .confine = std::move(confine)};
    const auto child = rt::process::spawn(std::move(args), std::move(options));
    const auto done = child.exited();
    co_return co_await done.wait();
  }
}
#endif

TEST("shared auth follower mode overrides the Gemini opt-in before granting CLI access") {
#if defined(__linux__) && defined(__aarch64__)
  const Fixture fixture;
  const rt::env::ScopedEnvironment environment{{"HOME", fixture.home}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  ASSERT(uos::provider::gemini::geminiCliConfigured());
  const rt::env::ScopedOverride follower{{"UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER", "1"}};
  EXPECT(!uos::provider::gemini::geminiOperatorOptIn());
  EXPECT(!uos::provider::gemini::geminiCliConfigured());
  const auto profiles = gatewayLinuxPermissions(fixture.package, fixture.state);
  EXPECT(!profiles.antigravity);
  EXPECT(!profiles.gateway.allowChildProcess);
  EXPECT(profiles.gateway.execute.empty());
  EXPECT_EQ(profiles.gateway.write, std::vector<std::string>{fixture.data});
#endif
}

TEST("Gemini leaves the disabled gateway grants and ARM64 filter byte identical") {
#if defined(__linux__) && defined(__aarch64__)
  const Fixture fixture;
  rt::env::ScopedEnvironment environment{{"HOME", fixture.home}};
  // Starting revision 74062589, with canonical OS paths evaluated on this host.
  std::vector<std::string> read{fixture.package, fixture.state};
  for (const auto* path : {"/usr/lib", "/etc/ssl", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/localtime", "/dev/null",
                           "/dev/urandom", "/dev/random", "/proc/self"}) {
    if (fs::exists(path)) read.push_back(fs::canonical(path).string());
  }
  const std::vector<std::string> write{fixture.data};
  const std::optional<std::vector<std::string>> immutable{std::vector<std::string>{fixture.package}};
  const std::optional<std::string> settings[] = {std::nullopt, "0", "true", "01", "1 "};
  for (const auto& setting : settings) {
    rt::env::ScopedOverride flag{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", setting}};
    const auto profiles = gatewayLinuxPermissions(fixture.package, fixture.state);
    EXPECT(!profiles.antigravity);
    EXPECT_EQ(profiles.gateway.read, read);
    EXPECT_EQ(profiles.gateway.write, write);
    EXPECT_EQ(profiles.gateway.immutable, immutable);
    EXPECT(!profiles.gateway.allowReducedConfinement);
    EXPECT(profiles.gateway.execute.empty());
    EXPECT(!profiles.gateway.allowChildProcess);
    const auto filter = detail::seccompProcessFilter(profiles.gateway.allowChildProcess);
    const auto bytes = std::span(reinterpret_cast<const std::uint8_t*>(filter.data()), filter.size() * sizeof(sock_filter));
    // The raw sock_fprog instructions emitted by the original installProcessFilter.
    EXPECT_EQ(bytes.size(), std::size_t(408));
    EXPECT_EQ(rt::crypto::createHash("sha256").update(bytes).digest("hex"),
      "6536d38a3e616f3fe229a8cef632c6cda5c15b3aff6caf75de2d7c527b1dd552");
  }
#endif
}

TEST("executable grants require an explicit child-process opt-in") {
#if defined(__linux__) && defined(__aarch64__)
  const Fixture fixture;
  LinuxPermissions permissions;
  permissions.execute = {fixture.agy};
  bool rejected = false;
  try {
    static_cast<void>(applyLinuxPermissions(permissions));
  } catch (const rt::TypeError&) {
    rejected = true;
  }
  EXPECT(rejected);
#endif
}

TEST("Antigravity denies direct execution of a different ELF and limits loader visibility in the child") {
#if defined(__linux__) && defined(__aarch64__)
  const Fixture fixture;
  rt::env::ScopedEnvironment environment{{"HOME", fixture.home}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  const auto profiles = gatewayLinuxPermissions(fixture.package, fixture.state);
  ASSERT(profiles.antigravity.has_value());
  const auto loader = fs::canonical("/lib/ld-linux-aarch64.so.1").string();
  EXPECT_EQ(profiles.gateway.execute, (std::vector<std::string>{fixture.agy, loader}));
  EXPECT_EQ(profiles.antigravity->execute, profiles.gateway.execute);
  EXPECT_EQ(profiles.antigravity->write, std::vector<std::string>{fixture.cliState});
  std::fflush(nullptr);
  const pid_t child = ::fork();
  ASSERT(child >= 0);
  if (child == 0) {
    try {
      const auto receipt = applyLinuxPermissions(profiles.gateway);
      require(receipt.processThreadSync && receipt.filesystemThreadSync, "Missing opt-in confinement receipt");

      pid_t unexpected = -1;
      char* args[] = {const_cast<char*>(fixture.other.c_str()), nullptr};
      char* environmentList[] = {nullptr};
      const int status = ::posix_spawn(&unexpected, args[0], nullptr, nullptr, args, environmentList);
      if (status == 0) ::waitpid(unexpected, nullptr, 0);
      require(status == EACCES, "An ELF outside the execute grants was not denied");

      // A loader verification opens the target ELF without executing its code.
      // It sees gateway state from the gateway and loses that access in the child.
      rt::Loop loop;
      require(rt::blockOn(loop, spawnAndWait(loader, fixture.other)) == 0, "Loader did not see the readable fixture before child confinement");
      const auto confine = [&] {
        auto permissions = *profiles.antigravity;
        permissions.read.push_back(fixture.workspace);
        permissions.write.push_back(fixture.workspace);
        restrictLinuxChildThread(permissions);
        const int outside = ::open(fixture.other.c_str(), O_WRONLY | O_CLOEXEC);
        if (outside >= 0) ::close(outside);
        require(outside < 0 && errno == EACCES, "Child retained gateway state write access");
      };
      require(rt::blockOn(loop, spawnAndWait(fixture.agy, std::nullopt, confine)) == 0, "The allowed dynamic Antigravity stand-in did not run");
      require(rt::blockOn(loop, spawnAndWait(loader, fixture.agy, confine)) == 0, "Loader did not see the permitted Antigravity ELF");
      require(rt::blockOn(loop, spawnAndWait(loader, fixture.other, confine)) != 0, "The child loader retained gateway state read access");
      require(rt::blockOn(loop, spawnAndWait(loader, fixture.other)) == 0, "The request thread inherited the child's Landlock scope");
    } catch (const std::exception& error) {
      std::fprintf(stderr, "%s\n", error.what());
      ::_exit(1);
    }
    ::_exit(0);
  }
  int status = 0;
  ASSERT(::waitpid(child, &status, 0) == child);
  ASSERT(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
#endif
}
