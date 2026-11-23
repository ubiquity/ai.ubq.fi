#pragma once
// Shared Codex catalog constants and record types, split out of src/codex_catalog.ts.
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include <cstddef>
#include <optional>
#include <string>

namespace uos::catalog::types {
  inline constexpr double CODEX_CATALOG_FRESH_MS = 5 * 60'000;
  inline constexpr double CODEX_CATALOG_RETENTION_MS = 24 * 60 * 60'000;
  inline constexpr double CODEX_CATALOG_REFRESH_LEASE_MS = 15'000;
  inline constexpr double CODEX_CATALOG_COLD_WAIT_MS = 5'000;
  inline constexpr double CODEX_CATALOG_CHUNK_BYTES = 55'000;
  inline constexpr double CODEX_CATALOG_MAX_VERSIONS = 32;
  // Cap the combined served JSON size represented by the in-memory memo.
  inline constexpr std::size_t CODEX_CATALOG_MEMO_BODY_BYTES = 8 * 1024 * 1024;
  inline constexpr double PROMPT_CACHE_SCOPE_PROMOTION_LEASE_MS = 120'000;

  extern const rt::KvKey CODEX_CATALOG_AUTH_GENERATION_KEY;
  extern const rt::KvKey CODEX_CATALOG_PREFIX;
  extern const rt::KvKey CODEX_CATALOG_CHUNK_PREFIX;
  extern const rt::KvKey CODEX_CATALOG_SOURCE_CHUNK_PREFIX;
  extern const rt::KvKey CODEX_CATALOG_LEASE_PREFIX;

  struct CodexCatalogMetadata {
    std::string client_version;
    std::string auth_generation;
    std::string body_generation;
    std::string content_type;
    std::optional<std::string> etag;  // string | null
    double fetched_at_ms;
    double chunk_count;
    double compressed_bytes;
    double body_bytes;
    std::string sha256;
    std::optional<double> source_chunk_count{};
    std::optional<double> source_compressed_bytes{};
    std::optional<double> source_body_bytes{};
    std::optional<std::string> source_sha256{};
    friend bool operator==(const CodexCatalogMetadata&, const CodexCatalogMetadata&) = default;
  };

  struct LoadedCodexCatalog {
    CodexCatalogMetadata metadata;
    std::string body;
    json::object parsed;
    std::optional<std::string> source_body{};
    friend bool operator==(const LoadedCodexCatalog&, const LoadedCodexCatalog&) = default;
  };

  struct RefreshLease {
    std::string owner;
    double lease_until_ms;
    friend bool operator==(const RefreshLease&, const RefreshLease&) = default;
  };

  // JSON members in declaration order; reading is strict (rt::TypeError naming the member).
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCatalogMetadata& value);
  CodexCatalogMetadata tag_invoke(const json::value_to_tag<CodexCatalogMetadata>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const LoadedCodexCatalog& value);
  LoadedCodexCatalog tag_invoke(const json::value_to_tag<LoadedCodexCatalog>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const RefreshLease& value);
  RefreshLease tag_invoke(const json::value_to_tag<RefreshLease>&, const json::value& value);
}
