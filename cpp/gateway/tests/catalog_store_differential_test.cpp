// Differential cases for src/catalog/store.ts. Its direct tests drive the catalog handler and are
// mostly not portable yet (see tests/codex_catalog_test.cpp), so these replay what Bun 1.4.0 printed
// for the module itself against a Map-backed KV stub with versionstamp-checked commits:
// - catalog_store_differential_vectors.inc: parseCatalogBody on 20 bodies; 10 storeCodexCatalog /
//   loadCatalog round trips with OpenRouter enrichment seeded (metadata with body_generation and the
//   compressed sizes masked, served body, parsed record, source body, chunk rows); 18 gzip streams
//   written by Bun's CompressionStream and node:zlib (stored, fixed, dynamic, Huffman-only and RLE
//   blocks, two members, an FNAME/FCOMMENT/FEXTRA/FHCRC header) that loadCatalog must inflate;
// - the scenario results below (integrity failures, memo LRU and pruning, auth generation, leases,
//   the current-generation read), printed by Bun for the same steps.
// The store here is rt::MemoryKv. The lease heartbeat and the cold-cache wait run on the test's fake
// clock; their expectations follow the TS timers (a renewal every floor(15000 / 3) ms, a 50 ms poll
// until the 5000 ms deadline). The gzip writer is uos::rt::gzip (zlib; Bun's zlib picks other blocks), so
// compressed sizes are not compared; every stream it writes is inflated back by loadCatalog.
// Links the sources of tests/codex_catalog_test.cpp.
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/models/openrouter_models.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::catalog::store;
using namespace uos::catalog::types;
namespace openrouter = uos::models::openrouter_models;

namespace {
#include "catalog_store_differential_vectors.inc"

  const std::string GEN = "gen-1";
  constexpr double NOW = 1'800'000'000'000;

  struct Store {
    std::shared_ptr<rt::MemoryKvDatabase> database;
    std::shared_ptr<rt::MemoryKv> kv;
  };

  // An empty store; with a non-empty `generation`, the catalog auth generation row. The memo is reset.
  rt::Task<Store> freshStore(std::string generation) {
    resetCodexCatalogMemoForTest();
    auto database = std::make_shared<rt::MemoryKvDatabase>([] { return rt::dateNow(); });
    auto kv = std::make_shared<rt::MemoryKv>(database);
    if (!generation.empty()) {
      const rt::KvValue value(generation);
      auto writing = kv->set(CODEX_CATALOG_AUTH_GENERATION_KEY, value);
      co_await std::move(writing);
    }
    Store fresh;
    fresh.database = std::move(database);
    fresh.kv = std::move(kv);
    co_return fresh;
  }

  rt::KvKey metadataKey(const std::string& version) { return rt::KvKey{CODEX_CATALOG_PREFIX, version}; }
  rt::KvKey leaseKey(const std::string& version) { return rt::KvKey{CODEX_CATALOG_LEASE_PREFIX, version}; }

  std::string body(const std::string& version) { return R"({"models":[{"slug":"gpt-)" + version + R"("}]})"; }

  rt::Task<bool> put(std::shared_ptr<rt::Kv> kv, std::string version, double fetchedAtMs) {
    const StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = GEN, .body = body(version), .fetchedAtMs = fetchedAtMs};
    auto storing = storeCodexCatalog(*kv, input);
    const bool stored = co_await std::move(storing);
    co_return stored;
  }

  // The served body, or "null".
  rt::Task<std::string> load(std::shared_ptr<rt::Kv> kv, std::string version, double nowMs, std::string generation) {
    auto loading = loadCatalog(*kv, std::move(version), std::move(generation), nowMs);
    const auto loaded = co_await std::move(loading);
    co_return loaded ? loaded->body : std::string("null");
  }

  rt::Task<void> setValue(std::shared_ptr<rt::Kv> kv, rt::KvKey key, rt::KvValue value) {
    auto writing = kv->set(std::move(key), std::move(value));
    co_await std::move(writing);
  }

  rt::Task<void> deleteKey(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
    auto deleting = kv->delete_(std::move(key));
    co_await std::move(deleting);
  }

  json::object metadataOf(const Store& store, const std::string& version) {
    json::value value = store.database->row(metadataKey(version))->value.toJson();
    return std::move(value.as_object());
  }

  // Rewrites the stored metadata (`{ ...metadata, ...patch }`, minus `remove`) and resets the memo.
  rt::Task<void> editMetadata(Store store, std::string version, json::object patch, std::vector<std::string> remove) {
    json::object metadata = metadataOf(store, version);
    for (const json::key_value_pair& member : patch) rt::objectSet(metadata, member.key(), member.value());
    for (const std::string& key : remove) rt::objectDelete(metadata, key);
    auto writing = setValue(store.kv, metadataKey(version), rt::KvValue(json::value(metadata)));
    co_await std::move(writing);
    resetCodexCatalogMemoForTest();
  }

  rt::KvKey firstChunkKey(const Store& store, const std::string& version) {
    const json::object metadata = metadataOf(store, version);
    return rt::KvKey{CODEX_CATALOG_CHUNK_PREFIX, version, std::string(metadata.at("body_generation").as_string()), 0};
  }

  bool startsWith(const rt::KvKey& key, const rt::KvKey& prefix) {
    if (key.size() < prefix.size()) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index)
      if (!(key[index] == prefix[index])) return false;
    return true;
  }

  std::size_t rowsUnder(const Store& store, const rt::KvKey& prefix) {
    std::size_t count = 0;
    for (const rt::MemoryKvDatabase::Row& row : store.database->rows())
      if (startsWith(row.key, prefix)) count += 1;
    return count;
  }

  // Client versions of the rows under `prefix` (key part 3), in key order without repeats.
  std::vector<std::string> versionsUnder(const Store& store, const rt::KvKey& prefix) {
    std::vector<std::string> versions;
    for (const rt::MemoryKvDatabase::Row& row : store.database->rows()) {
      if (!startsWith(row.key, prefix) || row.key.size() <= prefix.size()) continue;
      const std::string& version = row.key[prefix.size()].string();
      if (versions.empty() || versions.back() != version) versions.push_back(version);
    }
    return versions;
  }

  rt::Task<void> seedOpenRouter() {
    openrouter::resetOpenRouterModelsCacheForTest();
    openrouter::FetchOpenRouterModelsOptions options;
    options.force = true;
    options.fetcher = rt::Fetch([](rt::RequestInfo, rt::RequestInit) {
      return rt::Response::json(rt::jsonParse(R"({"data":[{"id":"openai/gpt-6-astra","context_length":1050000},)"
                                              R"({"id":"openai/gpt-6.1-sol","context_length":1050000},{"id":"openai/gpt-small","context_length":100000},)"
                                              R"({"id":"acme/zero","context_length":0}]})"));
    });
    auto fetching = openrouter::fetchOpenRouterModels(std::move(options));
    co_await std::move(fetching);
  }

  std::string sha256Hex(std::string_view text) { return rt::crypto::createHash("sha256").update(text).digest("hex"); }

  // The texts the Bun script compressed (the long ones are rebuilt here; their SHA-256 is checked).
  std::string recipeText(std::string_view recipe, const json::value* recorded) {
    if (recorded != nullptr && recorded->is_string()) return std::string(recorded->get_string());
    if (recipe == "medium") {
      json::array models;
      for (int index = 0; index < 100; ++index) {
        json::object model;
        model["slug"] = "model-" + std::to_string(index);
        model["description"] = rt::repeat("repeat ", 40);
        model["id"] = sha256Hex(std::to_string(index));
        models.push_back(std::move(model));
      }
      json::object root;
      root["models"] = std::move(models);
      return rt::jsonStringify(json::value(root));
    }
    json::object model;
    model["slug"] = "\xC3\xA9\xF0\x9F\x98\x80";
    model["text"] = rt::repeat("abc", 5000) + rt::repeat("\xC3\xA9\xF0\x9F\x98\x80", 1000);
    json::object root;
    root["models"] = json::array{model};
    return rt::jsonStringify(json::value(root));
  }

  // Commits that always fail, for the generation retry bound.
  class RefusingAtomic final : public rt::ForwardingAtomicOperation {
  public:
    using rt::ForwardingAtomicOperation::ForwardingAtomicOperation;
    rt::Task<rt::KvCommitResult> commit() override {
      rt::KvCommitResult refused;
      co_return refused;
    }
  };
  class RefusingKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<RefusingAtomic>(inner.atomic()); }
  };

  // Reads that fail on demand, for the lease heartbeat's error path.
  class FailingGetKv final : public rt::ForwardingKv {
  public:
    using rt::ForwardingKv::ForwardingKv;
    using rt::Kv::get;
    bool fail = false;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options) override {
      if (fail) throw rt::JsError("read failed");
      auto reading = inner.get(std::move(key), options);
      rt::KvEntryMaybe<> entry = co_await std::move(reading);
      co_return entry;
    }
  };

  // One integrity scenario: store "1.0.0", apply `name`, then load at `now`.
  rt::Task<std::string> integrityScenario(std::string name) {
    auto opening = freshStore(GEN);
    const Store store = co_await std::move(opening);
    auto storing = put(store.kv, "1.0.0", NOW);
    co_await std::move(storing);
    double now = NOW + 1;
    std::optional<json::object> patch;
    std::vector<std::string> remove;
    if (name == "corrupt chunk") {
      const rt::KvKey key = firstChunkKey(store, "1.0.0");
      rt::KvBytes bytes = store.database->row(key)->value.asBytes();
      bytes[12] ^= 0xFF;
      auto writing = setValue(store.kv, key, rt::KvValue(bytes));
      co_await std::move(writing);
      resetCodexCatalogMemoForTest();
    } else if (name == "compressed_bytes off by one" || name == "body_bytes off by one") {
      const std::string field = name == "compressed_bytes off by one" ? "compressed_bytes" : "body_bytes";
      patch.emplace();
      (*patch)[field] = rt::makeNumber(*rt::getNumber(metadataOf(store, "1.0.0"), field) + 1);
    } else if (name == "sha256 mismatch") {
      patch.emplace();
      (*patch)["sha256"] = std::string(64, '0');
    } else if (name == "missing chunk") {
      auto deleting = deleteKey(store.kv, firstChunkKey(store, "1.0.0"));
      co_await std::move(deleting);
      resetCodexCatalogMemoForTest();
    } else if (name == "string chunk") {
      auto writing = setValue(store.kv, firstChunkKey(store, "1.0.0"), rt::KvValue("abc"));
      co_await std::move(writing);
      resetCodexCatalogMemoForTest();
    } else if (name == "etag missing") {
      remove = {"etag"};
    } else if (name == "chunk_count zero" || name == "chunk_count fractional") {
      patch.emplace();
      (*patch)["chunk_count"] = rt::makeNumber(name == "chunk_count zero" ? 0.0 : 1.5);
    } else if (name == "fetched_at_ms string") {
      patch.emplace();
      (*patch)["fetched_at_ms"] = rt::numberToString(NOW);
    } else if (name == "source_sha256 missing") {
      remove = {"source_sha256"};
    } else if (name == "source fields absent") {
      remove = {"source_chunk_count", "source_compressed_bytes", "source_body_bytes", "source_sha256"};
    } else if (name == "retention edge") {
      now = NOW + CODEX_CATALOG_RETENTION_MS;
    } else if (name == "retention minus one") {
      now = NOW + CODEX_CATALOG_RETENTION_MS - 1;
    }
    if (patch || !remove.empty()) {
      auto editing = editMetadata(store, "1.0.0", patch.value_or(json::object()), remove);
      co_await std::move(editing);
    }
    auto loading = load(store.kv, "1.0.0", now, GEN);
    const std::string result = co_await std::move(loading);
    co_return result;
  }
}

TEST("parseCatalogBody matches Bun") {
  const json::value vectors = rt::jsonParse(CATALOG_STORE_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("parse").as_array()) {
    const std::string text(vector.at("body").as_string());
    const std::optional<json::object> parsed = parseCatalogBody(text);
    const std::string actual = parsed ? rt::jsonStringify(json::value(*parsed)) : std::string("null");
    EXPECT_EQ(actual, rt::jsonStringify(vector.at("out")));
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{20});
}

ASYNC_TEST("storeCodexCatalog and loadCatalog round trips match Bun") {
  auto seeding = seedOpenRouter();
  co_await std::move(seeding);
  const json::value vectors = rt::jsonParse(CATALOG_STORE_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("roundTrips").as_array()) {
    const json::object& input = vector.at("input").as_object();
    const std::string version(input.at("version").as_string());
    const double fetchedAtMs = *rt::getNumber(input, "fetchedAtMs");
    auto opening = freshStore("auth-generation-test");
    const Store store = co_await std::move(opening);
    StoreCodexCatalogInput storeInput{.clientVersion = version, .authGeneration = "auth-generation-test", .body = std::string(input.at("body").as_string())};
    if (const std::optional<std::string_view> etag = rt::getString(input, "etag")) storeInput.etag = std::string(*etag);
    if (const std::optional<std::string_view> contentType = rt::getString(input, "contentType")) storeInput.contentType = std::string(*contentType);
    if (const std::optional<std::string_view> generation = rt::getString(input, "generation")) storeInput.authGeneration = std::string(*generation);
    storeInput.fetchedAtMs = fetchedAtMs;
    auto storing = storeCodexCatalog(*store.kv, storeInput);
    const bool stored = co_await std::move(storing);
    EXPECT_EQ(stored, vector.at("stored").as_bool());

    std::string metadataText = "null";
    if (store.database->row(metadataKey(version))) {
      json::object metadata = metadataOf(store, version);
      rt::objectSet(metadata, "body_generation", json::string("<uuid>"));
      rt::objectSet(metadata, "compressed_bytes", json::string("<n>"));
      if (rt::hasOwn(metadata, "source_compressed_bytes")) rt::objectSet(metadata, "source_compressed_bytes", json::string("<n>"));
      metadataText = rt::jsonStringify(json::value(metadata));
    }
    EXPECT_EQ(metadataText, rt::jsonStringify(vector.at("metadata")));

    auto loading = loadCatalog(*store.kv, version, "auth-generation-test", fetchedAtMs + 1);
    const auto loaded = co_await std::move(loading);
    std::string loadedText = "null";
    if (loaded) {
      json::object view;
      view["body"] = loaded->body;
      view["parsed"] = loaded->parsed;
      auto readingSource = loadCatalogSource(*store.kv, loaded);
      auto source = co_await std::move(readingSource);
      view["source_body"] = source ? json::value(json::string(*source)) : json::value(nullptr);
      loadedText = rt::jsonStringify(json::value(view));
    }
    EXPECT_EQ(loadedText, rt::jsonStringify(vector.at("loaded")));
    EXPECT_EQ(static_cast<double>(rowsUnder(store, CODEX_CATALOG_CHUNK_PREFIX)), *rt::getNumber(vector, "chunkRows"));
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{10});
  openrouter::resetOpenRouterModelsCacheForTest();
}

ASYNC_TEST("loadCatalog inflates gzip streams written by Bun") {
  const json::value vectors = rt::jsonParse(CATALOG_STORE_VECTORS);
  std::size_t checked = 0;
  for (const json::value& vector : vectors.at("foreign").as_array()) {
    const json::object& entry = vector.as_object();
    const std::string recipe(entry.at("recipe").as_string());
    const std::string name(entry.at("name").as_string());
    const std::string text = recipeText(recipe, rt::get(entry, "text"));
    EXPECT_EQ(sha256Hex(text), std::string(entry.at("sha256").as_string()));
    const std::vector<std::uint8_t> compressed = rt::fromHex(entry.at("hex").as_string());
    auto opening = freshStore(GEN);
    const Store store = co_await std::move(opening);
    auto writingChunk = setValue(store.kv, rt::KvKey{CODEX_CATALOG_CHUNK_PREFIX, "2.0.0", "foreign", 0}, rt::KvValue(compressed));
    co_await std::move(writingChunk);
    json::object metadata;
    metadata["client_version"] = "2.0.0";
    metadata["auth_generation"] = GEN;
    metadata["body_generation"] = "foreign";
    metadata["content_type"] = "application/json";
    metadata["etag"] = nullptr;
    metadata["fetched_at_ms"] = rt::makeNumber(NOW);
    metadata["chunk_count"] = 1;
    metadata["compressed_bytes"] = rt::makeNumber(static_cast<double>(compressed.size()));
    metadata["body_bytes"] = entry.at("body_bytes");
    metadata["sha256"] = entry.at("sha256");
    auto writingMetadata = setValue(store.kv, metadataKey("2.0.0"), rt::KvValue(json::value(metadata)));
    co_await std::move(writingMetadata);
    auto loading = load(store.kv, "2.0.0", NOW + 1, GEN);
    const std::string served = co_await std::move(loading);
    EXPECT(served == text);
    if (served != text) std::fprintf(stderr, "    stream %s/%s did not inflate\n", recipe.c_str(), name.c_str());
    checked += 1;
  }
  EXPECT_EQ(checked, std::size_t{18});
}

ASYNC_TEST("large bodies are split into bounded chunks and inflate back") {
  // The store-level half of "codex catalog: gzip chunks are bounded and integrity failures force a refresh".
  json::array models;
  for (int index = 0; index < 4'000; ++index) {
    json::object model;
    model["slug"] = "model-" + std::to_string(index) + "-" + rt::crypto::randomUUID();
    model["description"] = rt::crypto::randomUUID();
    models.push_back(std::move(model));
  }
  json::object root;
  root["models"] = std::move(models);
  const std::string text = rt::jsonStringify(json::value(root));
  auto opening = freshStore(GEN);
  const Store store = co_await std::move(opening);
  const StoreCodexCatalogInput input{.clientVersion = "0.152.0", .authGeneration = GEN, .body = text};
  auto storing = storeCodexCatalog(*store.kv, input);
  const bool stored = co_await std::move(storing);
  EXPECT(stored);
  const rt::KvKey chunks{CODEX_CATALOG_CHUNK_PREFIX, "0.152.0"};
  EXPECT(rowsUnder(store, chunks) > 1);
  bool bounded = true;
  for (const rt::MemoryKvDatabase::Row& row : store.database->rows())
    if (startsWith(row.key, chunks)) bounded = bounded && static_cast<double>(row.value.asBytes().size()) <= CODEX_CATALOG_CHUNK_BYTES;
  EXPECT(bounded);
  auto loading = loadCatalog(*store.kv, "0.152.0", GEN, rt::dateNow());
  const auto loaded = co_await std::move(loading);
  ASSERT(loaded);
  auto readingSource = loadCatalogSource(*store.kv, loaded);
  const auto source = co_await std::move(readingSource);
  EXPECT(loaded->body == text && source == std::optional<std::string>(text));
  resetCodexCatalogMemoForTest();
}

ASYNC_TEST("integrity failures and metadata guards load nothing") {
  const std::string served = body("1.0.0");
  const std::vector<std::pair<std::string, bool>> expectations{
    {"baseline", true},
    {"corrupt chunk", false},
    {"compressed_bytes off by one", false},
    {"body_bytes off by one", false},
    {"sha256 mismatch", false},
    {"missing chunk", false},
    {"string chunk", false},
    {"etag missing", false},
    {"chunk_count zero", false},
    {"chunk_count fractional", false},
    {"fetched_at_ms string", false},
    {"source_sha256 missing", false},
    {"source fields absent", true},
    {"retention edge", false},
    {"retention minus one", true},
  };
  for (const auto& [name, loads] : expectations) {
    auto running = integrityScenario(name);
    const std::string result = co_await std::move(running);
    const std::string expected = loads ? served : std::string("null");
    EXPECT_EQ(result, expected);
    if (result != expected) std::fprintf(stderr, "    scenario: %s\n", name.c_str());
  }

  auto openingOther = freshStore(GEN);
  const Store other = co_await std::move(openingOther);
  auto storingOther = put(other.kv, "1.0.0", NOW);
  co_await std::move(storingOther);
  auto loadingOther = load(other.kv, "1.0.0", NOW + 1, "gen-2");
  const std::string otherGeneration = co_await std::move(loadingOther);
  EXPECT_EQ(otherGeneration, std::string("null"));

  // A corrupt source body keeps the catalog without `source_body`.
  auto openingSource = freshStore(GEN);
  const Store source = co_await std::move(openingSource);
  auto storingSource = put(source.kv, "1.0.0", NOW);
  co_await std::move(storingSource);
  const std::string generation(metadataOf(source, "1.0.0").at("body_generation").as_string());
  auto writingJunk = setValue(source.kv, rt::KvKey{CODEX_CATALOG_SOURCE_CHUNK_PREFIX, "1.0.0", generation, 0}, rt::KvValue(rt::KvBytes{1, 2, 3}));
  co_await std::move(writingJunk);
  resetCodexCatalogMemoForTest();
  auto loading = loadCatalog(*source.kv, "1.0.0", GEN, NOW + 1);
  const auto loaded = co_await std::move(loading);
  ASSERT(loaded);
  auto readingSource = loadCatalogSource(*source.kv, loaded);
  const auto sourceBody = co_await std::move(readingSource);
  EXPECT(loaded->body == served && !sourceBody.has_value());
  resetCodexCatalogMemoForTest();
}

ASYNC_TEST("the memo serves hits, keeps 32 catalogs in LRU order, and pruning keeps 32 versions") {
  auto openingHit = freshStore(GEN);
  const Store hit = co_await std::move(openingHit);
  auto storingHit = put(hit.kv, "1.0.0", NOW);
  co_await std::move(storingHit);
  auto warming = load(hit.kv, "1.0.0", NOW + 1, GEN);
  co_await std::move(warming);
  auto dropping = deleteKey(hit.kv, firstChunkKey(hit, "1.0.0"));
  co_await std::move(dropping);
  auto hitting = load(hit.kv, "1.0.0", NOW + 1, GEN);
  const std::string memoHit = co_await std::move(hitting);
  EXPECT_EQ(memoHit, body("1.0.0"));

  auto opening = freshStore(GEN);
  const Store store = co_await std::move(opening);
  for (int index = 0; index < 33; ++index) {
    const std::string version = "1.0." + std::to_string(index);
    auto storing = put(store.kv, version, NOW + index);
    co_await std::move(storing);
    auto loading = load(store.kv, version, NOW + 100, GEN);
    co_await std::move(loading);
  }
  std::vector<std::string> after33;
  for (int index = 1; index <= 32; ++index) after33.push_back("1.0." + std::to_string(index));
  EXPECT_EQ(getCodexCatalogMemoVersionsForTest(), after33);
  auto touching = load(store.kv, "1.0.5", NOW + 100, GEN);
  co_await std::move(touching);
  std::vector<std::string> afterHit;
  for (int index = 1; index <= 32; ++index)
    if (index != 5) afterHit.push_back("1.0." + std::to_string(index));
  afterHit.push_back("1.0.5");
  EXPECT_EQ(getCodexCatalogMemoVersionsForTest(), afterHit);
  EXPECT_EQ(rowsUnder(store, CODEX_CATALOG_PREFIX), std::size_t{32});

  auto storing40 = put(store.kv, "1.0.40", NOW + 200);
  co_await std::move(storing40);
  auto storing41 = put(store.kv, "1.0.41", NOW + 201);
  co_await std::move(storing41);
  std::vector<std::string> kept;
  for (int index = 3; index <= 32; ++index) kept.push_back("1.0." + std::to_string(index));
  kept.push_back("1.0.40");
  kept.push_back("1.0.41");
  std::vector<std::string> metadataVersions = versionsUnder(store, CODEX_CATALOG_PREFIX);
  std::vector<std::string> chunkVersions = versionsUnder(store, CODEX_CATALOG_CHUNK_PREFIX);
  rt::sort(metadataVersions);
  rt::sort(chunkVersions);
  rt::sort(kept);
  EXPECT_EQ(metadataVersions, kept);
  EXPECT_EQ(chunkVersions, kept);
  std::vector<std::string> memoAfter35;
  for (int index = 3; index <= 32; ++index)
    if (index != 5) memoAfter35.push_back("1.0." + std::to_string(index));
  memoAfter35.push_back("1.0.5");
  EXPECT_EQ(getCodexCatalogMemoVersionsForTest(), memoAfter35);
  auto restoring = put(store.kv, "1.0.40", NOW + 300);
  co_await std::move(restoring);
  EXPECT_EQ(getCodexCatalogMemoVersionsForTest(), memoAfter35);
  resetCodexCatalogMemoForTest();
}

ASYNC_TEST("large catalogs evict by served bytes while a reader keeps its catalog") {
  auto opening = freshStore(GEN);
  const Store store = co_await std::move(opening);
  const std::string text = R"({"models":[{"slug":"large","base_instructions":")" +
    std::string(CODEX_CATALOG_MEMO_BODY_BYTES / 2, 'a') + R"("}]})";
  const std::vector<std::string> versions{"1.0.0", "1.0.1"};
  Catalog first;
  for (const auto& version : versions) {
    StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = GEN, .body = text, .fetchedAtMs = NOW};
    auto writing = storeCodexCatalog(*store.kv, std::move(input));
    const bool stored = co_await std::move(writing);
    ASSERT(stored);
    auto loading = loadCatalog(*store.kv, version, GEN, NOW + 1);
    const auto loaded = co_await std::move(loading);
    ASSERT(loaded);
    EXPECT(loaded->body == text);
    if (!first) first = loaded;
  }
  EXPECT_EQ(getCodexCatalogMemoVersionsForTest(), std::vector<std::string>{"1.0.1"});
  EXPECT_EQ(first->metadata.client_version, versions.front());
  EXPECT(first->body == text);
  resetCodexCatalogMemoForTest();
}

ASYNC_TEST("the auth generation is created once, replaces a falsy value and gives up after three lost commits") {
  auto opening = freshStore("");
  const Store store = co_await std::move(opening);
  auto creating = getAuthGeneration(*store.kv);
  const std::string first = co_await std::move(creating);
  EXPECT_EQ(first.size(), std::size_t{36});
  auto rereading = getAuthGeneration(*store.kv);
  const std::string again = co_await std::move(rereading);
  EXPECT_EQ(again, first);
  auto blanking = setValue(store.kv, CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(""));
  co_await std::move(blanking);
  auto replacing = getAuthGeneration(*store.kv);
  const std::string replaced = co_await std::move(replacing);
  EXPECT(!replaced.empty() && replaced != first);

  auto openingRefusing = freshStore("");
  const Store refusingStore = co_await std::move(openingRefusing);
  RefusingKv refusing(*refusingStore.kv);
  EXPECT_REJECTS_MESSAGE(getAuthGeneration(refusing), rt::JsError, "Deno KV could not initialize the Codex catalog auth generation");

  // A truthy generation that is not a string has no C++ value (the TS returns it as is).
  auto numbering = setValue(store.kv, CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(5));
  co_await std::move(numbering);
  EXPECT_REJECTS_MESSAGE(getAuthGeneration(*store.kv), rt::TypeError, "KV value is number, not string");

  auto checkingMissing = authGenerationIsCurrent(*refusingStore.kv, "x");
  const bool missing = co_await std::move(checkingMissing);
  EXPECT_EQ(missing, false);
  auto settingX = setValue(refusingStore.kv, CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue("x"));
  co_await std::move(settingX);
  auto checkingCurrent = authGenerationIsCurrent(*refusingStore.kv, "x");
  const bool current = co_await std::move(checkingCurrent);
  EXPECT_EQ(current, true);
}

ASYNC_TEST("refresh leases follow the stored lease and JS comparison of its expiry") {
  auto opening = freshStore("");
  const Store store = co_await std::move(opening);
  const auto acquire = [&store](std::string owner, double nowMs) { return acquireRefreshLease(*store.kv, "1.0.0", std::move(owner), nowMs); };
  auto a = acquire("a", NOW);
  const bool acquiredFirst = co_await std::move(a);
  EXPECT_EQ(acquiredFirst, true);
  EXPECT(rt::deepEqual(store.database->row(leaseKey("1.0.0"))->value.toJson(), rt::jsonParse(R"({"owner":"a","lease_until_ms":1800000015000})")));
  auto b1 = acquire("b", NOW);
  const bool heldNow = co_await std::move(b1);
  EXPECT_EQ(heldNow, false);
  auto b2 = acquire("b", NOW + 14'999);
  const bool heldLater = co_await std::move(b2);
  EXPECT_EQ(heldLater, false);
  auto b3 = acquire("b", NOW + 15'000);
  const bool expired = co_await std::move(b3);
  EXPECT_EQ(expired, true);
  EXPECT(rt::deepEqual(store.database->row(leaseKey("1.0.0"))->value.toJson(), rt::jsonParse(R"({"owner":"b","lease_until_ms":1800000030000})")));
  EXPECT_EQ(store.database->row(leaseKey("1.0.0"))->expirationMs, rt::dateNow() + 30'000);
  auto releasingOther = releaseRefreshLease(*store.kv, "1.0.0", "a");
  co_await std::move(releasingOther);
  EXPECT(store.database->row(leaseKey("1.0.0")).has_value());
  auto releasingOwn = releaseRefreshLease(*store.kv, "1.0.0", "b");
  co_await std::move(releasingOwn);
  EXPECT(!store.database->row(leaseKey("1.0.0")).has_value());

  // Stored lease values Bun compared with `lease_until_ms > nowMs`.
  std::vector<std::pair<rt::KvValue, bool>> stored;
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":"9e99"})")), false);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":null})")), true);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":[9e15]})")), false);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":[]})")), true);
  stored.emplace_back(rt::KvValue(true), true);
  stored.emplace_back(rt::KvValue("text"), true);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z"})")), true);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":[[9e15]]})")), false);
  stored.emplace_back(rt::KvValue(rt::jsonParse(R"({"owner":"z","lease_until_ms":["1","2"]})")), true);
  // Values only a stored structured clone holds, with what Bun answered for each.
  const auto lease = [](rt::KvValue until) {
    rt::KvObject object;
    object.set("owner", rt::KvValue("z"));
    object.set("lease_until_ms", std::move(until));
    return rt::KvValue(std::move(object));
  };
  const auto view = [](rt::KvViewKind kind, std::vector<std::uint8_t> buffer) {
    rt::KvArrayBufferView typed;
    typed.kind = kind;
    typed.byteLength = buffer.size();
    typed.buffer = std::move(buffer);
    return rt::KvValue(std::move(typed));
  };
  const auto littleEndian = [](auto number) {
    std::vector<std::uint8_t> bytes(sizeof(number));
    std::memcpy(bytes.data(), &number, sizeof(number));
    return bytes;
  };
  stored.emplace_back(lease(rt::KvValue(rt::BigInt::fromNumber(9e15))), false);
  stored.emplace_back(lease(rt::KvValue(rt::KvDate{8e15})), false);
  stored.emplace_back(lease(rt::KvValue(rt::KvDate{std::numeric_limits<double>::quiet_NaN()})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvBytes{9})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvBytes{})), true);
  stored.emplace_back(lease(view(rt::KvViewKind::Float64Array, littleEndian(9e15))), false);
  stored.emplace_back(lease(view(rt::KvViewKind::BigInt64Array, littleEndian(std::int64_t{9'000'000'000'000'000}))), false);
  stored.emplace_back(lease(view(rt::KvViewKind::DataView, std::vector<std::uint8_t>(8))), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvArray{rt::KvValue(rt::KvDate{std::numeric_limits<double>::quiet_NaN()})})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvArray{rt::KvValue(rt::BigInt::fromNumber(9e15))})), false);
  stored.emplace_back(lease(rt::KvValue(rt::KvArray{rt::KvValue()})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvArray{rt::KvValue(nullptr)})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvMap{})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvRegExp{"x", ""})), true);
  stored.emplace_back(lease(rt::KvValue(rt::KvError{.name = "Error", .message = "9e15"})), true);
  for (auto& [value, acquires] : stored) {
    auto writing = setValue(store.kv, leaseKey("1.0.0"), value);
    co_await std::move(writing);
    auto acquiring = acquire("c", NOW);
    const bool acquired = co_await std::move(acquiring);
    EXPECT_EQ(acquired, acquires);
  }
}

ASYNC_TEST("a lease heartbeat renews every five seconds until it is lost or stopped") {
  auto opening = freshStore("");
  const Store store = co_await std::move(opening);
  const double start = rt::dateNow();
  auto acquiring = acquireRefreshLease(*store.kv, "1.0.0", "owner-a", start);
  co_await std::move(acquiring);
  RefreshLeaseHeartbeat heartbeat = startRefreshLeaseHeartbeat(*store.kv, "1.0.0", "owner-a");
  co_await rt::sleep(5'001);
  EXPECT(rt::deepEqual(store.database->row(leaseKey("1.0.0"))->value["lease_until_ms"].toJson(), rt::makeNumber(start + 5'000 + 15'000)));
  EXPECT_EQ(heartbeat.lost(), false);
  co_await rt::sleep(5'000);
  EXPECT(rt::deepEqual(store.database->row(leaseKey("1.0.0"))->value["lease_until_ms"].toJson(), rt::makeNumber(start + 10'000 + 15'000)));
  // Another owner takes the lease: the next renewal loses it and stops scheduling.
  auto stealing = setValue(store.kv, leaseKey("1.0.0"), rt::KvValue(rt::jsonParse(R"({"owner":"owner-b","lease_until_ms":1})")));
  co_await std::move(stealing);
  co_await rt::sleep(5'000);
  EXPECT_EQ(heartbeat.lost(), true);
  auto stopping = heartbeat.stop();
  co_await std::move(stopping);

  // Stopped before its first renewal: the lease is never renewed.
  auto reacquiring = acquireRefreshLease(*store.kv, "1.0.1", "owner-a", rt::dateNow());
  co_await std::move(reacquiring);
  const json::value before = store.database->row(leaseKey("1.0.1"))->value.toJson();
  RefreshLeaseHeartbeat stopped = startRefreshLeaseHeartbeat(*store.kv, "1.0.1", "owner-a");
  auto stoppingEarly = stopped.stop();
  co_await std::move(stoppingEarly);
  co_await rt::sleep(20'000);
  EXPECT(rt::deepEqual(store.database->row(leaseKey("1.0.1"))->value.toJson(), before));
  EXPECT_EQ(stopped.lost(), false);

  // A failed renewal reports the error and marks the lease lost.
  FailingGetKv failing(*store.kv);
  auto acquiringFailing = acquireRefreshLease(failing, "1.0.2", "owner-a", rt::dateNow());
  co_await std::move(acquiringFailing);
  rt::console::Capture capture({rt::console::Method::error});
  RefreshLeaseHeartbeat failed = startRefreshLeaseHeartbeat(failing, "1.0.2", "owner-a");
  failing.fail = true;
  co_await rt::sleep(5'001);
  EXPECT_EQ(failed.lost(), true);
  ASSERT_EQ(capture.calls().size(), std::size_t{1});
  EXPECT(capture.calls()[0].args[0] == "[ai.ubq.fi] Codex catalog lease renewal failed for 1.0.2:");
  EXPECT(capture.calls()[0].args[1].errorInfo().message == "read failed");
  auto stoppingFailed = failed.stop();
  co_await std::move(stoppingFailed);

  // Release errors are logged, not thrown.
  auto releasing = releaseRefreshLease(failing, "1.0.2", "owner-a");
  co_await std::move(releasing);
  ASSERT_EQ(capture.calls().size(), std::size_t{2});
  EXPECT(capture.calls()[1].args[0] == "[ai.ubq.fi] Codex catalog lease release failed for 1.0.2:");
}

ASYNC_TEST("cold waits poll until the catalog appears or the deadline passes") {
  auto opening = freshStore(GEN);
  const Store store = co_await std::move(opening);
  const double start = rt::dateNow();
  auto waitingEmpty = waitForColdCatalog(*store.kv, "1.0.0");
  const auto none = co_await std::move(waitingEmpty);
  EXPECT(!none);
  EXPECT_EQ(rt::dateNow() - start, CODEX_CATALOG_COLD_WAIT_MS);

  const double second = rt::dateNow();
  std::shared_ptr<rt::Kv> kv = store.kv;
  rt::setTimeout(
    [kv] {
      const StoreCodexCatalogInput input{.clientVersion = "1.0.0", .authGeneration = GEN, .body = body("1.0.0"), .fetchedAtMs = rt::dateNow()};
      rt::spawn(rt::async([kv, input]() { return storeCodexCatalog(*kv, input); }));
    },
    120);
  auto waiting = waitForColdCatalog(*store.kv, "1.0.0");
  const auto found = co_await std::move(waiting);
  EXPECT(found && found->body == body("1.0.0"));
  EXPECT_EQ(rt::dateNow() - second, 150.0);

  auto readingCurrent = loadCurrentGenerationCatalog(*store.kv, "1.0.0");
  const auto current = co_await std::move(readingCurrent);
  EXPECT(current && current->body == body("1.0.0"));
  auto dropping = deleteKey(store.kv, CODEX_CATALOG_AUTH_GENERATION_KEY);
  co_await std::move(dropping);
  auto readingNone = loadCurrentGenerationCatalog(*store.kv, "1.0.0");
  const auto absent = co_await std::move(readingNone);
  EXPECT(!absent);
  resetCodexCatalogMemoForTest();
}
