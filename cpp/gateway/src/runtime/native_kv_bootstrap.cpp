// Port of src/runtime/native-kv-bootstrap.ts.
#include "uos/runtime/native_kv_bootstrap.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::runtime::native_kv_bootstrap {
  using storage::native_addon::NativeKvArtifactBinding;

  namespace {
    const std::string& REPOSITORY_ROOT() {
      static const std::string root = rt::path::resolve(rt::path::dirname(__FILE__), "../../../..");
      return root;
    }

    constexpr std::string_view BINDING_FILE = "native-kv.binding.json";

    // `stats.dev` and the other Stats numbers as JS reads them.
    double number(std::uint64_t value) { return static_cast<double>(value); }

    void checkOwner(const rt::fs::Stats& info) {
      const std::uint32_t uid = rt::process::getuid();
      if (info.uid != uid || (info.mode & 0077) != 0) throw rt::JsError("Native KV development storage must be owner-private");
    }

    json::value privateReceipt(const std::string& path) {
      const rt::fs::Stats before = rt::fs::lstatSync(path);
      if (!before.isFile() || before.nlink != 1 || rt::fs::realpathSync(path) != path)
        throw rt::JsError("Native KV development binding must be a regular file, not a symlink");
      checkOwner(before);
      const int descriptor = rt::fs::openSync(path, rt::fs::constants::O_RDONLY_ | rt::fs::constants::O_NOFOLLOW_);
      json::value parsed;
      try {
        const rt::fs::Stats info = rt::fs::fstatSync(descriptor);
        if (number(info.dev) != number(before.dev) || number(info.ino) != number(before.ino) || info.size > 16384)
          throw rt::JsError("Native KV development binding changed or is oversized");
        checkOwner(info);
        parsed = rt::jsonParse(rt::fs::readFileSync(descriptor, "utf8"));
      } catch (...) {
        rt::fs::closeSync(descriptor);
        throw;
      }
      rt::fs::closeSync(descriptor);
      return parsed;
    }

    const rt::Regex& SHA256_HEX() {
      static const rt::Regex regex("^[0-9a-f]{64}$");
      return regex;
    }

    NativeKvArtifactBinding bindingDocument(const json::value& value) {
      if (!value.is_object()) throw rt::JsError("Invalid native KV development binding");
      const json::object& fields = value.as_object();
      constexpr std::array<std::string_view, 3> allowed = {"artifactDirectory", "expectedProvenanceSha256", "inputsManifest"};
      bool unknownKey = false;
      for (const std::string& key : rt::objectKeys(fields)) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) unknownKey = true;
      }
      const std::optional<std::string_view> artifactDirectory = rt::getString(fields, "artifactDirectory");
      const std::optional<std::string_view> expectedProvenanceSha256 = rt::getString(fields, "expectedProvenanceSha256");
      const json::value* inputsManifest = rt::get(fields, "inputsManifest");
      if (unknownKey || !artifactDirectory || !rt::path::isAbsolute(*artifactDirectory) || !expectedProvenanceSha256 ||
          !SHA256_HEX().test(*expectedProvenanceSha256) || (inputsManifest != nullptr && !inputsManifest->is_string())) {
        throw rt::JsError("Native KV development binding requires an independently supplied provenance hash and absolute paths");
      }
      NativeKvArtifactBinding binding;
      binding.artifactDirectory = std::string(*artifactDirectory);
      binding.expectedProvenanceSha256 = std::string(*expectedProvenanceSha256);
      if (inputsManifest != nullptr) binding.inputsManifest = std::string(inputsManifest->as_string());
      return binding;
    }
  }

  NativeKvDevelopmentPaths nativeKvDevelopmentPaths(const std::optional<std::string>& repositoryRoot) {
    const std::string root = rt::path::resolve(repositoryRoot.value_or(REPOSITORY_ROOT()));
    if (rt::fs::realpathSync(root) != root || !rt::fs::lstatSync(root).isDirectory()) throw rt::JsError("Native KV repository root must be a real directory");
    if (rt::process::arch != "arm64" || !(rt::process::platform == "linux" || rt::process::platform == "darwin"))
      throw rt::JsError("Native KV development platform is unsupported");
    const std::string data = rt::path::join(root, ".data");
    const std::string inputs = rt::path::join(root, "cpp/gateway/packaging/native-kv", rt::process::platform == "darwin" ? "inputs-darwin-arm64.json" : "inputs.json");
    return NativeKvDevelopmentPaths{root, data, rt::path::join(data, BINDING_FILE), inputs};
  }

  void verifyNativeKvDevelopmentDirectory(const std::string& path) {
    const rt::fs::Stats info = rt::fs::lstatSync(path);
    if (!info.isDirectory() || rt::fs::realpathSync(path) != path) throw rt::JsError("Native KV development directory must not contain symlinks");
    checkOwner(info);
  }

  NativeKvArtifactBinding readNativeKvDevelopmentBinding(const std::optional<std::string>& repositoryRoot) {
    const NativeKvDevelopmentPaths paths = nativeKvDevelopmentPaths(repositoryRoot);
    verifyNativeKvDevelopmentDirectory(paths.data);
    const NativeKvArtifactBinding binding = bindingDocument(privateReceipt(paths.receipt));
    const std::string artifact = rt::path::resolve(std::get<std::string>(binding.artifactDirectory));
    const std::string beneath = rt::path::relative(paths.data, artifact);
    if (beneath.empty() || beneath == ".." || rt::startsWith(beneath, std::string("..") + std::string(rt::path::sep)) || rt::path::isAbsolute(beneath)) {
      throw rt::JsError("Native KV development artifact must stay beneath repository .data");
    }
    std::string directory = paths.data;
    for (const std::string& part : rt::split(beneath, rt::path::sep)) {
      directory = rt::path::join(directory, part);
      verifyNativeKvDevelopmentDirectory(directory);
    }
    if (binding.inputsManifest && std::get<std::string>(*binding.inputsManifest) != paths.inputs) {
      throw rt::JsError("Native KV development binding must use the current tracked platform inputs");
    }
    const rt::fs::Stats inputs = rt::fs::lstatSync(paths.inputs);
    if (!inputs.isFile() || rt::fs::realpathSync(paths.inputs) != paths.inputs || (inputs.mode & 0022) != 0) {
      throw rt::JsError("Native KV tracked inputs must be safe regular files without symlinks");
    }
    for (const std::string& path : {rt::path::join(artifact, "native-kv.node"), rt::path::join(artifact, "native-kv.provenance.json")}) {
      const rt::fs::Stats info = rt::fs::lstatSync(path);
      if (!info.isFile() || info.nlink != 1 || rt::fs::realpathSync(path) != path) throw rt::JsError("Native KV development artifacts must not contain symlinks");
      checkOwner(info);
    }
    NativeKvArtifactBinding checked;
    checked.artifactDirectory = artifact;
    checked.expectedProvenanceSha256 = binding.expectedProvenanceSha256;
    checked.inputsManifest = paths.inputs;
    storage::native_addon::verifyNativeKvArtifact(checked);
    return checked;
  }

  rt::Promise<void> prepareNativeKvForDevelopment(std::optional<std::string> repositoryRoot) {
    const rt::Promise<void> ready = rt::Promise<void>::resolve();
    std::function<void()> preparation = [repositoryRoot] {
      if (!storage::native_addon::nativeKvBackendIsPrepared()) storage::native_addon::prepareNativeKvBackend(readNativeKvDevelopmentBinding(repositoryRoot));
    };
    return ready.then(std::move(preparation));
  }
}
