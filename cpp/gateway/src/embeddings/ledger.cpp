#include "uos/embeddings/ledger.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/http.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <utility>

namespace uos::embeddings::ledger {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr double EMBEDDINGS_IDEMPOTENCY_LEASE_MS = 60'000;
    constexpr double EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS = 48'000;

    // "reserved" | "dispatched" | "succeeded" | "indeterminate"
    using EmbeddingsIdempotencyState = std::string;

    struct EmbeddingsIdempotencyRecord {
      // `1`
      double v = 1;
      std::string fingerprint;
      EmbeddingsIdempotencyState state;
      std::optional<std::string> owner_request_id;
      double created_at_ms;
      double updated_at_ms;
      std::optional<double> lease_until_ms;
      std::optional<double> response_status;
      std::optional<std::string> response_content_type;
      std::optional<std::string> response_generation;
      std::optional<double> response_chunk_count;
      std::optional<std::string> response_sha256;
    };

    // The stored record, members in declaration order (the order the TS literals and spreads write).
    rt::KvValue storedRecord(const EmbeddingsIdempotencyRecord& record) {
      return rt::KvValue(Writer()
                           .required("v", record.v)
                           .required("fingerprint", record.fingerprint)
                           .required("state", record.state)
                           .orNull("owner_request_id", record.owner_request_id)
                           .required("created_at_ms", record.created_at_ms)
                           .required("updated_at_ms", record.updated_at_ms)
                           .orNull("lease_until_ms", record.lease_until_ms)
                           .orNull("response_status", record.response_status)
                           .orNull("response_content_type", record.response_content_type)
                           .orNull("response_generation", record.response_generation)
                           .orNull("response_chunk_count", record.response_chunk_count)
                           .orNull("response_sha256", record.response_sha256)
                           .done());
    }

    rt::KvKey embeddingsIdempotencyKey(std::string_view principalHash, std::string_view idempotencyKeyHash) {
      return rt::KvKey{"embeddings", "idempotency", "v1", principalHash, idempotencyKeyHash};
    }

    rt::KvKey embeddingsIdempotencyResponseKeyPrefix(std::string_view principalHash, std::string_view idempotencyKeyHash) {
      return rt::KvKey{"embeddings", "idempotency", "v1", "response", principalHash, idempotencyKeyHash};
    }

    // `isRecord` (src/utils.ts: `typeof value === "object" && value !== null`, arrays included)
    // over a stored value.

    bool isEmbeddingsIdempotencyState(const rt::KvValue& value) {
      if (!value.isString()) return false;
      const std::string& text = value.asString();
      return text == "reserved" || text == "dispatched" || text == "succeeded" || text == "indeterminate";
    }

    bool isEmbeddingsIdempotencyTimestampMs(const rt::KvValue& value) { return value.isNumber() && rt::Number::isFinite(value.asNumber()); }

    bool isEmbeddingsIdempotencyNullableTimestampMs(const rt::KvValue& value) { return value.isNull() || isEmbeddingsIdempotencyTimestampMs(value); }

    bool isEmbeddingsIdempotencyNullableString(const rt::KvValue& value) { return value.isNull() || value.isString(); }

    bool isEmbeddingsIdempotencyNullableInteger(const rt::KvValue& value) {
      return value.isNull() || (value.isNumber() && rt::Number::isInteger(value.asNumber()));
    }

    bool isEmbeddingsIdempotencyChunkCount(const rt::KvValue& value) {
      return value.isNull() || (value.isNumber() && rt::Number::isInteger(value.asNumber()) && value.asNumber() >= 1 &&
                                value.asNumber() <= EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS);
    }

    // A member the guards accepted as `string | null`.
    std::optional<std::string> nullableString(const rt::KvValue& value) {
      if (value.isNull()) return std::nullopt;
      return value.asString();
    }

    // A member the guards accepted as `number | null`, through Math.trunc.
    std::optional<double> nullableTruncated(const rt::KvValue& value) {
      if (value.isNull()) return std::nullopt;
      return rt::Math::trunc(value.asNumber());
    }

    // `value: unknown` (a stored value).
    std::optional<EmbeddingsIdempotencyRecord> normalizeEmbeddingsIdempotencyRecord(const rt::KvValue& value) {
      if (!rt::kvIsObject(value) || !(value["v"].isNumber() && value["v"].asNumber() == 1) || !value["fingerprint"].isString() ||
          !isEmbeddingsIdempotencyState(value["state"]) || !isEmbeddingsIdempotencyNullableString(value["owner_request_id"]) ||
          !isEmbeddingsIdempotencyTimestampMs(value["created_at_ms"]) || !isEmbeddingsIdempotencyTimestampMs(value["updated_at_ms"]) ||
          !isEmbeddingsIdempotencyNullableTimestampMs(value["lease_until_ms"]) || !isEmbeddingsIdempotencyNullableInteger(value["response_status"]) ||
          !isEmbeddingsIdempotencyNullableString(value["response_content_type"]) || !isEmbeddingsIdempotencyNullableString(value["response_generation"]) ||
          !isEmbeddingsIdempotencyChunkCount(value["response_chunk_count"]) || !isEmbeddingsIdempotencyNullableString(value["response_sha256"])) {
        return std::nullopt;
      }

      return EmbeddingsIdempotencyRecord{
        .v = 1,
        .fingerprint = value["fingerprint"].asString(),
        .state = value["state"].asString(),
        .owner_request_id = nullableString(value["owner_request_id"]),
        .created_at_ms = rt::Math::trunc(value["created_at_ms"].asNumber()),
        .updated_at_ms = rt::Math::trunc(value["updated_at_ms"].asNumber()),
        .lease_until_ms = nullableTruncated(value["lease_until_ms"]),
        .response_status = nullableTruncated(value["response_status"]),
        .response_content_type = nullableString(value["response_content_type"]),
        .response_generation = nullableString(value["response_generation"]),
        .response_chunk_count = nullableTruncated(value["response_chunk_count"]),
        .response_sha256 = nullableString(value["response_sha256"]),
      };
    }

    // `status: 409 | 503`; `code` is one of the embedding_idempotency_* codes; `retryAfterSeconds?: number`.
    rt::Response embeddingsIdempotencyError(double status, std::string message, std::string code, std::optional<double> retryAfterSeconds = std::nullopt) {
      http::OpenaiErrorOptions options;
      options.type = status == 503 ? "server_error" : "idempotency_error";
      options.param.emplace();  // `param: null`
      if (retryAfterSeconds) options.headers = rt::Headers{{"Retry-After", rt::numberToString(*retryAfterSeconds)}};
      return http::openaiError(status, std::move(message), std::move(code), std::move(options));
    }

    rt::Response embeddingsIdempotencyConflictResponse() {
      return embeddingsIdempotencyError(409, "Idempotency-Key was already used with a different embeddings request.", "embedding_idempotency_conflict");
    }

    rt::Response embeddingsIdempotencyInProgressResponse() {
      return embeddingsIdempotencyError(409, "The embeddings request for this Idempotency-Key is still in progress.", "embedding_idempotency_in_progress", 1);
    }

    rt::KvSetOptions ledgerSetOptions() {
      rt::KvSetOptions options;
      options.expireIn = EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS;
      return options;
    }

    // `lease: Omit<EmbeddingsIdempotencyLease, "ownerRequestId">` (the member is not read).
    rt::Task<std::optional<rt::Response>> loadEmbeddingsIdempotencyResponse(EmbeddingsIdempotencyLease lease, EmbeddingsIdempotencyRecord record) {
      if (record.state != "succeeded" || record.response_status != std::optional<double>(200) || !rt::truthy(record.response_content_type) ||
          !rt::truthy(record.response_generation) || record.response_chunk_count == std::nullopt || !rt::truthy(record.response_sha256)) {
        co_return std::nullopt;
      }

      const std::string responseGeneration = *record.response_generation;
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      for (double index = 0; index < *record.response_chunk_count; index += 1) {
        rt::KvKey chunkKey{lease.responseKeyPrefix, responseGeneration, index};
        auto read = lease.kv->get(std::move(chunkKey));
        reads.emplace_back(rt::detail::keepAlive(lease.kv, std::move(read)));
      }
      auto allReads = rt::all(std::move(reads));
      const std::vector<rt::KvEntryMaybe<>> chunks = co_await allReads.wait();
      for (const rt::KvEntryMaybe<>& entry : chunks)
        if (!entry.value.isString()) co_return std::nullopt;
      std::vector<std::string> parts;
      for (const rt::KvEntryMaybe<>& entry : chunks) parts.push_back(entry.value.asString());
      const std::string body = rt::join(parts, "");
      const std::string bodyHash = co_await utils::sha256Hex(body);
      if (bodyHash != *record.response_sha256) co_return std::nullopt;
      rt::ResponseInit init;
      init.status = *record.response_status;
      init.headers = rt::Headers{{"Content-Type", *record.response_content_type}, {"x-uos-idempotency-replayed", "true"}};
      rt::Response replay(rt::BodyInit(body), std::move(init));
      co_return replay;
    }

    EmbeddingsIdempotencyRecord buildEmbeddingsIdempotencyIndeterminateRecord(const EmbeddingsIdempotencyRecord& record, double now) {
      EmbeddingsIdempotencyRecord next = record;
      next.state = "indeterminate";
      next.owner_request_id = std::nullopt;
      next.updated_at_ms = now;
      next.lease_until_ms = std::nullopt;
      next.response_status = std::nullopt;
      next.response_content_type = std::nullopt;
      next.response_generation = std::nullopt;
      next.response_chunk_count = std::nullopt;
      next.response_sha256 = std::nullopt;
      return next;
    }

    rt::Task<bool> commitEmbeddingsIdempotencyRecordReplacement(std::shared_ptr<rt::Kv> kv, rt::KvEntryMaybe<> entry, rt::KvKey key, EmbeddingsIdempotencyRecord record) {
      auto operation = kv->atomic();
      operation->check(entry).set(key, storedRecord(record), ledgerSetOptions());
      const rt::KvCommitResult commit = co_await operation->commit();
      co_return commit.ok;
    }

    // `params: { kv; key; entry; record; lease; now }`
    struct ResolveSucceededEmbeddingsIdempotencyLeaseParams {
      std::shared_ptr<rt::Kv> kv;
      rt::KvKey key;
      rt::KvEntryMaybe<> entry;
      EmbeddingsIdempotencyRecord record;
      EmbeddingsIdempotencyLease lease;
      double now;
    };

    rt::Task<std::optional<EmbeddingsIdempotencyAcquireResult>> resolveSucceededEmbeddingsIdempotencyLease(ResolveSucceededEmbeddingsIdempotencyLeaseParams params) {
      auto loading = loadEmbeddingsIdempotencyResponse(params.lease, params.record);
      std::optional<rt::Response> replay = co_await std::move(loading);
      if (replay) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultReplay{.response = *replay};
        co_return result;
      }
      auto committing = commitEmbeddingsIdempotencyRecordReplacement(params.kv, params.entry, params.key,
                                                                     buildEmbeddingsIdempotencyIndeterminateRecord(params.record, params.now));
      const bool committed = co_await std::move(committing);
      if (committed) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyIndeterminateResponse()};
        co_return result;
      }
      co_return std::nullopt;
    }

    // `params: { kv; key; entry; record; now }`
    struct RetireDispatchedEmbeddingsIdempotencyLeaseParams {
      std::shared_ptr<rt::Kv> kv;
      rt::KvKey key;
      rt::KvEntryMaybe<> entry;
      EmbeddingsIdempotencyRecord record;
      double now;
    };

    rt::Task<std::optional<EmbeddingsIdempotencyAcquireResult>> retireDispatchedEmbeddingsIdempotencyLease(RetireDispatchedEmbeddingsIdempotencyLeaseParams params) {
      auto committing = commitEmbeddingsIdempotencyRecordReplacement(params.kv, params.entry, params.key,
                                                                     buildEmbeddingsIdempotencyIndeterminateRecord(params.record, params.now));
      const bool committed = co_await std::move(committing);
      if (committed) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyIndeterminateResponse()};
        co_return result;
      }
      co_return std::nullopt;
    }

    // `params: { kv; key; fingerprint; requestId; lease }`
    struct AttemptEmbeddingsIdempotencyLeaseAcquireParams {
      std::shared_ptr<rt::Kv> kv;
      rt::KvKey key;
      std::string fingerprint;
      std::string requestId;
      EmbeddingsIdempotencyLease lease;
    };

    rt::Task<std::optional<EmbeddingsIdempotencyAcquireResult>> attemptEmbeddingsIdempotencyLeaseAcquire(AttemptEmbeddingsIdempotencyLeaseAcquireParams params) {
      const rt::KvEntryMaybe<> entry = co_await params.kv->get(params.key);
      const std::optional<EmbeddingsIdempotencyRecord> record = normalizeEmbeddingsIdempotencyRecord(entry.value);
      const double now = rt::dateNow();

      if (entry.versionstamp == std::nullopt) {
        const EmbeddingsIdempotencyRecord reserved{
          .v = 1,
          .fingerprint = params.fingerprint,
          .state = "reserved",
          .owner_request_id = params.requestId,
          .created_at_ms = now,
          .updated_at_ms = now,
          .lease_until_ms = now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS,
          .response_status = std::nullopt,
          .response_content_type = std::nullopt,
          .response_generation = std::nullopt,
          .response_chunk_count = std::nullopt,
          .response_sha256 = std::nullopt,
        };
        auto committing = commitEmbeddingsIdempotencyRecordReplacement(params.kv, entry, params.key, reserved);
        const bool committed = co_await std::move(committing);
        if (committed) {
          std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultAcquired{.lease = params.lease};
          co_return result;
        }
        co_return std::nullopt;
      }

      if (!record) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyIndeterminateResponse()};
        co_return result;
      }
      if (record->fingerprint != params.fingerprint) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyConflictResponse()};
        co_return result;
      }

      if (record->state == "succeeded") {
        const ResolveSucceededEmbeddingsIdempotencyLeaseParams resolveParams{
          .kv = params.kv,
          .key = params.key,
          .entry = entry,
          .record = *record,
          .lease = params.lease,
          .now = now,
        };
        auto resolving = resolveSucceededEmbeddingsIdempotencyLease(resolveParams);
        std::optional<EmbeddingsIdempotencyAcquireResult> resolved = co_await std::move(resolving);
        co_return resolved;
      }

      if (record->state == "indeterminate") {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyIndeterminateResponse()};
        co_return result;
      }

      if (record->lease_until_ms != std::nullopt && *record->lease_until_ms > now) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyInProgressResponse()};
        co_return result;
      }

      if (record->state == "dispatched") {
        const RetireDispatchedEmbeddingsIdempotencyLeaseParams retireParams{
          .kv = params.kv,
          .key = params.key,
          .entry = entry,
          .record = *record,
          .now = now,
        };
        auto retiring = retireDispatchedEmbeddingsIdempotencyLease(retireParams);
        std::optional<EmbeddingsIdempotencyAcquireResult> retired = co_await std::move(retiring);
        co_return retired;
      }

      EmbeddingsIdempotencyRecord reserved = *record;
      reserved.state = "reserved";
      reserved.owner_request_id = params.requestId;
      reserved.updated_at_ms = now;
      reserved.lease_until_ms = now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS;
      reserved.response_status = std::nullopt;
      reserved.response_content_type = std::nullopt;
      reserved.response_generation = std::nullopt;
      reserved.response_chunk_count = std::nullopt;
      reserved.response_sha256 = std::nullopt;
      auto committing = commitEmbeddingsIdempotencyRecordReplacement(params.kv, entry, params.key, reserved);
      const bool committed = co_await std::move(committing);
      if (committed) {
        std::optional<EmbeddingsIdempotencyAcquireResult> result = EmbeddingsIdempotencyAcquireResultAcquired{.lease = params.lease};
        co_return result;
      }
      co_return std::nullopt;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResolvedEmbeddingsProfile& value) {
    out = Writer()
            .required("upstream", value.upstream)
            .required("upstream_model", value.upstream_model)
            .required("input_type", value.input_type)
            .required("dimensions", value.dimensions)
            .required("output_dtype", value.output_dtype)
            .required("encoding_format", value.encoding_format)
            .required("truncation", value.truncation)
            .required("cache_profile_key", value.cache_profile_key)
            .done();
  }

  ResolvedEmbeddingsProfile tag_invoke(const json::value_to_tag<ResolvedEmbeddingsProfile>&, const json::value& value) {
    const Reader in(value, "ResolvedEmbeddingsProfile");
    return {
      .upstream = in.required<std::string>("upstream"),
      .upstream_model = in.required<std::string>("upstream_model"),
      .input_type = in.required<std::string>("input_type"),
      .dimensions = in.required<double>("dimensions"),
      .output_dtype = in.required<std::string>("output_dtype"),
      .encoding_format = in.required<std::string>("encoding_format"),
      .truncation = in.required<bool>("truncation"),
      .cache_profile_key = in.required<std::string>("cache_profile_key"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ParsedEmbeddingsRequest& value) {
    out = Writer()
            .required("model", value.model)
            .required("inputs", value.inputs)
            .required("total_chars", value.total_chars)
            .required("profile", value.profile)
            .done();
  }

  ParsedEmbeddingsRequest tag_invoke(const json::value_to_tag<ParsedEmbeddingsRequest>&, const json::value& value) {
    const Reader in(value, "ParsedEmbeddingsRequest");
    return {
      .model = in.required<std::string>("model"),
      .inputs = in.required<std::vector<std::string>>("inputs"),
      .total_chars = in.required<double>("total_chars"),
      .profile = in.required<ResolvedEmbeddingsProfile>("profile"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const VoyageRateLimitState& value) {
    out = Writer().required("window_start_ms", value.window_start_ms).required("requests", value.requests).required("tokens", value.tokens).done();
  }

  VoyageRateLimitState tag_invoke(const json::value_to_tag<VoyageRateLimitState>&, const json::value& value) {
    const Reader in(value, "VoyageRateLimitState");
    return {.window_start_ms = in.required<double>("window_start_ms"), .requests = in.required<double>("requests"), .tokens = in.required<double>("tokens")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobRecord::Error& value) {
    out = Writer().required("message", value.message).required("type", value.type).orUndefined("code", value.code).done();
  }

  EmbeddingsJobRecord::Error tag_invoke(const json::value_to_tag<EmbeddingsJobRecord::Error>&, const json::value& value) {
    const Reader in(value, "EmbeddingsJobRecord.error");
    return {.message = in.required<std::string>("message"), .type = in.required<std::string>("type"), .code = in.orUndefined<std::string>("code")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobRecord& value) {
    out = Writer()
            .required("id", value.id)
            .required("status", value.status)
            .required("created_at_ms", value.created_at_ms)
            .required("updated_at_ms", value.updated_at_ms)
            .required("model", value.model)
            .required("cache_profile_key", value.cache_profile_key)
            .required("upstream", value.upstream)
            .required("upstream_model", value.upstream_model)
            .required("input_type", value.input_type)
            .required("dimensions", value.dimensions)
            .required("output_dtype", value.output_dtype)
            .required("encoding_format", value.encoding_format)
            .required("truncation", value.truncation)
            .required("input_hashes", value.input_hashes)
            .required("input_count", value.input_count)
            .required("total_chars", value.total_chars)
            .required("usage_total_tokens", value.usage_total_tokens)
            .orNull("retry_after_seconds", value.retry_after_seconds)
            .orNull("locked_until_ms", value.locked_until_ms)
            .orNull("error", value.error)
            .done();
  }

  EmbeddingsJobRecord tag_invoke(const json::value_to_tag<EmbeddingsJobRecord>&, const json::value& value) {
    const Reader in(value, "EmbeddingsJobRecord");
    return {
      .id = in.required<std::string>("id"),
      .status = in.required<std::string>("status"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
      .model = in.required<std::string>("model"),
      .cache_profile_key = in.required<std::string>("cache_profile_key"),
      .upstream = in.required<std::string>("upstream"),
      .upstream_model = in.required<std::string>("upstream_model"),
      .input_type = in.required<std::string>("input_type"),
      .dimensions = in.required<double>("dimensions"),
      .output_dtype = in.required<std::string>("output_dtype"),
      .encoding_format = in.required<std::string>("encoding_format"),
      .truncation = in.required<bool>("truncation"),
      .input_hashes = in.required<std::vector<std::string>>("input_hashes"),
      .input_count = in.required<double>("input_count"),
      .total_chars = in.required<double>("total_chars"),
      .usage_total_tokens = in.required<double>("usage_total_tokens"),
      .retry_after_seconds = in.orNull<double>("retry_after_seconds"),
      .locked_until_ms = in.orNull<double>("locked_until_ms"),
      .error = in.orNull<EmbeddingsJobRecord::Error>("error"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobInputRecord& value) {
    out = Writer().required("v", value.v).required("iv_b64", value.iv_b64).required("data_b64", value.data_b64).required("created_at_ms", value.created_at_ms).done();
  }

  EmbeddingsJobInputRecord tag_invoke(const json::value_to_tag<EmbeddingsJobInputRecord>&, const json::value& value) {
    const Reader in(value, "EmbeddingsJobInputRecord");
    return {
      .v = in.required<double>("v"),
      .iv_b64 = in.required<std::string>("iv_b64"),
      .data_b64 = in.required<std::string>("data_b64"),
      .created_at_ms = in.required<double>("created_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const EmbeddingsJobLookupRecord& value) {
    out = Writer().required("cache_profile_key", value.cache_profile_key).done();
  }

  EmbeddingsJobLookupRecord tag_invoke(const json::value_to_tag<EmbeddingsJobLookupRecord>&, const json::value& value) {
    const Reader in(value, "EmbeddingsJobLookupRecord");
    return {.cache_profile_key = in.required<std::string>("cache_profile_key")};
  }

  rt::KvKey embeddingsJobKey(std::string_view tokenHash, std::string_view cacheProfileKey, std::string_view id) {
    return rt::KvKey{"embeddings", "jobs", "v2", tokenHash, cacheProfileKey, id};
  }

  rt::KvKey embeddingsJobLookupKey(std::string_view tokenHash, std::string_view id) { return rt::KvKey{"embeddings", "jobs", "v2", "lookup", tokenHash, id}; }

  rt::KvKey embeddingsJobInputKey(std::string_view tokenHash, std::string_view cacheProfileKey, std::string_view jobId, std::string_view hash) {
    return rt::KvKey{"embeddings", "jobs", "v2", "input", tokenHash, cacheProfileKey, jobId, hash};
  }

  rt::KvKey embeddingsCacheIndexKey(std::string_view cacheProfileKey, double createdAtMs, std::string_view hash) {
    return rt::KvKey{"embeddings", "v2", "cache_index", cacheProfileKey, createdAtMs, hash};
  }

  rt::KvKey embeddingsCacheGlobalIndexKey(double createdAtMs, std::string_view cacheProfileKey, std::string_view hash) {
    return rt::KvKey{embeddingsCacheGlobalIndexPrefix, createdAtMs, cacheProfileKey, hash};
  }

  rt::KvKey embeddingsCacheIndexByHashKey(std::string_view cacheProfileKey, std::string_view hash) {
    return rt::KvKey{"embeddings", "v2", "cache_index_by_hash", cacheProfileKey, hash};
  }

  rt::KvKey embeddingsCacheKey(std::string_view cacheProfileKey, std::string_view hash) { return rt::KvKey{"embeddings", "v2", "cache", cacheProfileKey, hash}; }

  rt::Response embeddingsIdempotencyIndeterminateResponse() {
    return embeddingsIdempotencyError(409, "The embeddings request outcome is indeterminate and will not be dispatched again.", "embedding_idempotency_indeterminate");
  }

  rt::Response embeddingsIdempotencyUnavailableResponse() {
    return embeddingsIdempotencyError(503, "Idempotent embeddings requests require durable KV storage.", "embedding_idempotency_unavailable");
  }

  bool hasAsciiControlCharacter(std::string_view value) {
    const std::u16string units = rt::toUtf16(value);
    for (std::size_t index = 0; index < units.size(); index += 1) {
      const char16_t code = units[index];
      if (code <= 0x1f || code == 0x7f) return true;
    }
    return false;
  }

  rt::Task<std::string> buildEmbeddingsIdempotencyFingerprint(ResolvedEmbeddingsProfile profile, std::vector<std::string> orderedInputHashes) {
    json::array hashes;
    for (const std::string& hash : orderedInputHashes) hashes.push_back(json::value(hash));
    json::array material;
    material.push_back(json::value("uos-embeddings-idempotency-v1"));
    material.push_back(json::value(profile.upstream));
    material.push_back(json::value(profile.upstream_model));
    material.push_back(json::value(profile.input_type));
    material.push_back(rt::makeNumber(profile.dimensions));
    material.push_back(json::value(profile.output_dtype));
    material.push_back(json::value(profile.encoding_format));
    material.push_back(json::value(profile.truncation));
    material.push_back(json::value(std::move(hashes)));
    const std::string serialized = rt::jsonStringify(json::value(std::move(material)));
    const std::string fingerprint = co_await utils::sha256Hex(serialized);
    co_return fingerprint;
  }

  rt::Task<void> markEmbeddingsIdempotencyIndeterminate(EmbeddingsIdempotencyLease lease) {
    try {
      for (double attempt = 0; attempt < 4; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await lease.kv->get(lease.key);
        const std::optional<EmbeddingsIdempotencyRecord> record = normalizeEmbeddingsIdempotencyRecord(entry.value);
        if (!record || record->fingerprint != lease.fingerprint) co_return;
        if (record->state == "indeterminate" || record->state == "succeeded") co_return;
        const double now = rt::dateNow();
        EmbeddingsIdempotencyRecord next = *record;
        next.state = "indeterminate";
        next.owner_request_id = std::nullopt;
        next.updated_at_ms = now;
        next.lease_until_ms = std::nullopt;
        next.response_status = std::nullopt;
        next.response_content_type = std::nullopt;
        next.response_generation = std::nullopt;
        next.response_chunk_count = std::nullopt;
        next.response_sha256 = std::nullopt;
        auto operation = lease.kv->atomic();
        operation->check(entry).set(lease.key, storedRecord(next), ledgerSetOptions());
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return;
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] embeddings idempotency indeterminate-state write failed:", std::current_exception());
    }
  }

  rt::Task<EmbeddingsIdempotencyAcquireResult> acquireEmbeddingsIdempotencyLease(AcquireEmbeddingsIdempotencyLeaseParams params) {
    std::vector<rt::Promise<std::string>> hashing;
    hashing.push_back(rt::Promise<std::string>(utils::sha256Hex("uos-embeddings-principal-v1:" + params.principal)));
    hashing.push_back(rt::Promise<std::string>(utils::sha256Hex("uos-embeddings-key-v1:" + params.idempotencyKey)));
    const std::vector<std::string> hashes = co_await rt::all(std::move(hashing)).wait();
    const std::string& principalHash = hashes[0];
    const std::string& idempotencyKeyHash = hashes[1];
    const rt::KvKey key = embeddingsIdempotencyKey(principalHash, idempotencyKeyHash);
    const rt::KvKey responseKeyPrefix = embeddingsIdempotencyResponseKeyPrefix(principalHash, idempotencyKeyHash);
    const EmbeddingsIdempotencyLease lease{
      .kv = params.kv,
      .key = key,
      .responseKeyPrefix = responseKeyPrefix,
      .fingerprint = params.fingerprint,
      .ownerRequestId = params.requestId,
    };

    try {
      for (double attempt = 0; attempt < 8; attempt += 1) {
        const AttemptEmbeddingsIdempotencyLeaseAcquireParams attemptParams{
          .kv = params.kv,
          .key = key,
          .fingerprint = params.fingerprint,
          .requestId = params.requestId,
          .lease = lease,
        };
        auto attempting = attemptEmbeddingsIdempotencyLeaseAcquire(attemptParams);
        std::optional<EmbeddingsIdempotencyAcquireResult> result = co_await std::move(attempting);
        if (result) {
          EmbeddingsIdempotencyAcquireResult acquired = std::move(*result);
          co_return acquired;
        }
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] embeddings idempotency reservation failed:", std::current_exception());
    }

    EmbeddingsIdempotencyAcquireResult unavailable = EmbeddingsIdempotencyAcquireResultError{.response = embeddingsIdempotencyUnavailableResponse()};
    co_return unavailable;
  }

  rt::Task<bool> markEmbeddingsIdempotencyDispatched(EmbeddingsIdempotencyLease lease) {
    try {
      for (double attempt = 0; attempt < 4; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await lease.kv->get(lease.key);
        const std::optional<EmbeddingsIdempotencyRecord> record = normalizeEmbeddingsIdempotencyRecord(entry.value);
        if (!record) co_return false;
        if (record->fingerprint != lease.fingerprint || record->state != "reserved" || record->owner_request_id != lease.ownerRequestId) {
          co_return false;
        }
        const double now = rt::dateNow();
        EmbeddingsIdempotencyRecord dispatched = *record;
        dispatched.state = "dispatched";
        dispatched.updated_at_ms = now;
        dispatched.lease_until_ms = now + EMBEDDINGS_IDEMPOTENCY_LEASE_MS;
        auto operation = lease.kv->atomic();
        operation->check(entry).set(lease.key, storedRecord(dispatched), ledgerSetOptions());
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return true;
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] embeddings idempotency dispatch-state write failed:", std::current_exception());
    }
    co_return false;
  }

  rt::Task<bool> releaseEmbeddingsIdempotencyReservation(EmbeddingsIdempotencyLease lease, bool allowDispatched) {
    try {
      for (double attempt = 0; attempt < 4; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await lease.kv->get(lease.key);
        const std::optional<EmbeddingsIdempotencyRecord> record = normalizeEmbeddingsIdempotencyRecord(entry.value);
        if (!record || record->fingerprint != lease.fingerprint) co_return false;
        if (record->owner_request_id != lease.ownerRequestId) co_return false;
        if (record->state != "reserved" && !(allowDispatched && record->state == "dispatched")) co_return false;
        auto operation = lease.kv->atomic();
        operation->check(entry).delete_(lease.key);
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return true;
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] embeddings idempotency reservation release failed:", std::current_exception());
    }
    co_return false;
  }

  rt::Task<bool> storeEmbeddingsIdempotencySuccess(EmbeddingsIdempotencyLease lease, rt::Response response) {
    try {
      const rt::Response copy = response.clone();
      const std::string body = co_await copy.text();
      std::vector<std::string> chunks;
      const double bodyLength = static_cast<double>(rt::length(body));
      for (double offset = 0; offset < bodyLength; offset += EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS) {
        chunks.push_back(rt::slice(body, offset, offset + EMBEDDINGS_IDEMPOTENCY_RESPONSE_CHUNK_CHARS));
      }
      if (chunks.empty()) chunks.push_back("");
      if (static_cast<double>(chunks.size()) > EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS) co_return false;
      const std::string responseGeneration = lease.ownerRequestId;
      rt::KvSetOptions chunkOptions;
      chunkOptions.expireIn = EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS;
      for (std::size_t index = 0; index < chunks.size(); index += 1) {
        const rt::KvKey chunkKey{lease.responseKeyPrefix, responseGeneration, index};
        const rt::KvValue chunkValue(chunks[index]);
        co_await lease.kv->set(chunkKey, chunkValue, chunkOptions);
      }
      const std::string bodyHash = co_await utils::sha256Hex(body);

      for (double attempt = 0; attempt < 4; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await lease.kv->get(lease.key);
        const std::optional<EmbeddingsIdempotencyRecord> record = normalizeEmbeddingsIdempotencyRecord(entry.value);
        if (!record) co_return false;
        if (record->fingerprint != lease.fingerprint || (record->state != "reserved" && record->state != "dispatched") ||
            record->owner_request_id != lease.ownerRequestId) {
          co_return false;
        }
        const double now = rt::dateNow();
        EmbeddingsIdempotencyRecord succeeded = *record;
        succeeded.state = "succeeded";
        succeeded.owner_request_id = std::nullopt;
        succeeded.updated_at_ms = now;
        succeeded.lease_until_ms = std::nullopt;
        succeeded.response_status = static_cast<double>(response.status());
        succeeded.response_content_type = response.headers().get("Content-Type").value_or("application/json");
        succeeded.response_generation = responseGeneration;
        succeeded.response_chunk_count = static_cast<double>(chunks.size());
        succeeded.response_sha256 = bodyHash;
        auto operation = lease.kv->atomic();
        operation->check(entry).set(lease.key, storedRecord(succeeded), ledgerSetOptions());
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return true;
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] embeddings idempotency response write failed:", std::current_exception());
    }
    co_return false;
  }
}
