// Port of src/storage/native-addon.ts.
#include "uos/storage/native_addon.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/regex.hpp"
#include <array>
#include <span>
#include <string_view>

namespace uos::storage::native_addon {
  namespace {
    using PathLike = std::variant<std::string, rt::URL>;

    constexpr std::string_view SOURCE_COMMIT = "9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5";

    const std::string& INPUTS_MANIFEST() {
      static const std::string path = rt::path::resolve(rt::path::dirname(__FILE__), "../../../../tools/native-kv/inputs.json");
      return path;
    }

    constexpr std::string_view PROVENANCE_FILE = "native-kv.provenance.json";
    constexpr std::string_view ARTIFACT_FILE = "native-kv.node";
    constexpr std::array<std::string_view, 9> NATIVE_EXPORTS = {
      "open", "close", "snapshotRead", "atomicWrite", "dequeueNextMessage", "finishMessage", "startWatch", "dequeueNextWatchMessage", "endWatch",
    };

    std::string sha256(std::span<const std::uint8_t> bytes) { return rt::crypto::createHash("sha256").update(bytes).digest("hex"); }

    const rt::Regex& SHA256_HEX() {
      static const rt::Regex regex("^[0-9a-f]{64}$");
      return regex;
    }

    const json::object& object(const json::value* value) {
      if (value == nullptr || !value->is_object()) throw rt::JsError("Invalid native KV provenance object");
      return value->as_object();
    }

    std::string pathOf(const PathLike& value) {
      if (const auto* url = std::get_if<rt::URL>(&value)) return rt::fileURLToPath(*url);
      return std::get<std::string>(value);
    }

    rt::fs::Bytes readRegularFile(const std::string& path) {
      if (!rt::fs::lstatSync(path).isFile()) throw rt::JsError("Native KV artifacts must be regular files, not symlinks");
      return rt::fs::readFileSync(path);
    }

    // `value === text` / `value === number` / `a === b` on members of parsed JSON
    // (nullptr is undefined).
    bool isStringValue(const json::value* value, std::string_view text) { return value != nullptr && value->is_string() && value->as_string() == text; }
    bool isNumberValue(const json::value* value, double number) {
      const std::optional<double> actual = rt::getNumber(value);
      return actual.has_value() && *actual == number;
    }
    bool strictEquals(const json::value* left, const json::value* right) {
      if (left == nullptr || right == nullptr) return left == right;
      return rt::strictEquals(*left, *right);
    }
    // `isDeepStrictEqual(left, right)` from node:util.
    bool isDeepStrictEqual(const json::value* left, const json::value* right) {
      if (left == nullptr || right == nullptr) return left == right;
      return rt::deepEqual(*left, *right);
    }

    bool exportIsFunction(const NapiInterface& exports, std::string_view name) {
      if (name == "open") return static_cast<bool>(exports.open);
      if (name == "close") return static_cast<bool>(exports.close);
      if (name == "snapshotRead") return static_cast<bool>(exports.snapshotRead);
      if (name == "atomicWrite") return static_cast<bool>(exports.atomicWrite);
      if (name == "dequeueNextMessage") return static_cast<bool>(exports.dequeueNextMessage);
      if (name == "finishMessage") return static_cast<bool>(exports.finishMessage);
      if (name == "startWatch") return static_cast<bool>(exports.startWatch);
      if (name == "dequeueNextWatchMessage") return static_cast<bool>(exports.dequeueNextWatchMessage);
      if (name == "endWatch") return static_cast<bool>(exports.endWatch);
      return false;
    }
  }

  VerifiedNativeKvArtifact verifyNativeKvArtifact(const NativeKvArtifactBinding& binding) {
    if (!SHA256_HEX().test(binding.expectedProvenanceSha256)) throw rt::JsError("Native KV requires an independently bound provenance hash");
    const std::string directory = pathOf(binding.artifactDirectory);
    if (!rt::fs::lstatSync(directory).isDirectory()) throw rt::JsError("Native KV artifact directory must not be a symlink");
    const rt::fs::Bytes provenanceBytes = readRegularFile(rt::path::join(directory, PROVENANCE_FILE));
    const std::string provenanceSha256 = sha256(provenanceBytes);
    if (provenanceSha256 != binding.expectedProvenanceSha256) throw rt::JsError("Native KV provenance hash does not match its release binding");
    const rt::fs::Bytes inputsBytes = readRegularFile(binding.inputsManifest ? pathOf(*binding.inputsManifest) : INPUTS_MANIFEST());
    const json::value inputsValue = rt::jsonParse(rt::bufferToString(inputsBytes));
    const json::object& inputs = object(&inputsValue);
    const json::value provenanceValue = rt::jsonParse(rt::bufferToString(provenanceBytes));
    const json::object& provenance = object(&provenanceValue);
    const std::string inputsSha256 = sha256(inputsBytes);
    if (!isNumberValue(rt::get(inputs, "schema"), 1) || !isNumberValue(rt::get(provenance, "schema"), 1) || !isStringValue(rt::get(provenance, "inputsSha256"), inputsSha256))
      throw rt::JsError("Native KV frozen inputs do not match provenance");
    const json::object& build = object(rt::get(provenance, "build"));
    const std::optional<std::string_view> logSha256 = rt::getString(rt::get(build, "logSha256"));
    if (!isNumberValue(rt::get(build, "exitCode"), 0) || !logSha256 || !SHA256_HEX().test(*logSha256))
      throw rt::JsError("Native KV build did not complete successfully");
    const json::object& sdk = object(rt::get(inputs, "sdk"));
    if (!isStringValue(rt::get(sdk, "name"), "@deno/kv") || !isStringValue(rt::get(sdk, "version"), "0.14.0") ||
        !isStringValue(rt::get(object(rt::get(inputs, "source")), "commit"), SOURCE_COMMIT))
      throw rt::JsError("Native KV source identity is unsupported");
    for (const std::string_view field : {"source", "patches", "lock", "toolchain", "recipe"}) {
      if (!isDeepStrictEqual(rt::get(provenance, field), rt::get(inputs, field)))
        throw rt::JsError("Native KV provenance " + std::string(field) + " differs from frozen inputs");
    }
    const json::object& expectedArtifact = object(rt::get(inputs, "artifact"));
    const json::object& artifact = object(rt::get(provenance, "artifact"));
    for (const std::string_view field : {"file", "platform", "architecture", "target", "napiVersion"}) {
      if (!strictEquals(rt::get(artifact, field), rt::get(expectedArtifact, field)))
        throw rt::JsError("Native KV artifact " + std::string(field) + " differs from frozen inputs");
    }
    if (!isStringValue(rt::get(artifact, "file"), ARTIFACT_FILE) || !isStringValue(rt::get(artifact, "platform"), rt::process::platform) ||
        !isStringValue(rt::get(artifact, "architecture"), rt::process::arch) || !isNumberValue(rt::get(artifact, "napiVersion"), 4)) {
      throw rt::JsError("Native KV artifact does not support this runtime platform or ABI");
    }
    const std::optional<std::string_view> expectedSha256 = rt::getString(rt::get(artifact, "sha256"));
    if (!expectedSha256 || !SHA256_HEX().test(*expectedSha256)) throw rt::JsError("Native KV artifact hash is invalid");
    const std::string path = rt::path::join(directory, ARTIFACT_FILE);
    const std::string artifactSha256 = sha256(readRegularFile(path));
    if (artifactSha256 != *expectedSha256) throw rt::JsError("Native KV artifact hash does not match provenance");
    return VerifiedNativeKvArtifact{rt::fs::realpathSync(path), artifactSha256, provenanceSha256, inputsSha256};
  }

  namespace {
    struct Prepared {
      std::string binding;
      NativeKvArtifactBinding artifact;
      NapiInterface napi;
    };

    std::optional<Prepared>& prepared() {
      static std::optional<Prepared> value;
      return value;
    }
  }

  void prepareNativeKvBackend(const NativeKvArtifactBinding& binding) {
    const VerifiedNativeKvArtifact verified = verifyNativeKvArtifact(binding);
    const std::string identity = verified.path + ":" + verified.provenanceSha256;
    if (prepared()) {
      if (prepared()->binding != identity) throw rt::JsError("Native KV backend was already prepared for another artifact");
      return;
    }
    const std::optional<NapiInterface> loaded = nativeKvRequire()(verified.path);
    if (!loaded) throw rt::JsError("Invalid native KV provenance object");
    const NapiInterface& exports = *loaded;
    for (const std::string_view name : NATIVE_EXPORTS) {
      if (!exportIsFunction(exports, name)) throw rt::JsError("Native KV artifact exports do not satisfy its pinned NAPI interface");
    }
    NativeKvArtifactBinding artifact;
    artifact.artifactDirectory = rt::path::dirname(verified.path);
    artifact.expectedProvenanceSha256 = verified.provenanceSha256;
    artifact.inputsManifest = rt::fs::realpathSync(binding.inputsManifest ? pathOf(*binding.inputsManifest) : INPUTS_MANIFEST());
    prepared() = Prepared{identity, std::move(artifact), exports};
  }

  bool nativeKvBackendIsPrepared() { return prepared().has_value(); }

  NativeKvArtifactBinding preparedNativeKvArtifactBinding() {
    if (!prepared()) throw rt::JsError("Prepare the verified native KV artifact before starting a child");
    return prepared()->artifact;
  }

  NapiInterface preparedNativeKvBackend() {
    if (!prepared()) throw rt::JsError("Prepare the verified native KV artifact before opening a database");
    return prepared()->napi;
  }

  namespace {
    // The C++ runtime links the native store (rt::SqliteKv, docs/rt/kv-durable.md) instead of
    // loading the verified Node-API module: storage::kv::openNativeKv() opens that store
    // directly, so these nine exports complete the pinned interface without carrying the
    // addon's protobuf requests.
    template <class Result, class... Arguments> std::function<Result(Arguments...)> linkedExport(std::string_view name) {
      const std::string message =
        "Native KV export '" + std::string(name) + "' is not called in the C++ runtime: storage::kv::openNativeKv opens the linked rt::SqliteKv store directly";
      return [message](Arguments...) -> Result { throw rt::JsError(message); };
    }

    NapiInterface linkedNapiInterface() {
      NapiInterface napi;
      napi.open = linkedExport<double, std::string, std::optional<bool>, bool>("open");
      napi.close = linkedExport<void, double, bool>("close");
      napi.snapshotRead = linkedExport<rt::Promise<std::vector<std::uint8_t>>, double, std::vector<std::uint8_t>, bool>("snapshotRead");
      napi.atomicWrite = linkedExport<rt::Promise<std::vector<std::uint8_t>>, double, std::vector<std::uint8_t>, bool>("atomicWrite");
      napi.dequeueNextMessage = linkedExport<rt::Promise<std::optional<NapiDequeuedMessage>>, double, bool>("dequeueNextMessage");
      napi.finishMessage = linkedExport<rt::Promise<void>, double, double, bool, bool>("finishMessage");
      napi.startWatch = linkedExport<rt::Promise<double>, double, std::vector<std::uint8_t>, bool>("startWatch");
      napi.dequeueNextWatchMessage = linkedExport<rt::Promise<std::optional<std::vector<std::uint8_t>>>, double, double, bool>("dequeueNextWatchMessage");
      napi.endWatch = linkedExport<void, double, double, bool>("endWatch");
      return napi;
    }
  }

  NativeKvRequire& nativeKvRequire() {
    static NativeKvRequire require = [](const std::string&) -> std::optional<NapiInterface> { return linkedNapiInterface(); };
    return require;
  }
}
