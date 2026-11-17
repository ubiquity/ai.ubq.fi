#pragma once
// Port of src/embeddings/ledger.ts.
// Embeddings KV ledger, idempotency records and job/cache keys, extracted from src/openai.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::embeddings::ledger {
  // "float" | "base64"
  using EmbeddingsEncodingFormat = std::string;
  // "query" | "document"
  using VoyageEmbeddingsInputType = std::string;
  // 256 | 512 | 1024 | 2048
  using VoyageEmbeddingsDimension = double;
  // "float"
  using VoyageEmbeddingsOutputDtype = std::string;

  struct ResolvedEmbeddingsProfile {
    // "voyage"
    std::string upstream = "voyage";
    // "voyage-4-large"
    std::string upstream_model = "voyage-4-large";
    VoyageEmbeddingsInputType input_type;
    VoyageEmbeddingsDimension dimensions;
    VoyageEmbeddingsOutputDtype output_dtype;
    EmbeddingsEncodingFormat encoding_format;
    bool truncation;
    std::string cache_profile_key;
    friend bool operator==(const ResolvedEmbeddingsProfile&, const ResolvedEmbeddingsProfile&) = default;
  };

  // Module-private in the TS; the shape of EmbeddingsParseResult's value.
  struct ParsedEmbeddingsRequest {
    std::string model;
    std::vector<std::string> inputs;
    double total_chars;
    ResolvedEmbeddingsProfile profile;
    friend bool operator==(const ParsedEmbeddingsRequest&, const ParsedEmbeddingsRequest&) = default;
  };

  // `Readonly<{ ok: true; value: ParsedEmbeddingsRequest }> | Readonly<{ ok: false; response: Response }>`
  struct EmbeddingsParseResultOk {
    static constexpr bool ok = true;
    ParsedEmbeddingsRequest value;
  };
  struct EmbeddingsParseResultNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using EmbeddingsParseResult = std::variant<EmbeddingsParseResultOk, EmbeddingsParseResultNotOk>;

  struct VoyageRateLimitState {
    double window_start_ms;
    double requests;
    double tokens;
    friend bool operator==(const VoyageRateLimitState&, const VoyageRateLimitState&) = default;
  };

  inline constexpr double EMBEDDINGS_MAX_INPUTS_PER_REQUEST = 128;
  inline constexpr double EMBEDDINGS_MAX_CHARS_PER_INPUT = 20'000;
  inline constexpr double EMBEDDINGS_MAX_TOTAL_CHARS = 100'000;
  inline constexpr double EMBEDDINGS_TIMEOUT_MS = 20'000;
  // KV cache is best-effort and quota-driven: we cache embeddings until KV rejects
  // writes (storage/quota), then evict the oldest entries (FIFO index) and retry.
  // We do not track "last read" to keep writes minimal.
  inline constexpr double EMBEDDINGS_CACHE_EVICT_BATCH = 512;
  inline constexpr double EMBEDDINGS_CACHE_EVICT_MAX_BATCH = 8192;
  inline constexpr double EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES = 4;
  inline constexpr double EMBEDDINGS_JOB_TTL_MS = 24 * 60 * 60'000;
  inline constexpr double EMBEDDINGS_JOB_LOCK_MS = 30'000;
  inline const rt::OrderedSet<double> EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES{429, 500, 502, 503, 504};
  // 128 inputs x 2,048 finite JSON numbers fit comfortably below this cap.
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS = 256;
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS = 7 * 24 * 60 * 60'000;
  // Response chunks are published before their ledger record. Keep them for one
  // extra day so every published ledger expires before the chunks it references;
  // unpublished/orphaned generations are reclaimed by the same TTL.
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS = EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS + 24 * 60 * 60'000;
  inline constexpr double EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS = 255;

  inline constexpr std::string_view VOYAGE_EMBEDDINGS_URL = "https://api.voyageai.com/v1/embeddings";
  inline constexpr std::string_view VOYAGE_EMBEDDINGS_MODEL = "voyage-4-large";
  inline constexpr VoyageEmbeddingsDimension VOYAGE_DEFAULT_DIMENSIONS = 1024;
  inline constexpr std::string_view VOYAGE_OUTPUT_DTYPE = "float";
  inline const rt::OrderedSet<double> VOYAGE_SUPPORTED_DIMENSIONS{256, 512, 1024, 2048};
  inline const rt::OrderedSet<std::string> UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS{"dimensions", "encoding_format", "input", "input_type", "model", "truncation", "user"};
  // Jobs deliberately retain the original Voyage-only profile. In particular,
  // they require an explicit retrieval input type and only persist float vectors.
  inline const rt::OrderedSet<std::string> UOS_EMBEDDINGS_JOB_ALLOWED_KEYS{"dimensions", "encoding_format", "input", "input_type", "model", "truncation"};
  // Voyage free-tier throttles are tiny; we enforce conservative defaults to avoid 429s.
  inline constexpr double VOYAGE_RATE_LIMIT_RPM = 3;
  inline constexpr double VOYAGE_RATE_LIMIT_TPM = 10'000;
  inline const rt::KvKey VOYAGE_RATE_LIMIT_KEY{"embeddings", "v1", "rate", "voyage"};
  inline const rt::KvKey VOYAGE_API_KEY_KV_KEY{"uos_ai", "voyage_api_key"};

  struct EmbeddingsIdempotencyLease {
    // `kv: Kv` (non-null)
    std::shared_ptr<rt::Kv> kv;
    rt::KvKey key;
    rt::KvKey responseKeyPrefix;
    std::string fingerprint;
    std::string ownerRequestId;
  };

  // Module-private in the TS (the result of acquireEmbeddingsIdempotencyLease):
  // `{ kind: "acquired"; lease } | { kind: "replay"; response } | { kind: "error"; response }`
  struct EmbeddingsIdempotencyAcquireResultAcquired {
    static constexpr std::string_view kind = "acquired";
    EmbeddingsIdempotencyLease lease;
  };
  struct EmbeddingsIdempotencyAcquireResultReplay {
    static constexpr std::string_view kind = "replay";
    rt::Response response;
  };
  struct EmbeddingsIdempotencyAcquireResultError {
    static constexpr std::string_view kind = "error";
    rt::Response response;
  };
  using EmbeddingsIdempotencyAcquireResult =
      std::variant<EmbeddingsIdempotencyAcquireResultAcquired, EmbeddingsIdempotencyAcquireResultReplay, EmbeddingsIdempotencyAcquireResultError>;

  // Module-private in the TS: "queued" | "running" | "succeeded" | "failed"
  using EmbeddingsJobStatus = std::string;

  struct EmbeddingsJobRecord {
    // `{ message: string; type: string; code?: string }`
    struct Error {
      std::string message;
      std::string type;
      std::optional<std::string> code{};
      friend bool operator==(const Error&, const Error&) = default;
    };
    std::string id;
    EmbeddingsJobStatus status;
    double created_at_ms;
    double updated_at_ms;
    std::string model;
    std::string cache_profile_key;
    // "voyage"
    std::string upstream = "voyage";
    // "voyage-4-large"
    std::string upstream_model = "voyage-4-large";
    VoyageEmbeddingsInputType input_type;
    VoyageEmbeddingsDimension dimensions;
    VoyageEmbeddingsOutputDtype output_dtype;
    EmbeddingsEncodingFormat encoding_format;
    bool truncation;
    std::vector<std::string> input_hashes;
    double input_count;
    double total_chars;
    double usage_total_tokens;
    std::optional<double> retry_after_seconds;
    std::optional<double> locked_until_ms;
    std::optional<Error> error;
    friend bool operator==(const EmbeddingsJobRecord&, const EmbeddingsJobRecord&) = default;
  };

  struct EmbeddingsJobInputRecord {
    // `1`
    double v = 1;
    std::string iv_b64;
    std::string data_b64;
    double created_at_ms;
    friend bool operator==(const EmbeddingsJobInputRecord&, const EmbeddingsJobInputRecord&) = default;
  };

  struct EmbeddingsJobLookupRecord {
    std::string cache_profile_key;
    friend bool operator==(const EmbeddingsJobLookupRecord&, const EmbeddingsJobLookupRecord&) = default;
  };

  // The wire / stored records: value_from writes the members in declaration order; value_to is
  // strict (rt::TypeError) and drops undeclared members.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResolvedEmbeddingsProfile& value);
  ResolvedEmbeddingsProfile tag_invoke(const json::value_to_tag<ResolvedEmbeddingsProfile>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ParsedEmbeddingsRequest& value);
  ParsedEmbeddingsRequest tag_invoke(const json::value_to_tag<ParsedEmbeddingsRequest>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const VoyageRateLimitState& value);
  VoyageRateLimitState tag_invoke(const json::value_to_tag<VoyageRateLimitState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobRecord::Error& value);
  EmbeddingsJobRecord::Error tag_invoke(const json::value_to_tag<EmbeddingsJobRecord::Error>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobRecord& value);
  EmbeddingsJobRecord tag_invoke(const json::value_to_tag<EmbeddingsJobRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobInputRecord& value);
  EmbeddingsJobInputRecord tag_invoke(const json::value_to_tag<EmbeddingsJobInputRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobLookupRecord& value);
  EmbeddingsJobLookupRecord tag_invoke(const json::value_to_tag<EmbeddingsJobLookupRecord>&, const json::value& value);

  rt::KvKey embeddingsJobKey(std::string_view tokenHash, std::string_view cacheProfileKey, std::string_view id);
  rt::KvKey embeddingsJobLookupKey(std::string_view tokenHash, std::string_view id);
  rt::KvKey embeddingsJobInputKey(std::string_view tokenHash, std::string_view cacheProfileKey, std::string_view jobId, std::string_view hash);

  rt::KvKey embeddingsCacheIndexKey(std::string_view cacheProfileKey, double createdAtMs, std::string_view hash);
  inline const rt::KvKey embeddingsCacheGlobalIndexPrefix{"embeddings", "v2", "cache_index_global"};
  rt::KvKey embeddingsCacheGlobalIndexKey(double createdAtMs, std::string_view cacheProfileKey, std::string_view hash);
  rt::KvKey embeddingsCacheIndexByHashKey(std::string_view cacheProfileKey, std::string_view hash);
  rt::KvKey embeddingsCacheKey(std::string_view cacheProfileKey, std::string_view hash);

  rt::Response embeddingsIdempotencyIndeterminateResponse();

  rt::Response embeddingsIdempotencyUnavailableResponse();

  bool hasAsciiControlCharacter(std::string_view value);

  rt::Task<std::string> buildEmbeddingsIdempotencyFingerprint(ResolvedEmbeddingsProfile profile, std::vector<std::string> orderedInputHashes);

  rt::Task<void> markEmbeddingsIdempotencyIndeterminate(EmbeddingsIdempotencyLease lease);

  // `params: { kv: Kv; principal: string; idempotencyKey: string; fingerprint: string; requestId: string }`
  struct AcquireEmbeddingsIdempotencyLeaseParams {
    std::shared_ptr<rt::Kv> kv;
    std::string principal;
    std::string idempotencyKey;
    std::string fingerprint;
    std::string requestId;
  };

  // Bind the params to a named local and the Task before awaiting it (GCC 12 destroys aggregate
  // arguments of a coroutine inside a co_await expression twice).
  rt::Task<EmbeddingsIdempotencyAcquireResult> acquireEmbeddingsIdempotencyLease(AcquireEmbeddingsIdempotencyLeaseParams params);

  rt::Task<bool> markEmbeddingsIdempotencyDispatched(EmbeddingsIdempotencyLease lease);

  rt::Task<bool> releaseEmbeddingsIdempotencyReservation(EmbeddingsIdempotencyLease lease, bool allowDispatched);

  rt::Task<bool> storeEmbeddingsIdempotencySuccess(EmbeddingsIdempotencyLease lease, rt::Response response);
}
