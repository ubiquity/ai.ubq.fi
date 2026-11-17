#include "tests/helpers/linked_native_kv.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/storage/native_addon.hpp"

#include <string_view>

namespace uos::tests::helpers::linked_native_kv {
  namespace {
    namespace rt = uos::rt;
    namespace json = uos::json;
    namespace native_addon = uos::storage::native_addon;

    std::string sha256Hex(std::string_view text) { return rt::crypto::createHash("sha256").update(text).digest("hex"); }

    // The artifact directory, removed when the process exits.
    struct ArtifactDirectory {
      std::string path = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-linked-native-kv-"));
      ArtifactDirectory() = default;
      ArtifactDirectory(const ArtifactDirectory&) = delete;
      ArtifactDirectory& operator=(const ArtifactDirectory&) = delete;
      ~ArtifactDirectory() {
        rt::fs::RmOptions options;
        options.recursive = true;
        options.force = true;
        rt::fs::rmSync(path, options);
      }
    };
  }

  const std::string& bindingJson() {
    static const ArtifactDirectory directory;
    static const std::string binding = [] {
      const std::string& root = directory.path;
      const std::string artifact = "linked";
      json::object artifactFields;
      artifactFields["file"] = "native-kv.node";
      artifactFields["platform"] = std::string(rt::process::platform);
      artifactFields["architecture"] = std::string(rt::process::arch);
      artifactFields["target"] = "fixture-target";
      artifactFields["napiVersion"] = 4;
      json::object inputs =
        rt::jsonParse(R"({"schema":1,"sdk":{"name":"@deno/kv","version":"0.14.0"},"source":{"commit":"9fe65cc138ff56ca5bfe7a1bf8c53e832246f9d5"},)"
                      R"("patches":[],"lock":{"sha256":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},"toolchain":{"version":"fixture"},"recipe":{"locked":true}})")
          .as_object();
      inputs["artifact"] = artifactFields;
      const std::string inputsText = rt::jsonStringify(json::value(inputs));
      json::object provenance = rt::objectSpread(inputs);
      rt::objectDefine(provenance, "inputsSha256", json::value(sha256Hex(inputsText)));
      json::object provenanceArtifact = rt::objectSpread(artifactFields);
      rt::objectDefine(provenanceArtifact, "sha256", json::value(sha256Hex(artifact)));
      rt::objectDefine(provenance, "artifact", json::value(provenanceArtifact));
      rt::objectDefine(provenance, "build", rt::jsonParse(R"({"exitCode":0,"logSha256":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"})"));
      const std::string provenanceText = rt::jsonStringify(json::value(provenance));
      rt::fs::writeFileSync(root + "/inputs.json", inputsText);
      rt::fs::writeFileSync(root + "/native-kv.provenance.json", provenanceText);
      rt::fs::writeFileSync(root + "/native-kv.node", artifact);
      json::object fields;
      fields["artifactDirectory"] = root;
      fields["expectedProvenanceSha256"] = sha256Hex(provenanceText);
      fields["inputsManifest"] = root + "/inputs.json";
      return rt::jsonStringify(json::value(fields));
    }();
    return binding;
  }

  void prepare(const std::string& bindingText) {
    const json::value binding = rt::jsonParse(bindingText);
    native_addon::NativeKvArtifactBinding prepared;
    prepared.artifactDirectory = std::string(binding.at("artifactDirectory").as_string());
    prepared.expectedProvenanceSha256 = std::string(binding.at("expectedProvenanceSha256").as_string());
    prepared.inputsManifest = std::string(binding.at("inputsManifest").as_string());
    native_addon::prepareNativeKvBackend(prepared);
  }

  void prepareOnce() {
    if (!native_addon::nativeKvBackendIsPrepared()) prepare(bindingJson());
  }
}
