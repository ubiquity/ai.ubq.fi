#include "runtime/startup.hpp"

#include "runtime/paths.hpp"
#include "src/rt/tls_internal.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/kv.hpp"
#include "uos/live/upstream.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"
#include "uos/runtime/permissions_linux.hpp"
#include "uos/serve.hpp"
#include "uos/storage/kv.hpp"
#include "uos/storage/native_addon.hpp"

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

    void requireArm64Elf(const fs::path& path) {
      regular(path);
      std::ifstream input(path, std::ios::binary);
      std::array<unsigned char, 20> header{};
      input.read(reinterpret_cast<char*>(header.data()), static_cast<std::streamsize>(header.size()));
      if (input.gcount() != static_cast<std::streamsize>(header.size()) || header[0] != 0x7f || header[1] != 'E' || header[2] != 'L' || header[3] != 'F' ||
          header[4] != 2 || header[5] != 1 || header[18] != 183 || header[19] != 0)
        refuse("required artifact is not a Linux ARM64 ELF file: " + path.string());
    }

    void verifyPackage(const fs::path& root, const std::string& expected, std::string_view executable = "uos-gateway") {
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
      for (const std::string_view required : {"bin/uos-gateway", "bin/uos-kv-convert", "bin/uos-kv-proof", "package.json", "runtime-closure.json",
                                               "native/kv/inputs.json", "native/kv/native-kv.node", "native/kv/native-kv.provenance.json",
                                               "native/permissions/manifest.json", "native/permissions/linux-arm64.node", "static/index.html", "static/openapi.json",
                                               "runtime/launch-schema.json", "runtime/offline-schema.json", "src/kv-conversion/v8-helper.mjs", "src/kv-conversion/typed-value.ts"}) {
        if (!selected.contains(std::string(required))) refuse("package lacks a required file: " + std::string(required));
      }
      if (canonical(rt::process::execPath()) != root / "bin" / std::string(executable)) refuse("executable is outside the selected package");
    }

    void verifyClosure(const fs::path& root) {
      const json::object closure = object(root / "runtime-closure.json");
      if (string(closure, "schema") != "uos-native-runtime-closure-v1" || string(closure, "platform") != "linux" || string(closure, "machine") != "AArch64")
        refuse("runtime closure platform is unsupported");
      const json::value* libraries = rt::get(closure, "libraries");
      if (!libraries || !libraries->is_array() || libraries->as_array().empty()) refuse("runtime host-library closure is missing");
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
      explicit FixtureTransport(std::string origin) : origin_(std::move(origin)), original_(rt::fetchTransport()) {
        if (!original_) refuse("real HTTP client transport is not installed");
      }
      rt::Task<rt::Response> send(rt::Request request) override {
        // The real client follows redirects internally. A fixture refuses them
        // before any second destination can bypass this outer guard.
        rt::RequestInit policy;
        policy.redirect = "error";
        rt::Request guarded(request, policy);
        const std::string destination = guarded.url();
        if (destination == deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL) {
          rt::Request mapped(origin_ + "/chat/completions", guarded);
          return original_->send(std::move(mapped));
        }
        if (destination.starts_with(origin_ + "/codex/") || destination == origin_ + "/chat/completions") return original_->send(std::move(guarded));
        refuse("fixture refused an unmapped provider destination");
      }

    private:
      std::string origin_;
      std::shared_ptr<rt::FetchTransport> original_;
    };

    struct FixtureRouting {
      std::shared_ptr<rt::FetchTransport> original;
      std::string originalCodexBase;
      explicit FixtureRouting(const std::string& origin) : original(rt::fetchTransport()), originalCodexBase(config::config.codexBaseUrl) {
        rt::setFetchTransport(std::make_shared<FixtureTransport>(origin));
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

    void enforce(const fs::path& package, const fs::path& state, const permissions_linux::LinuxPermissionsArtifact& artifact, int port,
                 const std::optional<std::string>& origin) {
      permissions_linux::LinuxPermissions permissions;
      permissions.read = {package.string(), state.string()};
      permissions.write = {(state / ".data").string()};
      permissions.immutable = std::vector<std::string>{package.string()};
      for (const std::string_view name : {"/usr/lib", "/lib", "/usr/bin", "/etc/ssl", "/etc/resolv.conf", "/etc/hosts", "/etc/nsswitch.conf", "/etc/localtime", "/etc/passwd", "/etc/group", "/dev/null", "/dev/urandom", "/dev/random", "/proc/self"}) {
        if (fs::exists(name)) permissions.read.push_back(fs::canonical(name).string());
      }
      // The owner-authorized Antigravity CLI is the only executable admitted
      // through the otherwise fork/exec-denying boundary. Its credential and
      // session state is the one existing home subtree deliberately shared
      // with the gateway; no other home path or executable is granted.
      if (const auto home = rt::env::get("HOME"); home && !home->empty()) {
        const fs::path executable = fs::path(*home) / ".local/bin/agy";
        const fs::path cliState = fs::path(*home) / ".gemini/antigravity-cli";
        struct stat executableInfo {};
        struct stat cliInfo {};
        if (::lstat(executable.c_str(), &executableInfo) == 0 && S_ISREG(executableInfo.st_mode) && !S_ISLNK(executableInfo.st_mode) &&
            executableInfo.st_nlink == 1 && executableInfo.st_uid == ::getuid() && (executableInfo.st_mode & 0077) == 0 && (executableInfo.st_mode & 0100) != 0 &&
            ::lstat(cliState.c_str(), &cliInfo) == 0 && S_ISDIR(cliInfo.st_mode) && !S_ISLNK(cliInfo.st_mode) && cliInfo.st_uid == ::getuid() &&
            (cliInfo.st_mode & 0077) == 0) {
          permissions.read.push_back(fs::canonical(cliState).string());
          permissions.write.push_back(fs::canonical(cliState).string());
          permissions.execute.push_back(fs::canonical(executable).string());
          const fs::path loader = "/lib/ld-linux-aarch64.so.1";
          if (fs::exists(loader)) permissions.execute.push_back(fs::canonical(loader).string());
          const fs::path pluginConfig = fs::path(*home) / ".config/antigravity";
          struct stat pluginInfo {};
          if (::lstat(pluginConfig.c_str(), &pluginInfo) == 0 && S_ISDIR(pluginInfo.st_mode) && !S_ISLNK(pluginInfo.st_mode) &&
              pluginInfo.st_uid == ::getuid() && (pluginInfo.st_mode & 0022) == 0) {
            permissions.read.push_back(fs::canonical(pluginConfig).string());
          }
        }
      }
      if (const char* optIn = std::getenv("UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT"); optIn && std::string_view(optIn) == "1") {
        permissions_linux::LinuxPermissionsTcpPorts ports{static_cast<std::uint16_t>(port)};
        if (origin) ports.connectPorts.push_back(static_cast<std::uint16_t>(std::stoi(rt::URL(*origin).port())));
        else ports.connectPorts = {53, 443}; // DNS TCP fallback and the gateway's HTTPS/WSS upstreams.
        permissions.allowReducedConfinement = std::move(ports);
      }
      const auto receipt = permissions_linux::applyLinuxPermissions(permissions, artifact);
      if (!receipt.processThreadSync || (!receipt.filesystemThreadSync && !permissions.allowReducedConfinement))
        refuse("kernel confinement did not produce both enforcement receipts");
      const std::string active = rt::join(receipt.activeProtections);
      const std::string missing = rt::join(receipt.missingProtections);
      std::fprintf(stderr, "Linux confinement Landlock ABI=%g active=%s missing=%s\n", receipt.landlockAbi, active.c_str(), missing.empty() ? "none" : missing.c_str());
    }

    rt::Task<void> serveGateway(serve::ServeStartupOptions options, std::optional<std::string> origin, std::string databasePath) {
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
      rt::Task<void> serving = serve::importMetaMain(options);
      closeStore.release();
      co_await std::move(serving);
    }
  }

  std::string manifestArgument(int argc, const char* const* argv) {
    if (argc != 3 || std::string_view(argv[1]) != "--runtime-manifest" || !fs::path(argv[2]).is_absolute())
      refuse("expected exactly --runtime-manifest <absolute-path>");
    return argv[2];
  }

  void prepareOfflineCli(int argc, const char* const* argv) {
    if (argc < 3 || std::string_view(argv[1]) != "--runtime-manifest" || !fs::path(argv[2]).is_absolute())
      refuse("expected prefix --runtime-manifest <absolute-path> before offline CLI arguments");
    const fs::path launchPath = canonical(argv[2]);
    const json::object launch = object(launchPath, true);
    keys(launch, {"schema", "packageRoot", "packageManifestSha256", "nativeKvProvenanceSha256"});
    if (string(launch, "schema") != "uos-gateway-offline-v1") refuse("offline CLI manifest schema is unsupported");
    const fs::path package = canonical(string(launch, "packageRoot"));
    const std::string executable = fs::path(rt::process::execPath()).filename().string();
    if (executable != "uos-kv-convert" && executable != "uos-kv-proof") refuse("offline preflight requires a packaged offline CLI executable");
    verifyPackage(package, string(launch, "packageManifestSha256"), executable);
    verifyClosure(package);
    const json::object packageInfo = object(package / "package.json");
    if (string(packageInfo, "schema") != "uos-native-package-v1" || string(packageInfo, "platform") != "linux" || string(packageInfo, "architecture") != "arm64")
      refuse("package identity is unsupported");
    const std::string provenance = string(launch, "nativeKvProvenanceSha256");
    if (provenance != string(packageInfo, "nativeKvProvenanceSha256")) refuse("native KV provenance differs from package binding");
    storage::native_addon::NativeKvArtifactBinding backend;
    backend.artifactDirectory = (package / "native/kv").string();
    backend.expectedProvenanceSha256 = provenance;
    backend.inputsManifest = (package / "native/kv/inputs.json").string();
    requireArm64Elf(package / "native/kv/native-kv.node");
    storage::native_addon::prepareNativeKvBackend(backend);
    const json::object permissionsInfo = object(package / "native/permissions/manifest.json");
    if (string(permissionsInfo, "path") != "linux-arm64.node" || string(permissionsInfo, "architecture") != "arm64") refuse("Linux confinement manifest is invalid");
    const fs::path permissionsPath = package / "native/permissions/linux-arm64.node";
    requireArm64Elf(permissionsPath);
    requireHash(permissionsInfo, "sha256", permissionsPath);
    paths::setResourceRoot(package.string());
    std::vector<const char*> application;
    application.reserve(static_cast<std::size_t>(argc - 2));
    application.push_back(argv[0]);
    for (int index = 3; index < argc; ++index) application.push_back(argv[index]);
    rt::process::setArgv(static_cast<int>(application.size()), application.data());
  }

  rt::Task<void> run(std::string manifestPath) {
    const fs::path launchPath = canonical(manifestPath);
    const json::object launch = object(launchPath, true);
    keys(launch, {"schema", "packageRoot", "stateRoot", "packageManifestSha256", "nativeKvProvenanceSha256", "hostname", "port", "fixture"});
    if (string(launch, "schema") != "uos-gateway-launch-v1") refuse("launch manifest schema is unsupported");
    const fs::path package = canonical(string(launch, "packageRoot"));
    const fs::path state = canonical(string(launch, "stateRoot"));
    const fs::path data = state / ".data";
    privateDirectory(state);
    privateDirectory(data);
    const auto relative = state.lexically_relative(package);
    const auto packageRelative = package.lexically_relative(state);
    if (relative.empty() || (!relative.empty() && *relative.begin() != "..") || packageRelative.empty() || (!packageRelative.empty() && *packageRelative.begin() != ".."))
      refuse("immutable package and mutable state must be disjoint");
    const std::string packageHash = string(launch, "packageManifestSha256");
    verifyPackage(package, packageHash);
    verifyClosure(package);
    const json::object packageInfo = object(package / "package.json");
    if (string(packageInfo, "schema") != "uos-native-package-v1" || string(packageInfo, "platform") != "linux" || string(packageInfo, "architecture") != "arm64")
      refuse("package identity is unsupported");
    const std::string provenance = string(launch, "nativeKvProvenanceSha256");
    if (provenance != string(packageInfo, "nativeKvProvenanceSha256")) refuse("native KV provenance differs from package binding");
    storage::native_addon::NativeKvArtifactBinding backend;
    backend.artifactDirectory = (package / "native/kv").string();
    backend.expectedProvenanceSha256 = provenance;
    backend.inputsManifest = (package / "native/kv/inputs.json").string();
    requireArm64Elf(package / "native/kv/native-kv.node");
    storage::native_addon::prepareNativeKvBackend(backend);
    const json::object permissionsInfo = object(package / "native/permissions/manifest.json");
    if (string(permissionsInfo, "path") != "linux-arm64.node" || string(permissionsInfo, "architecture") != "arm64") refuse("Linux confinement manifest is invalid");
    const fs::path permissionsPath = package / "native/permissions/linux-arm64.node";
    requireArm64Elf(permissionsPath);
    requireHash(permissionsInfo, "sha256", permissionsPath);
    const permissions_linux::LinuxPermissionsArtifact artifact{permissionsPath.string(), string(permissionsInfo, "sha256")};
    const std::string hostname = string(launch, "hostname");
    const auto portValue = rt::getNumber(launch, "port");
    if (hostname != "127.0.0.1" || !portValue || !std::isfinite(*portValue) || *portValue < 0 || *portValue > 65535 || std::floor(*portValue) != *portValue)
      refuse("listener must select an IPv4 loopback hostname and integer port");
    const int port = static_cast<int>(*portValue);
    if (port == 7999 || port == 8000) refuse("production service ports are unavailable to this uninstalled build");
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
      regular(data / "kv-bun.sqlite3", true);
    }
    // Load the existing operator-selected CA bundles while startup can read them.
    rt::detail::tls::defaultContext();
    enforce(package, state, artifact, port, origin);
    if (newFixture) createFixtureBinding(data, binding);
    paths::setResourceRoot(package.string());
    if (::chdir(state.c_str()) != 0) refuse("private state working directory is unavailable");
    if (::setenv("SQLITE_TMPDIR", data.c_str(), 1) != 0) refuse("private SQLite temporary directory is unavailable");
    return serveGateway(serve::ServeStartupOptions{hostname, port}, std::move(origin), (data / "kv-bun.sqlite3").string());
  }
}
