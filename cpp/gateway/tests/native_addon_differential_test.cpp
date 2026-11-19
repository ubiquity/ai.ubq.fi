// Differential cases for src/storage/native-addon.ts and src/runtime/native-kv-bootstrap.ts
// recorded from Bun 1.4.0: verifyNativeKvArtifact over one fixture per verification
// branch, and readNativeKvDevelopmentBinding over the development-startup fixture with
// one tweak per receipt, layout and permission rule. Fixture paths are recorded as
// "<root>". Then the prepare/prepared lifecycle with an injected native backend (the TS
// loads a real Node-API addon there, which these vectors cannot record).
#include "uos/runtime/native_kv_bootstrap.hpp"
#include "uos/storage/native_addon.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <unistd.h>

#include <cstdio>
#include <optional>
#include <string>
#include <vector>

using namespace uos;
namespace native_addon = uos::storage::native_addon;
namespace bootstrap = uos::runtime::native_kv_bootstrap;

namespace {
#if defined(__APPLE__)
#include "native_addon_darwin_vectors.inc"
#else
  const char* const VECTORS = R"VECTORS({"verify":[{"spec":{"name":"valid"},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsSha256":"faa6d6b42151e44b2339f4ee0f230076f95c47c789be9778affd359c31890532"}}},{"spec":{"name":"uppercase expected hash","expectedHash":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"},"outcome":{"error":{"name":"Error","message":"Native KV requires an independently bound provenance hash"}}},{"spec":{"name":"short expected hash","expectedHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"outcome":{"error":{"name":"Error","message":"Native KV requires an independently bound provenance hash"}}},{"spec":{"name":"missing directory","fs":"missing-dir"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/absent'"}}},{"spec":{"name":"directory symlink","fs":"dir-symlink"},"outcome":{"error":{"name":"Error","message":"Native KV artifact directory must not be a symlink"}}},{"spec":{"name":"provenance symlink","fs":"provenance-symlink"},"outcome":{"error":{"name":"Error","message":"Native KV artifacts must be regular files, not symlinks"}}},{"spec":{"name":"provenance missing","fs":"provenance-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/artifact/native-kv.provenance.json'"}}},{"spec":{"name":"wrong expected hash","expectedHash":"0000000000000000000000000000000000000000000000000000000000000000"},"outcome":{"error":{"name":"Error","message":"Native KV provenance hash does not match its release binding"}}},{"spec":{"name":"inputs missing","fs":"inputs-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/artifact/absent.json'"}}},{"spec":{"name":"inputs not json","inputsText":"{\"schema\": 1,"},"outcome":{"error":{"name":"SyntaxError","message":"Invalid JSON"}}},{"spec":{"name":"inputs array","inputsText":"[1,2]"},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"inputs with bom","inputsText":"﻿{}"},"outcome":{"error":{"name":"SyntaxError","message":"Invalid JSON"}}},{"spec":{"name":"provenance not json","provenanceText":"{broken"},"outcome":{"error":{"name":"SyntaxError","message":"Invalid JSON"}}},{"spec":{"name":"provenance null","provenanceText":"null"},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"inputs schema 2","inputsSet":{"schema":2}},"outcome":{"error":{"name":"Error","message":"Native KV frozen inputs do not match provenance"}}},{"spec":{"name":"provenance schema string","provenanceSet":{"schema":"1"}},"outcome":{"error":{"name":"Error","message":"Native KV frozen inputs do not match provenance"}}},{"spec":{"name":"provenance inputs hash wrong","provenanceSet":{"inputsSha256":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"}},"outcome":{"error":{"name":"Error","message":"Native KV frozen inputs do not match provenance"}}},{"spec":{"name":"build missing","provenanceDelete":["build"]},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"build array","provenanceSet":{"build":[0]}},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"build exit 1","provenanceSet":{"build.exitCode":1}},"outcome":{"error":{"name":"Error","message":"Native KV build did not complete successfully"}}},{"spec":{"name":"build exit negative zero","provenanceReplace":["\"exitCode\":0","\"exitCode\":-0"]},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"28d46814a68cf806dd8f5ac1184b0ab10c36584a4560587a0c98232c61319473","inputsSha256":"faa6d6b42151e44b2339f4ee0f230076f95c47c789be9778affd359c31890532"}}},{"spec":{"name":"build exit string","provenanceSet":{"build.exitCode":"0"}},"outcome":{"error":{"name":"Error","message":"Native KV build did not complete successfully"}}},{"spec":{"name":"build log uppercase","provenanceSet":{"build.logSha256":"CCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCCC"}},"outcome":{"error":{"name":"Error","message":"Native KV build did not complete successfully"}}},{"spec":{"name":"build log missing","provenanceDelete":["build.logSha256"]},"outcome":{"error":{"name":"Error","message":"Native KV build did not complete successfully"}}},{"spec":{"name":"sdk missing","inputsDelete":["sdk"]},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"sdk name","inputsSet":{"sdk.name":"@deno/kv2"}},"outcome":{"error":{"name":"Error","message":"Native KV source identity is unsupported"}}},{"spec":{"name":"sdk version","inputsSet":{"sdk.version":"0.13.0"}},"outcome":{"error":{"name":"Error","message":"Native KV source identity is unsupported"}}},{"spec":{"name":"source missing","inputsDelete":["source"]},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"source commit","inputsSet":{"source.commit":"0000000000000000000000000000000000000000"}},"outcome":{"error":{"name":"Error","message":"Native KV source identity is unsupported"}}},{"spec":{"name":"sdk name and missing source","inputsSet":{"sdk.name":"x"},"inputsDelete":["source"]},"outcome":{"error":{"name":"Error","message":"Native KV source identity is unsupported"}}},{"spec":{"name":"patches differ","provenanceSet":{"patches":[{"file":"fixture.patch","sha256":"eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"}]}},"outcome":{"error":{"name":"Error","message":"Native KV provenance patches differs from frozen inputs"}}},{"spec":{"name":"lock absent in both","inputsDelete":["lock"]},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"e27c4463f38795f49b1b997e198d939236f580709570ffd78dc4be259f11ecd5","inputsSha256":"2c7c018795e02d4a4f0f9d65a799f60a345ae7185b444786c27cb0a0d91565ef"}}},{"spec":{"name":"recipe order differs","inputsSet":{"recipe":{"locked":true,"steps":["a","b"]}},"provenanceSet":{"recipe":{"steps":["a","b"],"locked":true}}},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"0f9c63da0d22f490b27a3ed143c0ba068bd43b91759a762f2553bf911119dce3","inputsSha256":"4a90b01f03c6cba0f5a9b91fc76429b03b6186ba188324cf4e0e9846385e7887"}}},{"spec":{"name":"recipe extra key","provenanceSet":{"recipe.extra":null}},"outcome":{"error":{"name":"Error","message":"Native KV provenance recipe differs from frozen inputs"}}},{"spec":{"name":"toolchain absent in provenance","provenanceDelete":["toolchain"]},"outcome":{"error":{"name":"Error","message":"Native KV provenance toolchain differs from frozen inputs"}}},{"spec":{"name":"target differs","provenanceSet":{"artifact.target":"other-target"}},"outcome":{"error":{"name":"Error","message":"Native KV artifact target differs from frozen inputs"}}},{"spec":{"name":"target object in both","inputsSet":{"artifact.target":{"triple":"x"}}},"outcome":{"error":{"name":"Error","message":"Native KV artifact target differs from frozen inputs"}}},{"spec":{"name":"file both other","inputsSet":{"artifact.file":"other.node"}},"outcome":{"error":{"name":"Error","message":"Native KV artifact does not support this runtime platform or ABI"}}},{"spec":{"name":"platform both darwin","inputsSet":{"artifact.platform":"darwin"}},"outcome":{"error":{"name":"Error","message":"Native KV artifact does not support this runtime platform or ABI"}}},{"spec":{"name":"architecture both x64","inputsSet":{"artifact.architecture":"x64"}},"outcome":{"error":{"name":"Error","message":"Native KV artifact does not support this runtime platform or ABI"}}},{"spec":{"name":"napi version both 5","inputsSet":{"artifact.napiVersion":5}},"outcome":{"error":{"name":"Error","message":"Native KV artifact does not support this runtime platform or ABI"}}},{"spec":{"name":"napi version string both","inputsSet":{"artifact.napiVersion":"4"}},"outcome":{"error":{"name":"Error","message":"Native KV artifact does not support this runtime platform or ABI"}}},{"spec":{"name":"artifact missing in inputs","inputsDelete":["artifact"],"provenanceSet":{"artifact":{"file":"native-kv.node"}}},"outcome":{"error":{"name":"Error","message":"Invalid native KV provenance object"}}},{"spec":{"name":"artifact sha missing","provenanceDelete":["artifact.sha256"]},"outcome":{"error":{"name":"Error","message":"Native KV artifact hash is invalid"}}},{"spec":{"name":"artifact sha number","provenanceSet":{"artifact.sha256":5}},"outcome":{"error":{"name":"Error","message":"Native KV artifact hash is invalid"}}},{"spec":{"name":"artifact bytes changed","artifactBytes":[4]},"outcome":{"error":{"name":"Error","message":"Native KV artifact hash does not match provenance"}}},{"spec":{"name":"artifact symlink","fs":"artifact-symlink"},"outcome":{"error":{"name":"Error","message":"Native KV artifacts must be regular files, not symlinks"}}},{"spec":{"name":"artifact missing","fs":"artifact-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/artifact/native-kv.node'"}}},{"spec":{"name":"directory url","forms":"url-dir"},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsSha256":"faa6d6b42151e44b2339f4ee0f230076f95c47c789be9778affd359c31890532"}}},{"spec":{"name":"inputs url","forms":"url-inputs"},"outcome":{"value":{"path":"<root>/artifact/native-kv.node","artifactSha256":"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81","provenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsSha256":"faa6d6b42151e44b2339f4ee0f230076f95c47c789be9778affd359c31890532"}}}],"bootstrap":[{"spec":{"name":"valid"},"outcome":{"value":{"artifactDirectory":"<root>/.data/native-kv-fixture","expectedProvenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsManifest":"<root>/tools/native-kv/inputs.json"}}},{"spec":{"name":"receipt without inputs manifest","tweak":"receipt-no-inputs"},"outcome":{"value":{"artifactDirectory":"<root>/.data/native-kv-fixture","expectedProvenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsManifest":"<root>/tools/native-kv/inputs.json"}}},{"spec":{"name":"receipt extra key","tweak":"receipt-extra-key"},"outcome":{"error":{"name":"Error","message":"Native KV development binding requires an independently supplied provenance hash and absolute paths"}}},{"spec":{"name":"receipt proto key","tweak":"receipt-proto"},"outcome":{"error":{"name":"Error","message":"Native KV development binding requires an independently supplied provenance hash and absolute paths"}}},{"spec":{"name":"receipt relative directory","tweak":"receipt-relative-dir"},"outcome":{"error":{"name":"Error","message":"Native KV development binding requires an independently supplied provenance hash and absolute paths"}}},{"spec":{"name":"receipt inputs null","tweak":"receipt-inputs-null"},"outcome":{"error":{"name":"Error","message":"Native KV development binding requires an independently supplied provenance hash and absolute paths"}}},{"spec":{"name":"receipt inputs number","tweak":"receipt-inputs-number"},"outcome":{"error":{"name":"Error","message":"Native KV development binding requires an independently supplied provenance hash and absolute paths"}}},{"spec":{"name":"receipt inputs elsewhere","tweak":"receipt-inputs-other"},"outcome":{"error":{"name":"Error","message":"Native KV development binding must use the current tracked platform inputs"}}},{"spec":{"name":"receipt array","tweak":"receipt-array"},"outcome":{"error":{"name":"Error","message":"Invalid native KV development binding"}}},{"spec":{"name":"receipt null","tweak":"receipt-null"},"outcome":{"error":{"name":"Error","message":"Invalid native KV development binding"}}},{"spec":{"name":"receipt bom","tweak":"receipt-bom"},"outcome":{"error":{"name":"SyntaxError","message":"Invalid JSON"}}},{"spec":{"name":"receipt oversize","tweak":"receipt-oversize"},"outcome":{"error":{"name":"Error","message":"Native KV development binding changed or is oversized"}}},{"spec":{"name":"receipt hard link","tweak":"receipt-hardlink"},"outcome":{"error":{"name":"Error","message":"Native KV development binding must be a regular file, not a symlink"}}},{"spec":{"name":"receipt group readable","tweak":"receipt-0640"},"outcome":{"error":{"name":"Error","message":"Native KV development storage must be owner-private"}}},{"spec":{"name":"artifact outside data","tweak":"artifact-outside"},"outcome":{"error":{"name":"Error","message":"Native KV development artifact must stay beneath repository .data"}}},{"spec":{"name":"artifact is data","tweak":"artifact-is-data"},"outcome":{"error":{"name":"Error","message":"Native KV development artifact must stay beneath repository .data"}}},{"spec":{"name":"artifact traverses","tweak":"artifact-traverse"},"outcome":{"error":{"name":"Error","message":"Native KV development artifact must stay beneath repository .data"}}},{"spec":{"name":"artifact nested","tweak":"artifact-nested"},"outcome":{"value":{"artifactDirectory":"<root>/.data/nested/native-kv-fixture","expectedProvenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsManifest":"<root>/tools/native-kv/inputs.json"}}},{"spec":{"name":"nested directory 0750","tweak":"nested-0750"},"outcome":{"error":{"name":"Error","message":"Native KV development storage must be owner-private"}}},{"spec":{"name":"artifact file hard link","tweak":"artifact-hardlink"},"outcome":{"error":{"name":"Error","message":"Native KV development artifacts must not contain symlinks"}}},{"spec":{"name":"artifact directory 0710","tweak":"artifact-dir-0710"},"outcome":{"error":{"name":"Error","message":"Native KV development storage must be owner-private"}}},{"spec":{"name":"inputs 0644","tweak":"inputs-0644"},"outcome":{"value":{"artifactDirectory":"<root>/.data/native-kv-fixture","expectedProvenanceSha256":"782c05ced38d56c481ca991d075db7d96f4bcb57266de0b8bb17de3f4a09bdd9","inputsManifest":"<root>/tools/native-kv/inputs.json"}}},{"spec":{"name":"inputs 0620","tweak":"inputs-0620"},"outcome":{"error":{"name":"Error","message":"Native KV tracked inputs must be safe regular files without symlinks"}}},{"spec":{"name":"data missing","tweak":"data-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/.data'"}}},{"spec":{"name":"root symlink","tweak":"root-symlink"},"outcome":{"error":{"name":"Error","message":"Native KV repository root must be a real directory"}}},{"spec":{"name":"root missing","tweak":"root-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/absent'"}}},{"spec":{"name":"wrong hash","tweak":"wrong-hash"},"outcome":{"error":{"name":"Error","message":"Native KV provenance hash does not match its release binding"}}},{"spec":{"name":"provenance missing","tweak":"provenance-missing"},"outcome":{"error":{"name":"Error","message":"ENOENT: no such file or directory, lstat '<root>/.data/native-kv-fixture/native-kv.provenance.json'"}}}],"texts":{"inputs":"{\"schema\":1,\"sdk\":{\"name\":\"@deno/kv\",\"version\":\"0.14.0\"},\"source\":{\"commit\":\"9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5\"},\"patches\":[{\"file\":\"fixture.patch\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],\"lock\":{\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},\"toolchain\":{\"version\":\"fixture\"},\"recipe\":{\"locked\":true},\"artifact\":{\"file\":\"native-kv.node\",\"platform\":\"linux\",\"architecture\":\"arm64\",\"target\":\"fixture-target\",\"napiVersion\":4}}","provenance":"{\"schema\":1,\"sdk\":{\"name\":\"@deno/kv\",\"version\":\"0.14.0\"},\"source\":{\"commit\":\"9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5\"},\"patches\":[{\"file\":\"fixture.patch\",\"sha256\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\"}],\"lock\":{\"sha256\":\"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\"},\"toolchain\":{\"version\":\"fixture\"},\"recipe\":{\"locked\":true},\"artifact\":{\"file\":\"native-kv.node\",\"platform\":\"linux\",\"architecture\":\"arm64\",\"target\":\"fixture-target\",\"napiVersion\":4,\"sha256\":\"039058c6f2c0cb492c533b0a4d14ef77cc0f78abccced5287d84a1a2011cfb81\"},\"inputsSha256\":\"faa6d6b42151e44b2339f4ee0f230076f95c47c789be9778affd359c31890532\",\"build\":{\"exitCode\":0,\"logSha256\":\"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc\"}}"}})VECTORS";
#endif

  const json::object& vectors() {
    // Bun 1.4.0 on Linux x64: hashes bind process.arch and development bootstrap
    // rejects the unsupported host before inspecting the receipt. ARM vectors stay intact.
    const char* const LINUX_X64_VECTORS =
#include "tests/native_addon_linux_x64_vectors.inc"
      ;
    static const json::object parsed = rt::jsonParse(rt::process::platform == "linux" && rt::process::arch == "x64" ? LINUX_X64_VECTORS : VECTORS).as_object();
    return parsed;
  }

  std::string sha(std::string_view text) { return rt::crypto::createHash("sha256").update(text).digest("hex"); }
  std::string sha(const std::vector<std::uint8_t>& bytes) { return rt::crypto::createHash("sha256").update(bytes).digest("hex"); }

  // node:fs symlinkSync / renameSync for the fixtures (rt::fs has neither).
  void symlinkSync(const std::string& target, const std::string& path) {
    if (::symlink(target.c_str(), path.c_str()) != 0) throw std::runtime_error("symlink failed: " + path);
  }
  void renameSync(const std::string& from, const std::string& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) throw std::runtime_error("rename failed: " + from);
  }

  json::object baseInputs() {
    return rt::jsonParse(std::string(R"({"schema":1,"sdk":{"name":"@deno/kv","version":"0.14.0"},"source":{"commit":"9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5"},)") +
                         R"("patches":[{"file":"fixture.patch","sha256":")" + std::string(64, 'a') + R"("}],"lock":{"sha256":")" + std::string(64, 'b') +
                         R"("},"toolchain":{"version":"fixture"},"recipe":{"locked":true},"artifact":{"file":"native-kv.node","platform":")" +
                         std::string(rt::process::platform) + R"(","architecture":")" + std::string(rt::process::arch) +
                         R"(","target":"fixture-target","napiVersion":4}})")
      .as_object();
  }

  json::object* parentOf(json::object& root, const std::vector<std::string>& parts) {
    json::object* node = &root;
    for (std::size_t index = 0; index + 1 < parts.size(); ++index) node = &rt::get(*node, parts[index])->as_object();
    return node;
  }
  void setPath(json::object& root, const std::string& path, const json::value& value) {
    const std::vector<std::string> parts = rt::split(path, ".");
    rt::objectSet(*parentOf(root, parts), parts.back(), value);
  }
  void deletePath(json::object& root, const std::string& path) {
    const std::vector<std::string> parts = rt::split(path, ".");
    rt::objectDelete(*parentOf(root, parts), parts.back());
  }

  struct Texts {
    std::string inputsText;
    std::string provenanceText;
    std::vector<std::uint8_t> bytes;
  };

  // `buildTexts(spec)` of the generator.
  Texts buildTexts(const json::object& spec) {
    const std::vector<std::uint8_t> defaultBytes = {1, 2, 3};
    json::object inputs = baseInputs();
    if (const json::object* set = rt::getObject(spec, "inputsSet")) {
      for (const auto& member : *set) setPath(inputs, std::string(member.key()), member.value());
    }
    if (const json::array* removed = rt::getArray(spec, "inputsDelete")) {
      for (const json::value& path : *removed) deletePath(inputs, std::string(path.as_string()));
    }
    Texts texts;
    const auto inputsText = rt::getString(spec, "inputsText");
    texts.inputsText = inputsText ? std::string(*inputsText) : rt::jsonStringify(json::value(inputs));
    const json::object* artifactInputs = rt::getObject(inputs, "artifact");
    json::object artifact = artifactInputs ? rt::objectSpread(*artifactInputs) : json::object();
    rt::objectDefine(artifact, "sha256", json::value(sha(defaultBytes)));
    json::object build;
    rt::objectDefine(build, "exitCode", rt::makeNumber(0.0));
    rt::objectDefine(build, "logSha256", json::value(std::string(64, 'c')));
    json::object provenance = rt::objectSpread(inputs);
    rt::objectDefine(provenance, "inputsSha256", json::value(sha(texts.inputsText)));
    rt::objectDefine(provenance, "artifact", json::value(std::move(artifact)));
    rt::objectDefine(provenance, "build", json::value(std::move(build)));
    if (const json::object* set = rt::getObject(spec, "provenanceSet")) {
      for (const auto& member : *set) setPath(provenance, std::string(member.key()), member.value());
    }
    if (const json::array* removed = rt::getArray(spec, "provenanceDelete")) {
      for (const json::value& path : *removed) deletePath(provenance, std::string(path.as_string()));
    }
    const auto provenanceText = rt::getString(spec, "provenanceText");
    texts.provenanceText = provenanceText ? std::string(*provenanceText) : rt::jsonStringify(json::value(provenance));
    if (const json::array* replace = rt::getArray(spec, "provenanceReplace")) {
      texts.provenanceText = rt::replace(texts.provenanceText, std::string(replace->at(0).as_string()), std::string(replace->at(1).as_string()));
    }
    texts.bytes = defaultBytes;
    if (const json::array* bytes = rt::getArray(spec, "artifactBytes")) {
      texts.bytes.clear();
      for (const json::value& byte : *bytes) texts.bytes.push_back(static_cast<std::uint8_t>(*rt::getNumber(byte)));
    }
    return texts;
  }

  // What the generator's `outcome(root, fn)` records.
  json::value normalized(const json::value& value, const std::string& root) { return rt::jsonParse(rt::replaceAll(rt::jsonStringify(value), root, "<root>")); }
  json::value errorOutcome(const rt::JsError& error, const std::string& root) {
    json::object inner;
    inner["name"] = error.name;
    inner["message"] = rt::replaceAll(error.message, root, "<root>");
    json::object out;
    out["error"] = std::move(inner);
    return out;
  }
  json::value valueOutcome(json::value value, const std::string& root) {
    json::object out;
    out["value"] = normalized(value, root);
    return out;
  }

  void expectOutcome(const json::value& actual, const json::value& expected, const std::string& label) {
    if (!rt::deepEqual(actual, expected))
      ::uos::rt::test::report(__FILE__, __LINE__, label + "\n      actual:   " + rt::jsonStringify(actual) + "\n      expected: " + rt::jsonStringify(expected), false);
  }

  json::value bindingJson(const native_addon::NativeKvArtifactBinding& binding) {
    const auto pathOf = [](const std::variant<std::string, rt::URL>& value) { return std::holds_alternative<rt::URL>(value) ? std::get<rt::URL>(value).href() : std::get<std::string>(value); };
    json::object out;
    out["artifactDirectory"] = pathOf(binding.artifactDirectory);
    out["expectedProvenanceSha256"] = binding.expectedProvenanceSha256;
    if (binding.inputsManifest) out["inputsManifest"] = pathOf(*binding.inputsManifest);
    return out;
  }

  struct TempBase {
    std::string path = rt::fs::realpathSync(rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-native-kv %?#-")));
    ~TempBase() {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      rt::fs::rmSync(path, options);
    }
  };

  void writeFile(const std::string& path, std::string_view text, std::uint32_t mode = 0666) {
    rt::fs::WriteFileOptions options;
    options.mode = mode;
    rt::fs::writeFileSync(path, text, options);
  }
  void writeFile(const std::string& path, const std::vector<std::uint8_t>& bytes, std::uint32_t mode = 0666) {
    rt::fs::WriteFileOptions options;
    options.mode = mode;
    rt::fs::writeFileSync(path, std::span<const std::uint8_t>(bytes), options);
  }
  void makeDirectory(const std::string& path, std::uint32_t mode = 0777, bool recursive = false) {
    rt::fs::MkdirOptions options;
    options.mode = mode;
    options.recursive = recursive;
    rt::fs::mkdirSync(path, options);
  }

  native_addon::NapiInterface fullInterface(std::shared_ptr<int> opened) {
    native_addon::NapiInterface napi;
    napi.open = [opened](std::string, std::optional<bool>, bool) {
      *opened += 1;
      return 7.0;
    };
    napi.close = [](double, bool) {};
    napi.snapshotRead = [](double, std::vector<std::uint8_t> bytes, bool) { return rt::Promise<std::vector<std::uint8_t>>::resolve(bytes); };
    napi.atomicWrite = [](double, std::vector<std::uint8_t> bytes, bool) { return rt::Promise<std::vector<std::uint8_t>>::resolve(bytes); };
    napi.dequeueNextMessage = [](double, bool) { return rt::Promise<std::optional<native_addon::NapiDequeuedMessage>>::resolve(std::nullopt); };
    napi.finishMessage = [](double, double, bool, bool) { return rt::Promise<void>::resolve(); };
    napi.startWatch = [](double, std::vector<std::uint8_t>, bool) { return rt::Promise<double>::resolve(1.0); };
    napi.dequeueNextWatchMessage = [](double, double, bool) { return rt::Promise<std::optional<std::vector<std::uint8_t>>>::resolve(std::nullopt); };
    napi.endWatch = [](double, double, bool) {};
    return napi;
  }

  std::string errorMessage(const std::function<void()>& run) {
    try {
      run();
    } catch (const rt::JsError& error) {
      return error.message;
    }
    return "<no error>";
  }
}

TEST("the fixture texts are the bytes the generator wrote") {
  const Texts texts = buildTexts(json::object());
  const json::object& expected = *rt::getObject(vectors(), "texts");
  EXPECT_EQ(texts.inputsText, std::string(*rt::getString(expected, "inputs")));
  EXPECT_EQ(texts.provenanceText, std::string(*rt::getString(expected, "provenance")));
}

TEST("verifyNativeKvArtifact matches the recorded outcomes") {
  TempBase base;
  std::size_t checked = 0;
  for (const json::value& entry : *rt::getArray(vectors(), "verify")) {
    const json::object& spec = *rt::getObject(entry, "spec");
    const std::string name(*rt::getString(spec, "name"));
    const std::string root = rt::fs::mkdtempSync(rt::path::join(base.path, "verify-"));
    const std::string directory = rt::path::join(root, "artifact");
    makeDirectory(directory);
    const Texts texts = buildTexts(spec);
    writeFile(rt::path::join(directory, "inputs.json"), texts.inputsText);
    writeFile(rt::path::join(directory, "native-kv.provenance.json"), texts.provenanceText);
    writeFile(rt::path::join(directory, "native-kv.node"), texts.bytes);
    std::variant<std::string, rt::URL> artifactDirectory = directory;
    std::variant<std::string, rt::URL> inputsManifest = rt::path::join(directory, "inputs.json");
    const std::string fs = rt::getString(spec, "fs") ? std::string(*rt::getString(spec, "fs")) : std::string();
    const std::string forms = rt::getString(spec, "forms") ? std::string(*rt::getString(spec, "forms")) : std::string();
    if (fs == "missing-dir") artifactDirectory = rt::path::join(root, "absent");
    if (fs == "dir-symlink") {
      renameSync(directory, directory + ".saved");
      symlinkSync(directory + ".saved", directory);
      inputsManifest = rt::path::join(directory + ".saved", "inputs.json");
    }
    if (fs == "provenance-symlink") {
      renameSync(rt::path::join(directory, "native-kv.provenance.json"), rt::path::join(directory, "provenance.saved"));
      symlinkSync(rt::path::join(directory, "provenance.saved"), rt::path::join(directory, "native-kv.provenance.json"));
    }
    if (fs == "provenance-missing") rt::fs::rmSync(rt::path::join(directory, "native-kv.provenance.json"));
    if (fs == "inputs-missing") inputsManifest = rt::path::join(directory, "absent.json");
    if (fs == "artifact-symlink") {
      renameSync(rt::path::join(directory, "native-kv.node"), rt::path::join(directory, "node.saved"));
      symlinkSync(rt::path::join(directory, "node.saved"), rt::path::join(directory, "native-kv.node"));
    }
    if (fs == "artifact-missing") rt::fs::rmSync(rt::path::join(directory, "native-kv.node"));
    if (forms == "url-dir") artifactDirectory = rt::pathToFileURL(directory);
    if (forms == "url-inputs") inputsManifest = rt::pathToFileURL(rt::path::join(directory, "inputs.json"));
    native_addon::NativeKvArtifactBinding binding;
    binding.artifactDirectory = artifactDirectory;
    const auto expectedHash = rt::getString(spec, "expectedHash");
    binding.expectedProvenanceSha256 = expectedHash ? std::string(*expectedHash) : sha(texts.provenanceText);
    binding.inputsManifest = inputsManifest;
    json::value actual;
    try {
      const native_addon::VerifiedNativeKvArtifact verified = native_addon::verifyNativeKvArtifact(binding);
      json::object value;
      value["path"] = verified.path;
      value["artifactSha256"] = verified.artifactSha256;
      value["provenanceSha256"] = verified.provenanceSha256;
      value["inputsSha256"] = verified.inputsSha256;
      actual = valueOutcome(value, root);
    } catch (const rt::JsError& error) {
      actual = errorOutcome(error, root);
    }
    expectOutcome(actual, *rt::get(entry, "outcome"), "verify " + name);
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t(50));
}

TEST("readNativeKvDevelopmentBinding matches the recorded outcomes") {
  TempBase base;
  const Texts valid = buildTexts(json::object());
  std::size_t checked = 0;
  for (const json::value& entry : *rt::getArray(vectors(), "bootstrap")) {
    const json::object& spec = *rt::getObject(entry, "spec");
    const std::string name(*rt::getString(spec, "name"));
    const std::string tweak = rt::getString(spec, "tweak") ? std::string(*rt::getString(spec, "tweak")) : std::string();
    const std::string outer = rt::fs::mkdtempSync(rt::path::join(base.path, "development-"));
    std::string root = outer;
    // Build the fixture independently so unsupported hosts still execute all 28
    // read attempts and assert the source's platform rejection.
    const std::string data = rt::path::join(root, ".data");
    const bootstrap::NativeKvDevelopmentPaths paths{
      root, data, rt::path::join(data, "native-kv.binding.json"),
      rt::path::join(root, "cpp/gateway/packaging/native-kv", rt::process::platform == "darwin" ? "inputs-darwin-arm64.json" : "inputs.json")};
    if (tweak != "data-missing") makeDirectory(paths.data, 0700);
    std::string artifactParent = paths.data;
    if (tweak == "artifact-nested" || tweak == "nested-0750") {
      artifactParent = rt::path::join(paths.data, "nested");
      makeDirectory(artifactParent, 0700);
      if (tweak == "nested-0750") rt::fs::chmodSync(artifactParent, 0750);
    }
    const std::string artifact = rt::path::join(artifactParent, "native-kv-fixture");
    if (tweak != "data-missing") {
      makeDirectory(artifact, 0700);
      rt::fs::chmodSync(artifact, tweak == "artifact-dir-0710" ? 0710 : 0700);
      writeFile(rt::path::join(artifact, "native-kv.node"), valid.bytes, 0600);
      if (tweak != "provenance-missing") writeFile(rt::path::join(artifact, "native-kv.provenance.json"), valid.provenanceText, 0600);
      if (tweak == "artifact-hardlink") rt::fs::linkSync(rt::path::join(artifact, "native-kv.node"), rt::path::join(artifact, "node.link"));
    }
    makeDirectory(rt::path::dirname(paths.inputs), 0777, true);
    writeFile(paths.inputs, valid.inputsText, 0600);
    rt::fs::chmodSync(paths.inputs, tweak == "inputs-0644" ? 0644 : tweak == "inputs-0620" ? 0620 : 0600);
    std::string receiptDirectory = artifact;
    if (tweak == "artifact-outside") receiptDirectory = root;
    if (tweak == "artifact-is-data") receiptDirectory = paths.data;
    if (tweak == "artifact-traverse") receiptDirectory = rt::path::join(paths.data, "..", "cpp");
    json::object document;
    rt::objectDefine(document, "artifactDirectory", json::value(receiptDirectory));
    rt::objectDefine(document, "expectedProvenanceSha256", json::value(tweak == "wrong-hash" ? std::string(64, '0') : sha(valid.provenanceText)));
    rt::objectDefine(document, "inputsManifest", json::value(paths.inputs));
    if (tweak == "receipt-no-inputs") rt::objectDelete(document, "inputsManifest");
    if (tweak == "receipt-extra-key") rt::objectSet(document, "extra", rt::makeNumber(1.0));
    if (tweak == "receipt-relative-dir") rt::objectSet(document, "artifactDirectory", json::value("native-kv-fixture"));
    if (tweak == "receipt-inputs-null") rt::objectSet(document, "inputsManifest", json::value(nullptr));
    if (tweak == "receipt-inputs-number") rt::objectSet(document, "inputsManifest", rt::makeNumber(7.0));
    if (tweak == "receipt-inputs-other") rt::objectSet(document, "inputsManifest", json::value("/elsewhere/inputs.json"));
    std::string receiptText = rt::jsonStringify(json::value(document));
    if (tweak == "receipt-proto") receiptText = rt::slice(receiptText, 0, -1) + ",\"__proto__\":{}}";
    if (tweak == "receipt-array") receiptText = "[]";
    if (tweak == "receipt-null") receiptText = "null";
    if (tweak == "receipt-bom") receiptText = "\xEF\xBB\xBF" + receiptText;
    if (tweak == "receipt-oversize") receiptText = receiptText + std::string(17000, ' ');
    if (tweak != "data-missing") writeFile(paths.receipt, receiptText, 0600);
    if (tweak == "receipt-hardlink") rt::fs::linkSync(paths.receipt, paths.receipt + ".link");
    if (tweak == "receipt-0640") rt::fs::chmodSync(paths.receipt, 0640);
    if (tweak == "root-symlink") {
      symlinkSync(outer, outer + ".link");
      root = outer + ".link";
    }
    if (tweak == "root-missing") root = rt::path::join(outer, "absent");
    json::value actual;
    try {
      actual = valueOutcome(bindingJson(bootstrap::readNativeKvDevelopmentBinding(root)), outer);
    } catch (const rt::JsError& error) {
      actual = errorOutcome(error, outer);
    }
    json::value expected = *rt::get(entry, "outcome");
    if (json::value* value = expected.as_object().if_contains("value")) {
      auto& input = value->as_object().at("inputsManifest");
      const std::string_view recorded(input.as_string().data(), input.as_string().size());
      constexpr std::string_view oldPrefix = "<root>/tools/native-kv/";
      EXPECT(recorded.starts_with(oldPrefix));
      input = json::value(std::string("<root>/cpp/gateway/packaging/native-kv/") + std::string(recorded.substr(oldPrefix.size())));
    }
    expectOutcome(actual, expected, "bootstrap " + name);
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t(28));
}

ASYNC_TEST("prepare lifecycle: nothing is prepared, the default host is the linked store, an injected backend is prepared once") {
  EXPECT(!native_addon::nativeKvBackendIsPrepared());
  EXPECT_EQ(errorMessage([] { (void)native_addon::preparedNativeKvArtifactBinding(); }), std::string("Prepare the verified native KV artifact before starting a child"));
  EXPECT_EQ(errorMessage([] { (void)native_addon::preparedNativeKvBackend(); }), std::string("Prepare the verified native KV artifact before opening a database"));

  TempBase base;
  const Texts valid = buildTexts(json::object());
  const auto artifactAt = [&](const std::string& name) {
    const std::string directory = rt::path::join(base.path, name);
    makeDirectory(directory);
    writeFile(rt::path::join(directory, "inputs.json"), valid.inputsText);
    writeFile(rt::path::join(directory, "native-kv.provenance.json"), valid.provenanceText);
    writeFile(rt::path::join(directory, "native-kv.node"), valid.bytes);
    native_addon::NativeKvArtifactBinding binding;
    binding.artifactDirectory = directory;
    binding.expectedProvenanceSha256 = sha(valid.provenanceText);
    binding.inputsManifest = rt::path::join(directory, "inputs.json");
    return binding;
  };
  const native_addon::NativeKvArtifactBinding first = artifactAt("first");
  const native_addon::NativeKvArtifactBinding second = artifactAt("second");

  // Verification runs first: a bad binding never reaches the loader.
  auto loads = std::make_shared<std::vector<std::string>>();
  const native_addon::NativeKvRequire original = native_addon::nativeKvRequire();
  native_addon::NativeKvArtifactBinding bad = first;
  bad.expectedProvenanceSha256 = std::string(64, '0');
  native_addon::nativeKvRequire() = [loads](const std::string& path) -> std::optional<native_addon::NapiInterface> {
    loads->push_back(path);
    return std::nullopt;
  };
  EXPECT_EQ(errorMessage([&] { native_addon::prepareNativeKvBackend(bad); }), std::string("Native KV provenance hash does not match its release binding"));
  EXPECT(loads->empty());
  // Exports that are not an object.
  EXPECT_EQ(errorMessage([&] { native_addon::prepareNativeKvBackend(first); }), std::string("Invalid native KV provenance object"));
  EXPECT_EQ(*loads, std::vector<std::string>{rt::path::join(base.path, "first", "native-kv.node")});
  // The default loader: the linked store's interface (rt::SqliteKv). Every pinned export is
  // present; the C++ runtime opens the store directly and never calls them.
  const std::optional<native_addon::NapiInterface> linked = original(rt::path::join(base.path, "first", "native-kv.node"));
  ASSERT(linked.has_value());
  EXPECT(linked->open && linked->close && linked->snapshotRead && linked->atomicWrite && linked->dequeueNextMessage && linked->finishMessage && linked->startWatch &&
         linked->dequeueNextWatchMessage && linked->endWatch);
  EXPECT_EQ(errorMessage([&] { (void)linked->open("x", std::nullopt, false); }),
            std::string("Native KV export 'open' is not called in the C++ runtime: storage::kv::openNativeKv opens the linked rt::SqliteKv store directly"));
  // One export missing.
  auto opened = std::make_shared<int>(0);
  native_addon::nativeKvRequire() = [opened](const std::string&) -> std::optional<native_addon::NapiInterface> {
    native_addon::NapiInterface napi = fullInterface(opened);
    napi.endWatch = nullptr;
    return napi;
  };
  EXPECT_EQ(errorMessage([&] { native_addon::prepareNativeKvBackend(first); }), std::string("Native KV artifact exports do not satisfy its pinned NAPI interface"));
  EXPECT(!native_addon::nativeKvBackendIsPrepared());

  // development startup before preparation rejects asynchronously with the receipt error
  rt::Promise<void> development = bootstrap::prepareNativeKvForDevelopment(rt::path::join(base.path, "no-root"));
  EXPECT(development.pending());
  bool developmentRejected = false;
  try {
    co_await development.wait();
  } catch (const rt::JsError& error) {
    developmentRejected = rt::includes(error.message, "ENOENT");
  }
  EXPECT(developmentRejected);

  // All nine exports: prepared once, for this artifact only.
  native_addon::nativeKvRequire() = [opened](const std::string&) -> std::optional<native_addon::NapiInterface> { return fullInterface(opened); };
  native_addon::prepareNativeKvBackend(first);
  EXPECT(native_addon::nativeKvBackendIsPrepared());
  const native_addon::NativeKvArtifactBinding prepared = native_addon::preparedNativeKvArtifactBinding();
  EXPECT_EQ(std::get<std::string>(prepared.artifactDirectory), rt::path::join(base.path, "first"));
  EXPECT_EQ(prepared.expectedProvenanceSha256, sha(valid.provenanceText));
  EXPECT_EQ(std::get<std::string>(*prepared.inputsManifest), rt::path::join(base.path, "first", "inputs.json"));
  const native_addon::NapiInterface napi = native_addon::preparedNativeKvBackend();
  EXPECT_EQ(napi.open("x", std::nullopt, false), 7.0);
  EXPECT_EQ(*opened, 1);
  native_addon::prepareNativeKvBackend(first);  // the same artifact again: a no-op
  EXPECT_EQ(errorMessage([&] { native_addon::prepareNativeKvBackend(second); }), std::string("Native KV backend was already prepared for another artifact"));
  // A prepared backend makes development startup skip the receipt entirely.
  rt::Promise<void> skipped = bootstrap::prepareNativeKvForDevelopment(rt::path::join(base.path, "no-root"));
  co_await skipped.wait();
  native_addon::nativeKvRequire() = original;
}
