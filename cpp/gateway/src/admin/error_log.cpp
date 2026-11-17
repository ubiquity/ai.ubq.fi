#include "uos/admin/error_log.hpp"

#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_retention.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <variant>

namespace uos::admin::error_log {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using sentinel::replay_retention_schema::SentinelReplayRetentionStatus;

    constexpr double DEFAULT_LIMIT = 100;
    constexpr double MAX_LIMIT = 500;

    // `bounded(value, fallback, maximum = 160)`: `value?.trim()`, then the first `maximum` code units or the fallback.
    std::string bounded(const std::optional<std::string>& value, const std::string& fallback, double maximum = 160) {
      if (!value) return fallback;
      const std::string normalized = rt::trim(*value);
      return !normalized.empty() ? rt::slice(normalized, 0, maximum) : fallback;
    }

    bool is(const std::optional<std::string>& value, std::string_view text) { return value && *value == text; }

    rt::KvValue textOrNull(const std::optional<std::string>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminErrorLogRecord& value) {
    out = Writer()
            .required("version", value.version)
            .required("request_id", value.request_id)
            .required("route", value.route)
            .required("status", value.status)
            .required("provider", value.provider)
            .orNull("model", value.model)
            .orNull("reasoning", value.reasoning)
            .orNull("stream", value.stream)
            .required("terminal_type", value.terminal_type)
            .required("failure_kind", value.failure_kind)
            .required("delivery_outcome", value.delivery_outcome)
            .required("created_at_ms", value.created_at_ms)
            .required("latency_ms", value.latency_ms)
            .orNull("git_sha", value.git_sha)
            .orNull("deno_revision", value.deno_revision)
            .done();
  }

  AdminErrorLogRecord tag_invoke(const json::value_to_tag<AdminErrorLogRecord>&, const json::value& value) {
    const Reader in(value, "AdminErrorLogRecord");
    return {
      .version = in.required<double>("version"),
      .request_id = in.required<std::string>("request_id"),
      .route = in.required<std::string>("route"),
      .status = in.required<double>("status"),
      .provider = in.required<std::string>("provider"),
      .model = in.orNull<std::string>("model"),
      .reasoning = in.orNull<std::string>("reasoning"),
      .stream = in.orNull<bool>("stream"),
      .terminal_type = in.required<std::string>("terminal_type"),
      .failure_kind = in.required<std::string>("failure_kind"),
      .delivery_outcome = in.required<std::string>("delivery_outcome"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .latency_ms = in.required<double>("latency_ms"),
      .git_sha = in.orNull<std::string>("git_sha"),
      .deno_revision = in.orNull<std::string>("deno_revision"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminLogFile& value) {
    out = Writer().required("path", value.path).orNull("bytes", value.bytes).done();
  }

  AdminLogFile tag_invoke(const json::value_to_tag<AdminLogFile>&, const json::value& value) {
    const Reader in(value, "AdminLogFile");
    return {.path = in.required<std::string>("path"), .bytes = in.orNull<double>("bytes")};
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminLogFileSizes& value) {
    out = Writer()
            .required("scope", value.scope)
            .required("platform", value.platform)
            .required("files", value.files)
            .orNull("total_bytes", value.total_bytes)
            .required("warning", value.warning)
            .done();
  }

  AdminLogFileSizes tag_invoke(const json::value_to_tag<AdminLogFileSizes>&, const json::value& value) {
    const Reader in(value, "AdminLogFileSizes");
    return {
      .scope = in.required<std::string>("scope"),
      .platform = in.required<std::string>("platform"),
      .files = in.required<std::vector<AdminLogFile>>("files"),
      .total_bytes = in.orNull<double>("total_bytes"),
      .warning = in.required<bool>("warning"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const AdminErrorBucket& value) {
    out = Writer().required("bucket_start_at_ms", value.bucket_start_at_ms).required("count", value.count).done();
  }

  AdminErrorBucket tag_invoke(const json::value_to_tag<AdminErrorBucket>&, const json::value& value) {
    const Reader in(value, "AdminErrorBucket");
    return {.bucket_start_at_ms = in.required<double>("bucket_start_at_ms"), .count = in.required<double>("count")};
  }

  namespace {
    rt::Task<void> recordAdminErrorTask(AdminErrorInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      // Authentication failures are not inference failures and must retain the
      // request path's zero-KV budget.
      if (input.status == 401 && input.provider == "gateway") co_return;
      const bool failed = input.status >= 400 || is(input.terminal_type, "error") || is(input.terminal_type, "eof") || is(input.terminal_type, "deadline") ||
                          is(input.terminal_type, "response.failed") || is(input.terminal_type, "response.incomplete") || input.failure_kind.has_value();
      if (!failed) co_return;

      std::shared_ptr<rt::Kv> kv;
      if (!kvOverride) {
        auto opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *kvOverride;
      }
      if (!kv) co_return;
      const std::string terminalType = bounded(input.terminal_type, input.status >= 400 ? "http.error" : "error");
      const std::string failureKind = bounded(input.failure_kind, input.status >= 400 ? "http_" + rt::numberToString(input.status) : terminalType);
      // `{ ...input, version: 1, terminal_type: terminalType, failure_kind: failureKind }`: the input's members in
      // its order, `version` appended.
      rt::KvObject record;
      record.set("request_id", rt::KvValue(input.request_id));
      record.set("route", rt::KvValue(input.route));
      record.set("status", rt::KvValue(input.status));
      record.set("provider", rt::KvValue(input.provider));
      record.set("model", textOrNull(input.model));
      record.set("reasoning", textOrNull(input.reasoning));
      record.set("stream", input.stream ? rt::KvValue(*input.stream) : rt::KvValue(nullptr));
      record.set("terminal_type", rt::KvValue(terminalType));
      record.set("failure_kind", rt::KvValue(failureKind));
      record.set("delivery_outcome", rt::KvValue(input.delivery_outcome));
      record.set("created_at_ms", rt::KvValue(input.created_at_ms));
      record.set("latency_ms", rt::KvValue(input.latency_ms));
      record.set("git_sha", textOrNull(input.git_sha));
      record.set("deno_revision", textOrNull(input.deno_revision));
      record.set("version", rt::KvValue(1));
      const rt::KvKey key{ADMIN_ERROR_LOG_PREFIX, input.created_at_ms, input.request_id};
      const rt::KvValue value(std::move(record));
      rt::KvSetOptions options;
      options.expireIn = ADMIN_ERROR_LOG_TTL_MS;
      co_await kv->set(key, value, options);
    }
  }

  rt::Task<void> recordAdminError(const AdminErrorInput& input) { return recordAdminErrorTask(input, std::nullopt); }

  rt::Task<void> recordAdminError(const AdminErrorInput& input, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    return recordAdminErrorTask(input, std::move(kvOverride));
  }

  namespace {
    // `typeof value === "object" && value !== null && !Array.isArray(value)`
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() ||
               value.is<rt::KvHole>() || value.isArray());
    }
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
  }

  bool isAdminErrorLogRecord(const rt::KvValue& value) {
    if (!isRecord(value)) return false;
    const rt::KvValue& deliveryOutcome = value["delivery_outcome"];
    return value["version"].isNumber() && value["version"].asNumber() == 1 && value["request_id"].isString() && value["route"].isString() &&
           value["status"].isNumber() && value["provider"].isString() && (value["model"].isNull() || value["model"].isString()) &&
           (value["reasoning"].isNull() || value["reasoning"].isString()) && (value["stream"].isNull() || value["stream"].isBool()) &&
           value["terminal_type"].isString() && value["failure_kind"].isString() &&
           (isText(deliveryOutcome, "delivered") || isText(deliveryOutcome, "interrupted") || isText(deliveryOutcome, "unobserved")) &&
           value["created_at_ms"].isNumber() && value["latency_ms"].isNumber() && (value["git_sha"].isNull() || value["git_sha"].isString()) &&
           (value["deno_revision"].isNull() || value["deno_revision"].isString());
  }

  namespace {
    // These deployments let partial-body framing override an authoritative
    // downstream cancellation. The retained rows cannot otherwise be separated
    // from genuine premature EOFs, so limit suppression to the affected code.
    const std::unordered_set<std::string_view>& LEGACY_INTERRUPTED_MISSING_SSE_TERMINAL_GIT_SHAS() {
      static const std::unordered_set<std::string_view> shas{
        "4d2928f47f3ba3ad66b8c15e6323bcccd0a39390",
        "0d795e28e42be63bbd7f0d4ce44d8ea0f6ab9d4a",
        "149ef399a52373ba09d75cfd00d5e5139564dcdf",
        "8532ec7ee1fc5a6a9bb44fe2a3703527b8d78952",
        "35fb0782c01309a85ea08f5a6a48f8de62a1f29f",
        "c18a4e091c09c7a9e04588a8fe745c696deaf6d2",
        "214957fe322c8cff6ad20ddea55dcf9576273107",
        "50c06cd8e93fe3b34a9699d31f1f8998f632fbe6",
        "64f4ba5a21e476386f95c8474bd76b037e6afc4f",
        "7fb508a076e9edf04c6e4087a19ad6e25452dc6b",
        "c9bcf7d5937cf137bc34d8656187d3f20e9f8b4d",
        "9d3b7d0eb8a1c57079d298b17d50d25f74dd39d9",
        "542df9ef4560ca9c090925a5a1b24a141a584867",
        "ce37210d58746a2ed3aec34c38b370f7060639e1",
        "f9dec6c3b6270813be7b4a957221f7130c37a52b",
      };
      return shas;
    }

    // `record`: a value isAdminErrorLogRecord accepted.
    bool isLegacyInterruptedMissingSseTerminal(const rt::KvValue& record) {
      const rt::KvValue& stream = record["stream"];
      const rt::KvValue& gitSha = record["git_sha"];
      return record["status"].asNumber() == 200 && stream.isBool() && stream.asBool() && isText(record["terminal_type"], "error") &&
             isText(record["failure_kind"], "missing_sse_terminal") && isText(record["delivery_outcome"], "interrupted") && !gitSha.isNull() &&
             LEGACY_INTERRUPTED_MISSING_SSE_TERMINAL_GIT_SHAS().contains(gitSha.asString());
    }

    const std::array<std::string_view, 2> MAC_TEXT_LOG_PATHS{"/Users/nv/repos/ubiquity/ai.ubq.fi/.data/mac.stdout.log",
                                                             "/Users/nv/repos/ubiquity/ai.ubq.fi/.data/mac.stderr.log"};

    SentinelReplayRetentionStatus unavailableRetention() {
      return {
        .state = "unavailable",
        .scope = "capture_owned_kv_payload",
        .budget_bytes = std::nullopt,
        .stored_bytes = std::nullopt,
        .reserved_bytes = std::nullopt,
        .records = std::nullopt,
        .metadata_bytes = std::nullopt,
        .status_records = std::nullopt,
        .evicted_records = std::nullopt,
        .evicted_bytes = std::nullopt,
        .expired_records = std::nullopt,
        .last_eviction_at_ms = std::nullopt,
        .last_warning_at_ms = std::nullopt,
        .over_budget = false,
        .near_capacity = false,
        .accounting_complete = false,
        .accounting_error = "unavailable",
        .skipped_reason = std::nullopt,
      };
    }

    rt::Task<AdminLogFileSizes> readAdminLogFileSizes() {
      std::vector<std::string> paths;
      if (rt::process::platform == "darwin") paths.assign(MAC_TEXT_LOG_PATHS.begin(), MAC_TEXT_LOG_PATHS.end());
      std::vector<AdminLogFile> files;
      // No measured paths (e.g. journald on Linux) stays unknown, never zero.
      std::optional<double> total = paths.empty() ? std::nullopt : std::optional<double>(0);
      for (const std::string& path : paths) {
        // `await stat(path)`: node:fs/promises is a synchronous core here.
        std::optional<double> size;
        try {
          size = static_cast<double>(rt::fs::promises::stat(path).size);
        } catch (...) {
          size = std::nullopt;
        }
        co_await rt::microtask();
        if (size) {
          files.push_back(AdminLogFile{.path = path, .bytes = size});
          if (total) *total += files.back().bytes.value_or(0);
        } else {
          files.push_back(AdminLogFile{.path = path, .bytes = std::nullopt});
          total = std::nullopt;
        }
      }
      co_return AdminLogFileSizes{
        .scope = "host_text_logs",
        .platform = std::string(rt::process::platform),
        .files = std::move(files),
        .total_bytes = total,
        .warning = total && *total >= sentinel::replay_limits::SENTINEL_REPLAY_BUDGET_BYTES,
      };
    }

    rt::Task<AdminErrorHistoryScan> listAdminErrorHistoryTask(double limit, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      std::shared_ptr<rt::Kv> kv;
      if (!kvOverride) {
        auto opening = ::uos::kv::getKv();
        kv = co_await opening.wait();
      } else {
        kv = *kvOverride;
      }
      if (!kv) co_return AdminErrorHistoryScan{.data = {}, .five_xx_buckets = {}};
      const double boundedLimit = rt::Math::max(1, rt::Math::min(MAX_LIMIT, rt::Math::trunc(limit)));
      std::vector<rt::KvValue> data;
      rt::OrderedMap<double, double> fiveXxCounts;
      rt::KvListSelector selector;
      selector.prefix = ADMIN_ERROR_LOG_PREFIX;
      rt::KvListOptions options;
      options.reverse = true;
      options.batchSize = MAX_LIMIT;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(std::move(selector), std::move(options));
      for (;;) {
        const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        if (!isAdminErrorLogRecord(entry->value)) continue;
        const rt::KvValue& record = entry->value;
        const double status = record["status"].asNumber();
        if (status >= 500 && status <= 599) {
          const double bucketStartAtMs = rt::Math::floor(record["created_at_ms"].asNumber() / ADMIN_ERROR_BUCKET_MS) * ADMIN_ERROR_BUCKET_MS;
          const double* counted = fiveXxCounts.get(bucketStartAtMs);
          fiveXxCounts.set(bucketStartAtMs, (counted ? *counted : 0) + 1);
        }
        if (static_cast<double>(data.size()) < boundedLimit && !isLegacyInterruptedMissingSseTerminal(record)) data.push_back(record);
      }
      std::vector<AdminErrorBucket> buckets;
      for (const auto& [bucketStartAtMs, count] : fiveXxCounts) buckets.push_back(AdminErrorBucket{.bucket_start_at_ms = bucketStartAtMs, .count = count});
      rt::sort(buckets, [](const AdminErrorBucket& left, const AdminErrorBucket& right) { return left.bucket_start_at_ms - right.bucket_start_at_ms; });
      co_return AdminErrorHistoryScan{.data = std::move(data), .five_xx_buckets = std::move(buckets)};
    }
  }

  rt::Task<AdminErrorHistoryScan> listAdminErrorHistory() { return listAdminErrorHistoryTask(DEFAULT_LIMIT, std::nullopt); }

  rt::Task<AdminErrorHistoryScan> listAdminErrorHistory(double limit) { return listAdminErrorHistoryTask(limit, std::nullopt); }

  rt::Task<AdminErrorHistoryScan> listAdminErrorHistory(double limit, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    return listAdminErrorHistoryTask(limit, std::move(kvOverride));
  }

  namespace {
    // JSON.stringify's view of a stored value (what the JSON response writes): undefined members are
    // left out and undefined array elements are null; NaN and the infinities are null; a Date is its
    // toJSON(); a Uint8Array or typed array is an object of its index members; ArrayBuffer, DataView,
    // RegExp, Error, Map and Set have no enumerable members; a BigInt throws. std::nullopt is undefined.
    [[noreturn]] void throwBigInt() { throw rt::TypeError("JSON.stringify cannot serialize BigInt."); }

    template <class T> json::value typedArrayJson(const rt::KvArrayBufferView& view) {
      const std::uint64_t length = view.byteLength / sizeof(T);
      json::object out;
      for (std::uint64_t index = 0; index < length; ++index) {
        T element{};
        std::memcpy(&element, view.buffer.data() + view.byteOffset + index * sizeof(T), sizeof(T));
        const double number = static_cast<double>(element);
        out[std::to_string(index)] = rt::Number::isFinite(number) ? rt::makeNumber(number) : json::value(nullptr);
      }
      return out;
    }

    json::value viewJson(const rt::KvArrayBufferView& view) {
      switch (view.kind) {
        case rt::KvViewKind::DataView:
          return json::object();
        case rt::KvViewKind::Int8Array:
          return typedArrayJson<std::int8_t>(view);
        case rt::KvViewKind::Uint8Array:
        case rt::KvViewKind::Uint8ClampedArray:
          return typedArrayJson<std::uint8_t>(view);
        case rt::KvViewKind::Int16Array:
          return typedArrayJson<std::int16_t>(view);
        case rt::KvViewKind::Uint16Array:
          return typedArrayJson<std::uint16_t>(view);
        case rt::KvViewKind::Int32Array:
          return typedArrayJson<std::int32_t>(view);
        case rt::KvViewKind::Uint32Array:
          return typedArrayJson<std::uint32_t>(view);
        case rt::KvViewKind::Float32Array:
          return typedArrayJson<float>(view);
        case rt::KvViewKind::Float64Array:
          return typedArrayJson<double>(view);
        case rt::KvViewKind::BigInt64Array:
        case rt::KvViewKind::BigUint64Array:
          if (view.byteLength >= 8) throwBigInt();
          return json::object();
      }
      return json::object();
    }

    std::optional<json::value> storedJson(const rt::KvValue& value, std::size_t depth) {
      if (depth > rt::kJsonMaxDepth) throw rt::RangeError("Maximum call stack size exceeded.");
      return std::visit(
        [depth](const auto& item) -> std::optional<json::value> {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, rt::KvUndefined> || std::is_same_v<T, rt::KvHole>) return std::nullopt;
          else if constexpr (std::is_same_v<T, std::nullptr_t>) return json::value(nullptr);
          else if constexpr (std::is_same_v<T, bool>) return json::value(item);
          else if constexpr (std::is_same_v<T, double>) return rt::Number::isFinite(item) ? rt::makeNumber(item) : json::value(nullptr);
          else if constexpr (std::is_same_v<T, std::string>) return json::value(json::string(item));
          else if constexpr (std::is_same_v<T, rt::BigInt> || std::is_same_v<T, rt::KvU64>) throwBigInt();
          else if constexpr (std::is_same_v<T, rt::KvBytes>) {
            json::object out;
            for (std::size_t index = 0; index < item.size(); ++index) out[std::to_string(index)] = rt::makeNumber(static_cast<std::int64_t>(item[index]));
            return json::value(std::move(out));
          } else if constexpr (std::is_same_v<T, rt::KvArrayBufferView>) return viewJson(item);
          else if constexpr (std::is_same_v<T, rt::KvDate>) {
            const std::optional<std::string> iso = rt::Date(item.time).toJSON();
            return iso ? json::value(json::string(*iso)) : json::value(nullptr);
          } else if constexpr (std::is_same_v<T, rt::KvArray>) {
            json::array out;
            for (const rt::KvValue& element : item) out.push_back(storedJson(element, depth + 1).value_or(nullptr));
            return json::value(std::move(out));
          } else if constexpr (std::is_same_v<T, rt::KvObject>) {
            json::object out;
            for (const auto& [key, member] : item) {
              std::optional<json::value> converted = storedJson(member, depth + 1);
              if (converted) out[key] = std::move(*converted);
            }
            return json::value(std::move(out));
          } else return json::value(json::object());  // ArrayBuffer, RegExp, Error, Map, Set
        },
        value.variant());
    }

    rt::Task<rt::Response> handleAdminErrorsTask(rt::Request req) {
      const rt::URL url(req.url());
      const std::optional<std::string> rawLimit = url.searchParams().get("limit");
      const double limit = !rawLimit ? DEFAULT_LIMIT : rt::stringToNumber(*rawLimit);
      if (!rt::Number::isSafeInteger(limit) || limit < 1 || limit > MAX_LIMIT) {
        co_return http::openaiError(400, "limit must be an integer from 1 to " + rt::numberToString(MAX_LIMIT), std::string("invalid_request_error"));
      }
      auto opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return http::openaiError(503, "Error history storage is unavailable", std::string("server_error"));
      auto listing = listAdminErrorHistory(limit, kv);
      const AdminErrorHistoryScan history = co_await std::move(listing);
      // One bounded maintenance pass keeps the reported capture usage current and
      // lets cleanup continue without a background daemon. It runs here, after the
      // single bounded history scan, so it can never widen that scan.
      std::optional<SentinelReplayRetentionStatus> retention;
      try {
        const sentinel::replay_retention::SentinelReplayMaintenanceOptions options{.now_ms = rt::dateNow()};
        auto maintaining = sentinel::replay_retention::runSentinelReplayRetentionMaintenance(*kv, options);
        retention = co_await std::move(maintaining);
      } catch (...) {
        retention = unavailableRetention();
      }
      auto reading = readAdminLogFileSizes();
      const AdminLogFileSizes logFiles = co_await std::move(reading);
      // `{ object: "list", ...history, retention, log_files: logFiles }`
      json::array data;
      for (const rt::KvValue& record : history.data) data.push_back(storedJson(record, 0).value_or(nullptr));
      json::object payload;
      payload["object"] = "list";
      payload["data"] = std::move(data);
      payload["five_xx_buckets"] = json::value_from(history.five_xx_buckets);
      payload["retention"] = json::value_from(*retention);
      payload["log_files"] = json::value_from(logFiles);
      co_return http::json(200, json::value(std::move(payload)));
    }
  }

  rt::Task<rt::Response> handleAdminErrors(rt::Request req) { return handleAdminErrorsTask(std::move(req)); }
}
