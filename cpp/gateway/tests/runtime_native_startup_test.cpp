// Port of tests/runtime-native-startup.test.ts.
//
// The TS starts a fresh Bun process per case that calls prepareNativeKvForDevelopment(root)
// and then opens the native KV. Here each refusal runs in-process on a fresh fixture
// (no native backend is prepared before the refusals; the last case prepares one), with
// globalThis.fetch replaced the way the child replaces it. The fixture artifact is
// synthetic: the TS copies the artifact the test preload prepared, while these cases
// only need a verification chain that holds (real tools/native-kv/inputs.json,
// generated provenance and artifact bytes).
//
// Not ported: opening, writing and reading the native KV after a successful startup
// (src/storage/kv.ts and the native backend are not ported), and the two
// "explicit preparation" cases (scripts/prepare-native-runtime.ts is outside the port).
#include "uos/runtime/native_kv_bootstrap.hpp"
#include "uos/storage/native_addon.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <unistd.h>

#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace uos;
using runtime::native_kv_bootstrap::nativeKvDevelopmentPaths;
using runtime::native_kv_bootstrap::prepareNativeKvForDevelopment;

namespace {
  // `fileURLToPath(new URL("../", import.meta.url))` of the test: the repository root.
  const std::string& workspace() {
    static const std::string root = rt::path::resolve(rt::path::dirname(__FILE__), "../../..");
    return root;
  }

  std::string sha256(std::span<const std::uint8_t> bytes) { return rt::crypto::createHash("sha256").update(bytes).digest("hex"); }

  // node:fs symlinkSync / renameSync / appendFileSync for the fixture changes.
  void symlinkSync(const std::string& target, const std::string& path) {
    if (::symlink(target.c_str(), path.c_str()) != 0) throw std::runtime_error("symlink failed: " + path);
  }
  void renameSync(const std::string& from, const std::string& to) {
    if (std::rename(from.c_str(), to.c_str()) != 0) throw std::runtime_error("rename failed: " + from);
  }
  void appendFileSync(const std::string& path, std::string_view data) {
    rt::fs::WriteFileOptions options;
    options.flag = "a";
    rt::fs::writeFileSync(path, data, options);
  }

  struct Fixtures {
    std::string root = rt::fs::realpathSync(rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-m21-native-startup-")));
    ~Fixtures() {
      rt::fs::RmOptions options;
      options.recursive = true;
      options.force = true;
      rt::fs::rmSync(root, options);
    }
  };

  // What `preparedNativeKvArtifactBinding()` supplies in the TS test: a verified
  // artifact directory and the tracked inputs manifest it was built from.
  struct SourceArtifact {
    std::string artifactDirectory;
    std::string inputsManifest;
    std::string expectedProvenanceSha256;
  };

  SourceArtifact sourceArtifact(const std::string& base) {
    SourceArtifact source;
    source.inputsManifest = rt::path::join(workspace(), "tools", "native-kv", "inputs.json");
    source.artifactDirectory = rt::path::join(base, "source");
    rt::fs::mkdirSync(source.artifactDirectory);
    const rt::fs::Bytes inputsBytes = rt::fs::readFileSync(source.inputsManifest);
    const json::object inputs = rt::jsonParse(rt::bufferToString(inputsBytes)).as_object();
    const std::string node = "synthetic native-kv.node fixture";
    const std::vector<std::uint8_t> nodeBytes(node.begin(), node.end());
    json::object artifact = rt::objectSpread(*rt::getObject(inputs, "artifact"));
    rt::objectDefine(artifact, "sha256", json::value(sha256(nodeBytes)));
    json::object build;
    rt::objectDefine(build, "exitCode", rt::makeNumber(0.0));
    rt::objectDefine(build, "logSha256", json::value(std::string(64, 'c')));
    json::object provenance = rt::objectSpread(inputs);
    rt::objectDefine(provenance, "inputsSha256", json::value(sha256(inputsBytes)));
    rt::objectDefine(provenance, "artifact", json::value(std::move(artifact)));
    rt::objectDefine(provenance, "build", json::value(std::move(build)));
    const std::string provenanceText = rt::jsonStringify(json::value(provenance));
    rt::fs::writeFileSync(rt::path::join(source.artifactDirectory, "native-kv.node"), std::span<const std::uint8_t>(nodeBytes));
    rt::fs::writeFileSync(rt::path::join(source.artifactDirectory, "native-kv.provenance.json"), provenanceText);
    source.expectedProvenanceSha256 = rt::crypto::createHash("sha256").update(provenanceText).digest("hex");
    return source;
  }

  struct Fixture {
    std::string root;
    std::string data;
    std::string receipt;
    std::string inputs;
    std::string artifact;
  };

  Fixture fixture(const Fixtures& fixtures) {
    const SourceArtifact source = sourceArtifact(rt::fs::mkdtempSync(rt::path::join(fixtures.root, "source-")));
    const std::string root = rt::fs::mkdtempSync(rt::path::join(fixtures.root, "development-"));
    const runtime::native_kv_bootstrap::NativeKvDevelopmentPaths paths = nativeKvDevelopmentPaths(root);
    rt::fs::MkdirOptions dataOptions;
    dataOptions.mode = 0700;
    rt::fs::mkdirSync(paths.data, dataOptions);
    const std::string artifact = rt::fs::mkdtempSync(rt::path::join(paths.data, "native-kv-"));
    rt::fs::chmodSync(artifact, 0700);
    for (const std::string name : {"native-kv.node", "native-kv.provenance.json"}) {
      rt::fs::copyFileSync(rt::path::join(source.artifactDirectory, name), rt::path::join(artifact, name));
      rt::fs::chmodSync(rt::path::join(artifact, name), 0600);
    }
    rt::fs::MkdirOptions toolsOptions;
    toolsOptions.recursive = true;
    rt::fs::mkdirSync(rt::path::join(root, "tools", "native-kv"), toolsOptions);
    rt::fs::copyFileSync(source.inputsManifest, paths.inputs);
    json::object receipt;
    rt::objectDefine(receipt, "artifactDirectory", json::value(artifact));
    rt::objectDefine(receipt, "expectedProvenanceSha256", json::value(source.expectedProvenanceSha256));
    rt::objectDefine(receipt, "inputsManifest", json::value(paths.inputs));
    rt::fs::WriteFileOptions receiptOptions;
    receiptOptions.mode = 0600;
    rt::fs::writeFileSync(paths.receipt, rt::jsonStringify(json::value(receipt)), receiptOptions);
    return Fixture{paths.root, paths.data, paths.receipt, paths.inputs, artifact};
  }

  std::vector<std::string> sortedListing(const std::string& directory) {
    std::vector<std::string> names = rt::fs::readdirSync(directory);
    rt::sort(names, [](const std::string& left, const std::string& right) { return rt::localeCompare(left, right); });
    return names;
  }

  struct Startup {
    int code = 0;
    std::string stdout_;
    std::string stderr_;
  };

  // The child's script up to `openKv`: fetch forbidden, the default root checked, then
  // preparation; a failure is reported as `console.error(error.message)` with exit 78.
  rt::Task<Startup> startup(std::string root) {
    rt::Fetch forbidden([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("Unexpected startup download"); });
    rt::GlobalFetchOverride fetchScope(forbidden);
    Startup result;
    try {
      if (nativeKvDevelopmentPaths().root != rt::path::resolve(workspace())) throw rt::JsError("Expected values to be strictly equal");
      const rt::Promise<void> prepared = prepareNativeKvForDevelopment(root);
      co_await prepared.wait();
      result.stdout_ = "prepared";
    } catch (const rt::JsError& error) {
      result.stderr_ = error.message;
      result.code = 78;
    }
    co_return result;
  }

  struct Refusal {
    std::string name;
    std::function<void(const Fixture&)> change;
    std::string error;
  };

  const std::vector<Refusal>& refusals() {
    static const std::vector<Refusal> list = {
      {"absent receipt", [](const Fixture& paths) { rt::fs::rmSync(paths.receipt); }, "ENOENT"},
      {"corrupt receipt", [](const Fixture& paths) { rt::fs::writeFileSync(paths.receipt, "{broken"); }, "JSON"},
      {"absent independent hash",
       [](const Fixture& paths) {
         json::object document;
         document["artifactDirectory"] = paths.artifact;
         rt::fs::writeFileSync(paths.receipt, rt::jsonStringify(json::value(document)));
       },
       "independently supplied"},
      {"wrong independent hash",
       [](const Fixture& paths) {
         json::object binding = rt::jsonParse(rt::fs::readFileSync(paths.receipt, "utf8")).as_object();
         rt::objectSet(binding, "expectedProvenanceSha256", json::value(std::string(64, '0')));
         rt::fs::writeFileSync(paths.receipt, rt::jsonStringify(json::value(binding)));
       },
       "release binding"},
      {"tampered provenance", [](const Fixture& paths) { appendFileSync(rt::path::join(paths.artifact, "native-kv.provenance.json"), " "); }, "release binding"},
      {"tampered native bytes", [](const Fixture& paths) { appendFileSync(rt::path::join(paths.artifact, "native-kv.node"), "tampered"); }, "artifact hash"},
      {"tampered tracked inputs", [](const Fixture& paths) { appendFileSync(paths.inputs, " "); }, "frozen inputs"},
      {"symlink receipt",
       [](const Fixture& paths) {
         renameSync(paths.receipt, paths.receipt + ".saved");
         symlinkSync(paths.receipt + ".saved", paths.receipt);
       },
       "regular file"},
      {"symlink artifact directory",
       [](const Fixture& paths) {
         renameSync(paths.artifact, paths.artifact + ".saved");
         symlinkSync(paths.artifact + ".saved", paths.artifact);
       },
       "symlinks"},
      {"symlink tracked inputs",
       [](const Fixture& paths) {
         renameSync(paths.inputs, paths.inputs + ".saved");
         symlinkSync(paths.inputs + ".saved", paths.inputs);
       },
       "symlinks"},
      {"unsafe receipt permissions", [](const Fixture& paths) { rt::fs::chmodSync(paths.receipt, 0644); }, "owner-private"},
      {"unsafe storage permissions", [](const Fixture& paths) { rt::fs::chmodSync(paths.data, 0755); }, "owner-private"},
      {"unsafe native permissions", [](const Fixture& paths) { rt::fs::chmodSync(rt::path::join(paths.artifact, "native-kv.node"), 0644); }, "owner-private"},
      {"unsafe provenance permissions", [](const Fixture& paths) { rt::fs::chmodSync(rt::path::join(paths.artifact, "native-kv.provenance.json"), 0644); }, "owner-private"},
      {"unsafe tracked input permissions", [](const Fixture& paths) { rt::fs::chmodSync(paths.inputs, 0666); }, "safe regular"},
      {"symlink native bytes",
       [](const Fixture& paths) {
         const std::string path = rt::path::join(paths.artifact, "native-kv.node");
         renameSync(path, path + ".saved");
         symlinkSync(path + ".saved", path);
       },
       "symlinks"},
      {"symlink provenance",
       [](const Fixture& paths) {
         const std::string path = rt::path::join(paths.artifact, "native-kv.provenance.json");
         renameSync(path, path + ".saved");
         symlinkSync(path + ".saved", path);
       },
       "symlinks"},
    };
    return list;
  }

  rt::Task<void> refuses(std::string name) {
    const Fixtures fixtures;
    const Refusal* refusal = nullptr;
    for (const Refusal& candidate : refusals()) {
      if (candidate.name == name) refusal = &candidate;
    }
    ASSERT(refusal != nullptr);
    const Fixture paths = fixture(fixtures);
    refusal->change(paths);
    const std::vector<std::string> before = sortedListing(paths.data);
    auto run = startup(paths.root);
    const Startup result = co_await std::move(run);
    EXPECT_EQ(result.code, 78);
    EXPECT_EQ(result.stdout_, std::string(""));
    if (!rt::includes(result.stderr_, refusal->error)) ::uos::rt::test::report(__FILE__, __LINE__, name + ": " + result.stderr_ + " lacks " + refusal->error, false);
    EXPECT(!rt::includes(result.stderr_, "Unexpected startup"));
    EXPECT_EQ(sortedListing(paths.data), before);
    EXPECT(!rt::fs::existsSync(rt::path::join(paths.data, "kv-bun.sqlite3")));
    EXPECT(!storage::native_addon::nativeKvBackendIsPrepared());
  }
}

ASYNC_TEST("development startup refuses absent receipt without database or marker writes") { co_await refuses("absent receipt"); }
ASYNC_TEST("development startup refuses corrupt receipt without database or marker writes") { co_await refuses("corrupt receipt"); }
ASYNC_TEST("development startup refuses absent independent hash without database or marker writes") { co_await refuses("absent independent hash"); }
ASYNC_TEST("development startup refuses wrong independent hash without database or marker writes") { co_await refuses("wrong independent hash"); }
ASYNC_TEST("development startup refuses tampered provenance without database or marker writes") { co_await refuses("tampered provenance"); }
ASYNC_TEST("development startup refuses tampered native bytes without database or marker writes") { co_await refuses("tampered native bytes"); }
ASYNC_TEST("development startup refuses tampered tracked inputs without database or marker writes") { co_await refuses("tampered tracked inputs"); }
ASYNC_TEST("development startup refuses symlink receipt without database or marker writes") { co_await refuses("symlink receipt"); }
ASYNC_TEST("development startup refuses symlink artifact directory without database or marker writes") { co_await refuses("symlink artifact directory"); }
ASYNC_TEST("development startup refuses symlink tracked inputs without database or marker writes") { co_await refuses("symlink tracked inputs"); }
ASYNC_TEST("development startup refuses unsafe receipt permissions without database or marker writes") { co_await refuses("unsafe receipt permissions"); }
ASYNC_TEST("development startup refuses unsafe storage permissions without database or marker writes") { co_await refuses("unsafe storage permissions"); }
ASYNC_TEST("development startup refuses unsafe native permissions without database or marker writes") { co_await refuses("unsafe native permissions"); }
ASYNC_TEST("development startup refuses unsafe provenance permissions without database or marker writes") { co_await refuses("unsafe provenance permissions"); }
ASYNC_TEST("development startup refuses unsafe tracked input permissions without database or marker writes") { co_await refuses("unsafe tracked input permissions"); }
ASYNC_TEST("development startup refuses symlink native bytes without database or marker writes") { co_await refuses("symlink native bytes"); }
ASYNC_TEST("development startup refuses symlink provenance without database or marker writes") { co_await refuses("symlink provenance"); }

// Registered last: it prepares the process-wide backend, which the refusals need unset.
ASYNC_TEST("development startup uses the explicit independent receipt before opening real native KV (preparation only)") {
  const Fixtures fixtures;
  const Fixture paths = fixture(fixtures);
  const std::string receipt = rt::fs::readFileSync(paths.receipt, "utf8");
  const storage::native_addon::NativeKvRequire original = storage::native_addon::nativeKvRequire();
  auto loaded = std::make_shared<std::vector<std::string>>();
  // The natively linked backend stands in for the addon `require` would load.
  storage::native_addon::nativeKvRequire() = [loaded](const std::string& path) -> std::optional<storage::native_addon::NapiInterface> {
    loaded->push_back(path);
    storage::native_addon::NapiInterface napi;
    napi.open = [](std::string, std::optional<bool>, bool) { return 1.0; };
    napi.close = [](double, bool) {};
    napi.snapshotRead = [](double, std::vector<std::uint8_t> bytes, bool) { return rt::Promise<std::vector<std::uint8_t>>::resolve(bytes); };
    napi.atomicWrite = [](double, std::vector<std::uint8_t> bytes, bool) { return rt::Promise<std::vector<std::uint8_t>>::resolve(bytes); };
    napi.dequeueNextMessage = [](double, bool) { return rt::Promise<std::optional<storage::native_addon::NapiDequeuedMessage>>::resolve(std::nullopt); };
    napi.finishMessage = [](double, double, bool, bool) { return rt::Promise<void>::resolve(); };
    napi.startWatch = [](double, std::vector<std::uint8_t>, bool) { return rt::Promise<double>::resolve(1.0); };
    napi.dequeueNextWatchMessage = [](double, double, bool) { return rt::Promise<std::optional<std::vector<std::uint8_t>>>::resolve(std::nullopt); };
    napi.endWatch = [](double, double, bool) {};
    return napi;
  };
  auto run = startup(paths.root);
  const Startup result = co_await std::move(run);
  storage::native_addon::nativeKvRequire() = original;
  EXPECT_EQ(result.stderr_, std::string(""));
  EXPECT_EQ(result.code, 0);
  EXPECT_EQ(result.stdout_, std::string("prepared"));
  EXPECT_EQ(*loaded, std::vector<std::string>{rt::path::join(paths.artifact, "native-kv.node")});
  EXPECT(storage::native_addon::nativeKvBackendIsPrepared());
  EXPECT_EQ(std::get<std::string>(storage::native_addon::preparedNativeKvArtifactBinding().artifactDirectory), paths.artifact);
  EXPECT_EQ(rt::fs::readFileSync(paths.receipt, "utf8"), receipt);
  EXPECT_EQ(rt::path::resolve(nativeKvDevelopmentPaths().root), rt::path::resolve(workspace()));
}
