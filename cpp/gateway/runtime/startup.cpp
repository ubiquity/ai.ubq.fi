#include "runtime/startup.hpp"

#include "runtime/paths.hpp"
#include "src/rt/tls_internal.hpp"
#include "uos/codex/auth_import.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/health.hpp"
#include "uos/kv.hpp"
#include "uos/live/upstream.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/lithos_models.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/memory.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/runtime/permissions_linux.hpp"
#include "uos/serve.hpp"
#include "uos/storage/kv.hpp"
#include "uos/upstream_auth.hpp"

#include <openssl/evp.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::runtime::startup {
  namespace {
    namespace fs = std::filesystem;

    [[noreturn]] void refuse(const std::string& reason) { throw std::runtime_error("Native startup refused: " + reason); }

    bool hashText(std::string_view value) {
      return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }

    fs::path canonical(const std::string& value) {
      const fs::path path(value);
      if (!path.is_absolute() || path.lexically_normal() != path || fs::canonical(path) != path) refuse("path is not canonical: " + value);
      return path;
    }

    struct stat regular(const fs::path& path, bool privateFile = false) {
      struct stat info {};
      if (::lstat(path.c_str(), &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 || (info.st_mode & 0022) != 0)
        refuse("required regular file is missing or unsafe: " + path.string());
      if (privateFile && (info.st_uid != ::getuid() || (info.st_mode & 0077) != 0)) refuse("file is not owner-private: " + path.string());
      return info;
    }

    void privateDirectory(const fs::path& path) {
      struct stat info {};
      if (::lstat(path.c_str(), &info) != 0 || !S_ISDIR(info.st_mode) || info.st_uid != ::getuid() || (info.st_mode & 0077) != 0 || fs::canonical(path) != path)
        refuse("state directory is not a real owner-private directory: " + path.string());
    }

    bool disjoint(const fs::path& first, const fs::path& second) {
      const auto relative = first.lexically_relative(second);
      const auto reverse = second.lexically_relative(first);
      return !relative.empty() && *relative.begin() == ".." && !reverse.empty() && *reverse.begin() == "..";
    }

    void requireStoppedDatabase(const fs::path& database) {
      regular(database, true);
      for (const auto* suffix : {"-wal", "-shm", "-journal"}) {
        if (fs::exists(fs::symlink_status(database.string() + suffix)))
          refuse("Codex auth import requires stopped, checkpointed databases without SQLite sidecars");
      }
    }

    std::string read(const fs::path& path, std::size_t maximum = 1024 * 1024, bool privateFile = false) {
      const struct stat before = regular(path, privateFile);
      if (before.st_size < 0 || static_cast<std::uintmax_t>(before.st_size) > maximum) refuse("required manifest is oversized: " + path.string());
      const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
      if (descriptor < 0) refuse("required file cannot be opened: " + path.string());
      const auto close = rt::scopeExit([descriptor] { ::close(descriptor); });
      struct stat opened {};
      if (::fstat(descriptor, &opened) != 0 || opened.st_dev != before.st_dev || opened.st_ino != before.st_ino || opened.st_size != before.st_size)
        refuse("required file identity changed: " + path.string());
      std::string bytes(static_cast<std::size_t>(before.st_size), '\0');
      std::size_t offset = 0;
      while (offset < bytes.size()) {
        const ssize_t count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
        if (count <= 0) refuse("required file changed while reading: " + path.string());
        offset += static_cast<std::size_t>(count);
      }
      return bytes;
    }

    std::string sha256(const fs::path& path) {
      regular(path);
      std::ifstream input(path, std::ios::binary);
      if (!input) refuse("required file cannot be hashed: " + path.string());
      const auto context = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>(EVP_MD_CTX_new(), EVP_MD_CTX_free);
      if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1) refuse("SHA256 initialization failed");
      std::array<char, 65536> chunk{};
      while (input) {
        input.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const auto count = input.gcount();
        if (count > 0 && EVP_DigestUpdate(context.get(), chunk.data(), static_cast<std::size_t>(count)) != 1) refuse("SHA256 update failed");
      }
      if (!input.eof()) refuse("required file changed while hashing: " + path.string());
      std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
      unsigned length = 0;
      if (EVP_DigestFinal_ex(context.get(), bytes.data(), &length) != 1 || length != 32) refuse("SHA256 finalization failed");
      constexpr char hex[] = "0123456789abcdef";
      std::string result;
      result.reserve(64);
      for (unsigned index = 0; index < length; ++index) {
        result.push_back(hex[bytes[index] >> 4]);
        result.push_back(hex[bytes[index] & 15]);
      }
      return result;
    }

    json::object object(const fs::path& path, bool privateFile = false) {
      const json::value value = rt::jsonParse(read(path, 1024 * 1024, privateFile));
      if (!value.is_object()) refuse("required manifest is not an object: " + path.string());
      return value.as_object();
    }

    void keys(const json::object& value, const std::set<std::string_view>& allowed) {
      for (const auto& field : value) {
        if (!allowed.contains(std::string_view(field.key()))) refuse("unknown manifest field: " + std::string(field.key()));
      }
    }

    std::string string(const json::object& value, std::string_view key) {
      const auto selected = rt::getString(value, key);
      if (!selected || selected->empty()) refuse("missing string field: " + std::string(key));
      return std::string(*selected);
    }

    void requireHash(const json::object& value, std::string_view key, const fs::path& path) {
      const std::string expected = string(value, key);
      if (!hashText(expected) || sha256(path) != expected) refuse("integrity check failed: " + path.string());
    }

    void verifyPackage(const fs::path& root, const std::string& expected) {
      const fs::path manifest = root / "manifest.sha256";
      if (!hashText(expected) || sha256(manifest) != expected) refuse("package manifest integrity check failed");
      const std::string text = read(manifest);
      std::set<std::string> selected;
      std::size_t start = 0;
      std::string previous;
      while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        if (end == std::string::npos) refuse("package manifest requires a final newline");
        const std::string line = text.substr(start, end - start);
        start = end + 1;
        if (line.size() < 67 || !hashText(std::string_view(line).substr(0, 64)) || line.substr(64, 2) != "  ") refuse("invalid package manifest record");
        const std::string name = line.substr(66);
        const fs::path relative(name);
        if (relative.is_absolute() || relative.lexically_normal() != relative || name == "manifest.sha256" || name <= previous ||
            std::find(relative.begin(), relative.end(), "..") != relative.end() || !selected.insert(name).second)
          refuse("invalid or unsorted package path: " + name);
        previous = name;
        const fs::path path = canonical((root / relative).string());
        if (sha256(path) != line.substr(0, 64)) refuse("package file integrity check failed: " + name);
      }
      for (const auto& entry : fs::recursive_directory_iterator(root)) {
        if (entry.is_symlink()) refuse("package contains a symlink");
        if (entry.is_regular_file()) {
          const std::string name = entry.path().lexically_relative(root).generic_string();
          if (name != "manifest.sha256" && !selected.contains(name)) refuse("package contains an unbound file: " + name);
        } else if (!entry.is_directory()) {
          refuse("package contains a non-regular entry");
        }
      }
      for (const std::string_view required : {"bin/uos-gateway", "package-info.json", "build-profile.json", "runtime-closure.json", "operator-closure.json",
                                               "static/index.html", "static/openapi.json", "favicon.png", "favicon-32.png", "licenses/sqlite/LICENSE", "licenses/denokv/LICENSE",
                                               "runtime/launch-schema.json", "ops/launch-native.sh"}) {
        if (!selected.contains(std::string(required))) refuse("package lacks a required file: " + std::string(required));
      }
      if (canonical(rt::process::execPath()) != root / "bin/uos-gateway") refuse("executable is outside the selected package");
    }

    void verifyClosure(const fs::path& root) {
      const json::object closure = object(root / "runtime-closure.json");
      if (string(closure, "schema") != "uos-native-runtime-closure-v1" || string(closure, "platform") != "linux" || string(closure, "machine") != "AArch64")
        refuse("runtime closure platform is unsupported");
      const json::value* hostData = rt::get(closure, "hostData");
      if (!hostData || !hostData->is_array() || hostData->as_array().empty()) refuse("runtime host-data closure is missing");
      const json::value* libraries = rt::get(closure, "libraries");
      if (!libraries || !libraries->is_array() || libraries->as_array().empty()) refuse("runtime host-library closure is missing");
      for (const json::value& value : hostData->as_array()) {
        if (!value.is_string() || value.as_string().empty()) refuse("invalid runtime host-data closure record");
        const fs::path source(std::string(value.as_string()));
        if (!source.is_absolute() || source.lexically_normal() != source) refuse("runtime host data path is invalid");
        if (!source.string().starts_with("/etc/") && !source.string().starts_with("/dev/") && !source.string().starts_with("/proc/"))
          refuse("runtime host data is outside the qualified system directories");
        if (!fs::exists(source)) refuse("runtime host data is unavailable");
      }
      for (const json::value& value : libraries->as_array()) {
        if (!value.is_object()) refuse("invalid host-library closure record");
        const fs::path path = canonical(string(value.as_object(), "path"));
        if (!path.string().starts_with("/usr/lib/")) refuse("runtime library is outside the qualified host directory");
        requireHash(value.as_object(), "sha256", path);
      }
    }

    void routeUnavailable(int family, const void* address, std::size_t size) {
      const int descriptor = ::socket(family, SOCK_DGRAM, 0);
      if (descriptor < 0) {
        if (family == AF_INET6 && errno == EAFNOSUPPORT) return;
        refuse("fixture network probe cannot create a socket");
      }
      const auto close = rt::scopeExit([descriptor] { ::close(descriptor); });
      if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) != 0) refuse("fixture network probe cannot protect its descriptor");
      const int status = ::connect(descriptor, static_cast<const sockaddr*>(address), static_cast<socklen_t>(size));
      if (status == 0 || (errno != ENETUNREACH && errno != EHOSTUNREACH)) refuse("fixture requires an OS network namespace without outbound routes");
    }

    void requireFixtureNetwork() {
      sockaddr_in outside{};
      outside.sin_family = AF_INET;
      outside.sin_port = htons(443);
      if (::inet_pton(AF_INET, "192.0.2.1", &outside.sin_addr) != 1) refuse("fixture IPv4 probe is invalid");
      routeUnavailable(AF_INET, &outside, sizeof outside);
      sockaddr_in6 outside6{};
      outside6.sin6_family = AF_INET6;
      outside6.sin6_port = htons(443);
      if (::inet_pton(AF_INET6, "2001:db8::1", &outside6.sin6_addr) != 1) refuse("fixture IPv6 probe is invalid");
      routeUnavailable(AF_INET6, &outside6, sizeof outside6);
    }

    std::string fixtureOrigin(const json::object& fixture) {
      keys(fixture, {"syntheticOrigin"});
      const std::string origin = string(fixture, "syntheticOrigin");
      const rt::URL parsed(origin);
      if (parsed.protocol() != "http:" || parsed.hostname() != "127.0.0.1" || parsed.port().empty() || parsed.port() == "0" || parsed.port() == "7999" || parsed.port() == "8000" ||
          parsed.pathname() != "/" || !parsed.search().empty() || !parsed.hash().empty() || !parsed.username().empty() || !parsed.password().empty() || parsed.origin() != origin)
        refuse("fixture syntheticOrigin must be one assigned IPv4 loopback HTTP origin");
      return origin;
    }

    class FixtureTransport final : public rt::FetchTransport {
    public:
      explicit FixtureTransport(std::string origin, bool authImport) : origin_(std::move(origin)), original_(rt::fetchTransport()), authImport_(authImport) {
        if (!original_) refuse("real HTTP client transport is not installed");
      }
      rt::Task<rt::Response> send(rt::Request request) override {
        // The real client follows redirects internally. A fixture refuses them
        // before any second destination can bypass this outer guard.
        rt::RequestInit policy;
        policy.redirect = "error";
        rt::Request guarded(request, policy);
        const std::string destination = guarded.url();
        if (authImport_) {
          if (destination == origin_ + "/backend-api/wham/usage") return original_->send(std::move(guarded));
          refuse("fixture auth import allows only the Codex usage probe");
        }
        if (codex::auth::hasLeaderAuthReader() && destination == origin_ + "/backend-api/wham/usage")
          return original_->send(std::move(guarded));
        if (destination == deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL) {
          rt::Request mapped(origin_ + "/chat/completions", guarded);
          return original_->send(std::move(mapped));
        }
        if (destination == provider::lithos::LITHOS_CHAT_COMPLETIONS_URL) {
          return original_->send(rt::Request(origin_ + "/lithos/chat/completions", guarded));
        }
        if (destination == provider::lithos_models::LITHOS_MODELS_URL) {
          return original_->send(rt::Request(origin_ + "/lithos/models", guarded));
        }
        if (destination.starts_with(origin_ + "/codex/") || destination == origin_ + "/chat/completions") return original_->send(std::move(guarded));
        refuse("fixture refused an unmapped provider destination");
      }

    private:
      std::string origin_;
      std::shared_ptr<rt::FetchTransport> original_;
      bool authImport_;
    };

    struct FixtureRouting {
      std::shared_ptr<rt::FetchTransport> original;
      std::string originalCodexBase;
      explicit FixtureRouting(const std::string& origin, bool authImport = false) : original(rt::fetchTransport()), originalCodexBase(config::config.codexBaseUrl) {
        rt::setFetchTransport(std::make_shared<FixtureTransport>(origin, authImport));
        config::config.codexBaseUrl = origin + "/codex";
        live::upstream::LiveUpstreamBases bases;
        bases.callsBaseUrl = origin + "/codex";
        bases.sidebandBaseUrl = "ws" + origin.substr(4) + "/live";
        live::upstream::setLiveUpstreamBasesForTest(bases);
      }
      ~FixtureRouting() {
        rt::setFetchTransport(original);
        config::config.codexBaseUrl = originalCodexBase;
        live::upstream::setLiveUpstreamBasesForTest({});
      }
      FixtureRouting(const FixtureRouting&) = delete;
      FixtureRouting& operator=(const FixtureRouting&) = delete;
    };

    std::string fixtureBinding(const fs::path& state, const std::string& packageHash, const std::string& origin) {
      const json::object value{{"schema", "uos-native-fixture-state-v1"}, {"packageManifestSha256", packageHash}, {"stateRoot", state.string()}, {"syntheticOrigin", origin}};
      return rt::jsonStringify(value) + "\n";
    }

    bool verifyFixtureState(const fs::path& data, const std::string& binding) {
      const fs::path marker = data / "native-fixture-binding.json";
      if (fs::exists(marker) || fs::is_symlink(marker)) {
        if (read(marker, 16384, true) != binding) refuse("fixture state belongs to another launch binding");
        return false;
      }
      if (!fs::is_empty(data)) refuse("fixture requires fresh empty state or its exact private reopen binding");
      return true;
    }

    void createFixtureBinding(const fs::path& data, const std::string& binding) {
      const fs::path marker = data / "native-fixture-binding.json";
      const int descriptor = ::open(marker.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (descriptor < 0) refuse("fixture state binding could not be created exclusively");
      const auto close = rt::scopeExit([descriptor] { ::close(descriptor); });
      struct stat info {};
      if (::fstat(descriptor, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != ::getuid() || info.st_nlink != 1 || (info.st_mode & 0077) != 0)
        refuse("exclusive fixture state binding is not owner-private");
      std::size_t offset = 0;
      while (offset < binding.size()) {
        const ssize_t count = ::write(descriptor, binding.data() + offset, binding.size() - offset);
        if (count <= 0) refuse("fixture state binding write failed");
        offset += static_cast<std::size_t>(count);
      }
      if (::fsync(descriptor) != 0) refuse("fixture state binding durability failed");
      const int directory = ::open(data.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
      if (directory < 0) refuse("fixture state directory cannot be synchronized");
      const auto closeDirectory = rt::scopeExit([directory] { ::close(directory); });
      if (::fsync(directory) != 0) refuse("fixture state directory durability failed");
    }

    void grantProductionCodexAccess(permissions_linux::LinuxPermissions& permissions, const fs::path& package, const fs::path& state) {
      const auto homes = codex::native_auth::nativeCodexConsumerHomes();
      if (homes.empty()) refuse("the production VPS service requires a Codex owner home");
      const fs::path home = homes.front();
      privateDirectory(home);
      if (!disjoint(home, package) || !disjoint(home, state))
        refuse("the Codex owner home must be separate from the package and gateway state");

      const fs::path control = home / "app-server-control";
      const fs::path daemon = fs::path("/tmp") / ("codex-daemon-" + std::to_string(::getuid()));
      struct stat info {};
      if (::lstat(control.c_str(), &info) == 0) privateDirectory(control);
      else if (errno != ENOENT) refuse("the Codex control directory cannot be inspected");

      fs::path socket = control / "app-server-control.sock";
      fs::path directory = control;
      if (::lstat(socket.c_str(), &info) == 0) {
        if (S_ISLNK(info.st_mode)) {
          std::error_code error;
          socket = fs::read_symlink(socket, error);
          if (error || info.st_uid != ::getuid() || socket.lexically_normal() != socket || socket.parent_path() != daemon)
            refuse("the Codex control socket link must select its dedicated daemon directory");
          directory = daemon;
        }
      } else {
        if (errno != ENOENT) refuse("the Codex control socket cannot be inspected");
        // A daemon restart can leave the directory in place without a socket or link.
        if (::lstat(daemon.c_str(), &info) == 0) {
          directory = daemon;
          socket.clear();
        } else if (errno != ENOENT) {
          refuse("the Codex daemon directory cannot be inspected");
        }
      }
      if (!disjoint(directory, package) || !disjoint(directory, state))
        refuse("the Codex control directory must be separate from the package and gateway state");
      privateDirectory(directory);
      if (!socket.empty()) {
        if (::lstat(socket.c_str(), &info) == 0) {
          if (!S_ISSOCK(info.st_mode) || info.st_uid != ::getuid() || (info.st_mode & 0077) != 0)
            refuse("the Codex control socket must be owner-private");
        } else if (errno != ENOENT) {
          refuse("the Codex control socket cannot be inspected");
        }
      }

      permissions.readFilesBelow.push_back(home.string());
      permissions.resolveUnixSocketsBelow.push_back(directory.string());
    }

    void enforce(const fs::path& package, const fs::path& state, int port,
                 const std::optional<std::string>& origin, const std::optional<fs::path>& authSource,
                 const std::optional<fs::path>& leaderData) {
      if (upstream_auth::followerMode())
        std::fprintf(stderr, "%s=1: Antigravity disabled because its CLI manages OAuth refresh.\n", upstream_auth::FOLLOWER_ENV.data());
      auto profiles = permissions_linux::gatewayLinuxPermissions(package.string(), state.string());
      auto& permissions = profiles.gateway;
      if (authSource) permissions.read.push_back(authSource->string());
      std::error_code error;
      if (leaderData && fs::is_directory(*leaderData, error)) permissions.readFilesBelow.push_back(leaderData->string());
      if (port == 7999) grantProductionCodexAccess(permissions, package, state);
      if (const char* optIn = std::getenv("UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT"); optIn && std::string_view(optIn) == "1") {
        permissions_linux::LinuxPermissionsTcpPorts ports{static_cast<std::uint16_t>(port)};
        if (origin) ports.connectPorts.push_back(static_cast<std::uint16_t>(std::stoi(rt::URL(*origin).port())));
        else ports.connectPorts = {53, 443}; // DNS TCP fallback and the gateway's HTTPS/WSS upstreams.
        permissions.allowReducedConfinement = std::move(ports);
      }
      if (const char* optIn = std::getenv("UOS_GATEWAY_ALLOW_NO_LANDLOCK"); optIn && std::string_view(optIn) == "1") {
        permissions.allowNoLandlock = permissions_linux::ServiceManagerPaths{package.string(), state.string()};
      }
      const auto receipt = permissions_linux::applyLinuxPermissions(permissions);
      if (!receipt.processThreadSync || (!receipt.filesystemThreadSync && receipt.mode == "landlock"))
        refuse("kernel confinement did not produce both enforcement receipts");
      health::setConfinementMode(receipt.mode);
      if (receipt.mode == "service-manager") {
        std::fprintf(stderr, "Warning: Landlock is absent (confinement=service-manager). Outbound TCP is not restricted by the gateway. "
                             "The supplied systemd unit uses IPAddressDeny=multicast and RestrictAddressFamilies=AF_INET AF_INET6 AF_UNIX.\n");
        return;
      }
      if (profiles.antigravity) {
        provider::gemini::setGeminiChildConfinement([base = std::move(*profiles.antigravity)](const std::string& workspace) {
          auto child = base;
          child.read.push_back(workspace);
          child.write.push_back(workspace);
          permissions_linux::restrictLinuxChildThread(child);
        });
      }
      const std::string active = rt::join(receipt.activeProtections);
      const std::string missing = rt::join(receipt.missingProtections);
      std::fprintf(stderr, "Linux confinement Landlock ABI=%g active=%s missing=%s\n", receipt.landlockAbi, active.c_str(), missing.empty() ? "none" : missing.c_str());
    }

    rt::Task<void> serveGateway(serve::ServeStartupOptions options, std::optional<std::string> origin, std::string databasePath,
                                std::optional<fs::path> leaderData) {
      std::unique_ptr<FixtureRouting> routing;
      if (origin) routing = std::make_unique<FixtureRouting>(*origin);
      rt::Task<std::shared_ptr<rt::Kv>> opening = storage::kv::openKv(databasePath);
      std::shared_ptr<rt::Kv> store = co_await std::move(opening);
      auto closeStore = rt::scopeExit([store] {
        try {
          store->close();
        } catch (...) {}
      });
      kv::initializeKv(store);
      if (leaderData) {
        const auto source = *leaderData / fs::path(storage::kv::BUN_KV_DATABASE_PATH).filename();
        codex::auth::setLeaderAuthReader(codex::auth_import::liveReader(source.string(), store));
        co_await codex::auth::checkLeaderAuth();
      }
      const auto heapReclamation = rt::memory::startHeapReclamation();
      const auto stopHeapReclamation = rt::scopeExit([&] { rt::clearInterval(heapReclamation); });
      rt::Task<void> serving = serve::importMetaMain(options);
      closeStore.release();
      co_await std::move(serving);
    }

    rt::Task<void> importAuth(std::string source, std::string destination, std::optional<std::string> origin) {
      std::unique_ptr<FixtureRouting> routing;
      if (origin) routing = std::make_unique<FixtureRouting>(*origin, true);
      auto importing = codex::auth_import::importCodexAuthPool(std::move(source), std::move(destination));
      const auto count = co_await std::move(importing);
      std::printf("Imported updated Codex credentials for %zu account(s).\n", count);
    }
  }

  LaunchArguments parseArguments(int argc, const char* const* argv) {
    if ((argc != 3 && argc != 5) || std::string_view(argv[1]) != "--runtime-manifest" || !fs::path(argv[2]).is_absolute())
      refuse("expected exactly --runtime-manifest <absolute-path>");
    LaunchArguments arguments{argv[2], std::nullopt};
    if (argc == 5) {
      if (std::string_view(argv[3]) != "--import-codex-auth-from" || !fs::path(argv[4]).is_absolute())
        refuse("expected --import-codex-auth-from <absolute-state-directory>");
      arguments.codexAuthSource = argv[4];
    }
    return arguments;
  }

  rt::Task<void> run(LaunchArguments arguments) {
    const auto leaderRoot = rt::env::get(upstream_auth::LEADER_STATE_ENV);
    if (leaderRoot && !upstream_auth::followerMode())
      refuse("UOS_GATEWAY_UPSTREAM_AUTH_LEADER_STATE requires UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER=1");
    const fs::path launchPath = canonical(arguments.manifestPath);
    const json::object launch = object(launchPath, true);
    keys(launch, {"schema", "packageRoot", "stateRoot", "packageManifestSha256", "hostname", "port", "fixture"});
    if (string(launch, "schema") != "uos-gateway-launch-v1") refuse("launch manifest schema is unsupported");
    const fs::path package = canonical(string(launch, "packageRoot"));
    const fs::path state = canonical(string(launch, "stateRoot"));
    const fs::path data = state / ".data";
    const fs::path database = state / storage::kv::BUN_KV_DATABASE_PATH;
    privateDirectory(state);
    privateDirectory(data);
    if (!disjoint(state, package)) refuse("immutable package and mutable state must be disjoint");
    std::optional<fs::path> leaderData;
    if (leaderRoot) {
      const fs::path root(*leaderRoot);
      const fs::path candidate = root / ".data";
      std::error_code error;
      const auto resolved = fs::weakly_canonical(candidate, error);
      if (!root.is_absolute() || root.lexically_normal() != root || (!error && resolved != candidate))
        refuse("UOS_GATEWAY_UPSTREAM_AUTH_LEADER_STATE must name a canonical absolute state root");
      if (!disjoint(root, state) || !disjoint(root, package))
        refuse("leader state must be separate from the follower state and package");
      if (fs::equivalent(data, candidate, error))
        refuse("leader and follower data directories must be distinct");
      if (!arguments.codexAuthSource) leaderData = candidate;
    }
    std::optional<fs::path> authSource;
    if (arguments.codexAuthSource) {
      if (!upstream_auth::followerMode()) refuse("Codex auth import requires UOS_GATEWAY_UPSTREAM_AUTH_FOLLOWER=1");
      const fs::path source = canonical(*arguments.codexAuthSource);
      if (!disjoint(source, state) || !disjoint(source, package)) refuse("Codex auth source must be separate from the package and follower state");
      privateDirectory(source);
      privateDirectory(source / ".data");
      authSource = source / storage::kv::BUN_KV_DATABASE_PATH;
      requireStoppedDatabase(*authSource);
      requireStoppedDatabase(database);
      ::umask(0077);
    }
    const std::string packageHash = string(launch, "packageManifestSha256");
    verifyPackage(package, packageHash);
    verifyClosure(package);
    const json::object packageInfo = object(package / "package-info.json");
    if (string(packageInfo, "schema") != "uos-native-package-v1" || string(packageInfo, "platform") != "linux" || string(packageInfo, "architecture") != "arm64")
      refuse("package identity is unsupported");
    requireHash(packageInfo, "profileSha256", package / "build-profile.json");
    const std::string hostname = string(launch, "hostname");
    const auto portValue = rt::getNumber(launch, "port");
    if (hostname != "127.0.0.1" || !portValue || !std::isfinite(*portValue) || *portValue < 0 || *portValue > 65535 || std::floor(*portValue) != *portValue)
      refuse("listener must select an IPv4 loopback hostname and integer port");
    const int port = static_cast<int>(*portValue);
    if (port == 8000) refuse("port 8000 is reserved");
    if (port == 7999 && (!config::config.isProductionTimeline || config::config.isDeploy || config::config.adminTokens.empty() ||
                         upstream_auth::followerMode() || rt::get(launch, "fixture")))
      refuse("port 7999 requires the production VPS service configuration");
    std::optional<std::string> origin;
    std::string binding;
    bool newFixture = false;
    if (const json::value* fixture = rt::get(launch, "fixture")) {
      if (!fixture->is_object() || port != 0 || config::config.isDeploy) refuse("fixture requires a non-deploy loopback runtime and an assigned listener port");
      origin = fixtureOrigin(fixture->as_object());
      requireFixtureNetwork();
      binding = fixtureBinding(state, packageHash, *origin);
      newFixture = verifyFixtureState(data, binding);
    } else {
      regular(database, true);
    }
    // Load the existing operator-selected CA bundles while startup can read them.
    rt::detail::tls::defaultContext();
    enforce(package, state, port, origin, authSource, leaderData);
    if (newFixture) createFixtureBinding(data, binding);
    paths::setResourceRoot(package.string());
    if (::chdir(state.c_str()) != 0) refuse("private state working directory is unavailable");
    if (::setenv("SQLITE_TMPDIR", data.c_str(), 1) != 0) refuse("private SQLite temporary directory is unavailable");
    if (authSource) return importAuth(authSource->string(), database.string(), std::move(origin));
    return serveGateway(serve::ServeStartupOptions{hostname, port}, std::move(origin), database.string(), std::move(leaderData));
  }
}
