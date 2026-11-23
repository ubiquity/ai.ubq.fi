// Differential cases for src/catalog/types.ts (its listed TS tests exercise the catalog
// modules that use these exports): the expected values were recorded from Bun 1.4.0 as
// JSON.stringify(module namespace).
#include "uos/catalog/types.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::catalog::types;

  const char* const RECORDED =
      R"({"CODEX_CATALOG_AUTH_GENERATION_KEY":["ubq_ai","codex_catalog_auth_generation"],"CODEX_CATALOG_CHUNK_BYTES":55000,"CODEX_CATALOG_CHUNK_PREFIX":["ubq_ai","codex_catalog_chunk"],"CODEX_CATALOG_COLD_WAIT_MS":5000,"CODEX_CATALOG_FRESH_MS":300000,"CODEX_CATALOG_LEASE_PREFIX":["ubq_ai","codex_catalog_refresh_lease"],"CODEX_CATALOG_MAX_VERSIONS":32,"CODEX_CATALOG_PREFIX":["ubq_ai","codex_catalog"],"CODEX_CATALOG_REFRESH_LEASE_MS":15000,"CODEX_CATALOG_RETENTION_MS":86400000,"CODEX_CATALOG_SOURCE_CHUNK_PREFIX":["ubq_ai","codex_catalog_source_chunk"],"PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS":120000})";

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const auto& part : key) out.push_back(json::string(part.string()));
    return out;
  }
}

TEST("differential: catalog constants and key prefixes match Bun") {
  json::object exported;
  exported["CODEX_CATALOG_AUTH_GENERATION_KEY"] = keyJson(CODEX_CATALOG_AUTH_GENERATION_KEY);
  exported["CODEX_CATALOG_CHUNK_BYTES"] = CODEX_CATALOG_CHUNK_BYTES;
  exported["CODEX_CATALOG_CHUNK_PREFIX"] = keyJson(CODEX_CATALOG_CHUNK_PREFIX);
  exported["CODEX_CATALOG_COLD_WAIT_MS"] = CODEX_CATALOG_COLD_WAIT_MS;
  exported["CODEX_CATALOG_FRESH_MS"] = CODEX_CATALOG_FRESH_MS;
  exported["CODEX_CATALOG_LEASE_PREFIX"] = keyJson(CODEX_CATALOG_LEASE_PREFIX);
  exported["CODEX_CATALOG_MAX_VERSIONS"] = CODEX_CATALOG_MAX_VERSIONS;
  exported["CODEX_CATALOG_PREFIX"] = keyJson(CODEX_CATALOG_PREFIX);
  exported["CODEX_CATALOG_REFRESH_LEASE_MS"] = CODEX_CATALOG_REFRESH_LEASE_MS;
  exported["CODEX_CATALOG_RETENTION_MS"] = CODEX_CATALOG_RETENTION_MS;
  exported["CODEX_CATALOG_SOURCE_CHUNK_PREFIX"] = keyJson(CODEX_CATALOG_SOURCE_CHUNK_PREFIX);
  exported["PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS"] = PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS;
  EXPECT_EQ(rt::jsonStringify(json::value(exported)), std::string(RECORDED));
  // `[...CODEX_CATALOG_PREFIX, version]` keys spread the prefix.
  const rt::KvKey versionKey{CODEX_CATALOG_PREFIX, "0.1.0"};
  EXPECT_EQ(versionKey.size(), std::size_t(3));
  EXPECT_EQ(versionKey[2].string(), "0.1.0");
}

TEST("catalog record types carry the TS fields") {
  CodexCatalogMetadata metadata{.client_version = "0.1.0",
                                .auth_generation = "auth-1",
                                .body_generation = "body-1",
                                .content_type = "application/json",
                                .etag = std::nullopt,
                                .fetched_at_ms = 1.5,
                                .chunk_count = 1,
                                .compressed_bytes = 10,
                                .body_bytes = 20,
                                .sha256 = "ab",
                                .source_chunk_count = 2};
  LoadedCodexCatalog loaded{metadata, "{}", json::object{}};
  EXPECT_EQ(loaded.metadata.client_version, "0.1.0");
  EXPECT(!loaded.source_body.has_value());
  const RefreshLease lease{"owner", 15000};
  EXPECT_EQ(lease.lease_until_ms, 15000.0);
  const double nowMs = 1000.5;
  EXPECT_EQ(nowMs + CODEX_CATALOG_REFRESH_LEASE_MS, 16000.5);
}

TEST("catalog records convert to and from JSON in declaration order") {
  const CodexCatalogMetadata metadata{.client_version = "0.1.0",
                                      .auth_generation = "auth-1",
                                      .body_generation = "body-1",
                                      .content_type = "application/json",
                                      .etag = std::nullopt,
                                      .fetched_at_ms = 1700000000000,
                                      .chunk_count = 2,
                                      .compressed_bytes = 10.5,
                                      .body_bytes = 20,
                                      .sha256 = "ab",
                                      .source_chunk_count = 1,
                                      .source_sha256 = "cd"};
  const json::value written = json::value_from(metadata);
  EXPECT_EQ(rt::jsonStringify(written),
            std::string(R"({"client_version":"0.1.0","auth_generation":"auth-1","body_generation":"body-1","content_type":"application/json","etag":null,"fetched_at_ms":1700000000000,"chunk_count":2,"compressed_bytes":10.5,"body_bytes":20,"sha256":"ab","source_chunk_count":1,"source_sha256":"cd"})"));
  EXPECT(json::value_to<CodexCatalogMetadata>(written) == metadata);
  const LoadedCodexCatalog loaded{metadata, "{}", json::object{{"models", json::array{}}}, "source"};
  EXPECT(json::value_to<LoadedCodexCatalog>(json::value_from(loaded)) == loaded);
  const RefreshLease lease{"owner", 15000};
  EXPECT_EQ(rt::jsonStringify(json::value_from(lease)), std::string(R"({"owner":"owner","lease_until_ms":15000})"));
  EXPECT(json::value_to<RefreshLease>(json::value_from(lease)) == lease);
  EXPECT_THROWS_MESSAGE(json::value_to<RefreshLease>(rt::jsonParse(R"({"owner":"o"})")), rt::TypeError, "RefreshLease.lease_until_ms is undefined, not number");
  EXPECT_THROWS_MESSAGE(json::value_to<CodexCatalogMetadata>(rt::jsonParse("[]")), rt::TypeError, "CodexCatalogMetadata is array, not object");
  json::value missingEtag = written;
  rt::objectDelete(missingEtag.as_object(), "etag");
  EXPECT_THROWS_MESSAGE(json::value_to<CodexCatalogMetadata>(missingEtag), rt::TypeError, "CodexCatalogMetadata.etag is undefined, not string or null");
}
