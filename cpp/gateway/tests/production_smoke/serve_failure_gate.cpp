// Native lifecycle gate using a private SQLite store inside the fixture namespace.
#include "uos/kv.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/serve.hpp"
#include "uos/storage/kv.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
  void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
  }

  class CountedStore final : public uos::rt::ForwardingKv {
  public:
    CountedStore(std::shared_ptr<uos::rt::Kv> store, std::shared_ptr<int> count)
        : ForwardingKv(*store), owner(std::move(store)), closes(std::move(count)) {}

    void close() override {
      ++*closes;
      owner->close();
    }

  private:
    std::shared_ptr<uos::rt::Kv> owner;
    std::shared_ptr<int> closes;
  };

  class OccupiedPort {
  public:
    OccupiedPort() {
#if defined(__APPLE__)
      descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
#else
      descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#endif
      require(descriptor >= 0, "Could not create the loopback bind-collision fixture");
#if defined(__APPLE__)
      if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        ::close(descriptor);
        descriptor = -1;
        throw std::runtime_error("Could not mark the collision socket close-on-exec");
      }
#endif
      sockaddr_in address{};
      address.sin_family = AF_INET;
      address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
      require(::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "Could not bind the collision fixture");
      require(::listen(descriptor, 1) == 0, "Could not listen on the collision fixture");
      socklen_t size = sizeof(address);
      require(::getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) == 0, "Could not observe the assigned collision port");
      port = static_cast<int>(ntohs(address.sin_port));
      require(port != 7999 && port != 8000, "OS assigned a prohibited fixture port");
    }
    OccupiedPort(const OccupiedPort&) = delete;
    OccupiedPort& operator=(const OccupiedPort&) = delete;
    ~OccupiedPort() { if (descriptor >= 0) ::close(descriptor); }
    int port = 0;

  private:
    int descriptor = -1;
  };

  uos::rt::Task<void> gate() {
    using namespace uos;
    const serve::ServeStartupOptions unprepared{"127.0.0.1", 0};
    bool refused = false;
    try {
      auto serving = serve::importMetaMain(unprepared);
      co_await std::move(serving);
    } catch (const rt::JsError& error) {
      refused = rt::includes(error.message, "must be initialized");
    }
    require(refused, "Uninitialized typed entry did not fail before storage/listener effects");

    require(!rt::fs::existsSync(".data"), "Uninitialized startup accessed the default database");

    const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-serve-failure-"));
    const auto cleanup = rt::scopeExit([directory] { rt::fs::rmSync(directory, {.recursive = true, .force = true}); });
    const std::string databasePath = directory + "/synthetic-state.sqlite3";
    auto opening = storage::kv::openKv(databasePath);
    auto store = co_await std::move(opening);
    const auto closes = std::make_shared<int>(0);
    const auto counted = std::make_shared<CountedStore>(store, closes);
    kv::initializeKv(counted);
    const OccupiedPort collision;
    const serve::ServeStartupOptions options{"127.0.0.1", collision.port};
    std::string failure;
    try {
      auto serving = serve::importMetaMain(options);
      co_await std::move(serving);
    } catch (const rt::JsError& error) {
      failure = error.message;
    } catch (const std::exception& error) {
      failure = error.what();
    }
    const std::string expectedFailure = "Failed to start server. Is port " + std::to_string(collision.port) + " in use?";
    if (failure != expectedFailure)
      std::fprintf(stderr, "observed_entry_failure=%s\n", failure.c_str());
    require(failure == expectedFailure, "Typed entry did not propagate the exact source-compatible bind collision");
    if (*closes != 1) {
      std::fprintf(stderr, "observed_kv_close_count=%d\n", *closes);
      throw std::runtime_error("Typed entry must close the real durable KV exactly once before propagating listen failure");
    }
    bool closed = false;
    const rt::KvKey probeKey{"synthetic", "closed-store-probe"};
    try {
      auto probing = store->get(probeKey);
      const auto ignored = co_await std::move(probing);
      (void)ignored;
    } catch (const rt::JsError& error) {
      closed = error.message == "Cannot call '.get' after '.close' is called";
    }
    require(closed, "The underlying real SQLite store remained open after listener failure");
    kv::resetKvForTest();
    std::printf("{\"cases\":3,\"passed\":3,\"failed\":0,\"kv_close_count\":%d,\"real_bind_error\":true}\n", *closes);
  }
}

int main(int argc, char** argv) {
  if (argc != 1) {
    std::fprintf(stderr, "usage: uos-serve-failure-gate\n");
    return 2;
  }
  uos::rt::process::setArgv(argc, argv);
  uos::rt::loop().setUncaughtPolicy(uos::rt::UncaughtPolicy::report);
  try {
    auto running = gate();
    uos::rt::blockOn(std::move(running));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
