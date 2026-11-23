#include "uos/sentinel/replay_model.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <utility>

namespace uos::sentinel::replay_model {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    rt::KvValue textOrNull(const std::optional<std::string>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }
    rt::KvValue numberOrNull(const std::optional<double>& value) { return value ? rt::KvValue(*value) : rt::KvValue(nullptr); }

    // `[...key]` as a stored array.
  }

  rt::TextDecoder TEXT_DECODER{};

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelFailureObservation& value) {
    out = Writer()
            .required("status", value.status)
            .orNull("stream", value.stream)
            .required("completed", value.completed)
            .orNull("terminal_type", value.terminal_type)
            .orNull("failure_kind", value.failure_kind)
            .orNull("synthetic_terminal_type", value.synthetic_terminal_type)
            .required("provider_route", value.provider_route)
            .done();
  }

  SentinelFailureObservation tag_invoke(const json::value_to_tag<SentinelFailureObservation>&, const json::value& value) {
    const Reader in(value, "SentinelFailureObservation");
    return {
      .status = in.required<double>("status"),
      .stream = in.orNull<bool>("stream"),
      .completed = in.required<bool>("completed"),
      .terminal_type = in.orNull<std::string>("terminal_type"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .synthetic_terminal_type = in.orNull<std::string>("synthetic_terminal_type"),
      .provider_route = in.required<std::string>("provider_route"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelClientFailureObservation& value) {
    out = Writer()
            .required("status", value.status)
            .required("stream", value.stream)
            .required("completed", value.completed)
            .orNull("terminal_type", value.terminal_type)
            .orNull("failure_kind", value.failure_kind)
            .required("framing_valid", value.framing_valid)
            .required("provider_route", value.provider_route)
            .orNull("error_code", value.error_code)
            .orNull("error_param", value.error_param)
            .orNull("terminal_body_base64", value.terminal_body_base64)
            .required("terminal_body_truncated", value.terminal_body_truncated)
            .done();
  }

  SentinelClientFailureObservation tag_invoke(const json::value_to_tag<SentinelClientFailureObservation>&, const json::value& value) {
    const Reader in(value, "SentinelClientFailureObservation");
    return {
      .status = in.required<double>("status"),
      .stream = in.required<bool>("stream"),
      .completed = in.required<bool>("completed"),
      .terminal_type = in.orNull<std::string>("terminal_type"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .framing_valid = in.required<bool>("framing_valid"),
      .provider_route = in.required<std::string>("provider_route"),
      .error_code = in.orNull<std::string>("error_code"),
      .error_param = in.orNull<std::string>("error_param"),
      .terminal_body_base64 = in.orNull<std::string>("terminal_body_base64"),
      .terminal_body_truncated = in.required<bool>("terminal_body_truncated"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelClientBodyObservation& value) {
    out = Writer()
            .required("stream", value.stream)
            .required("completed", value.completed)
            .orNull("terminal_type", value.terminal_type)
            .orNull("failure_kind", value.failure_kind)
            .required("framing_valid", value.framing_valid)
            .nullable("error_code", value.error_code)
            .nullable("error_param", value.error_param)
            .nullable("terminal_body_base64", value.terminal_body_base64)
            .orUndefined("terminal_body_truncated", value.terminal_body_truncated)
            .done();
  }

  SentinelClientBodyObservation tag_invoke(const json::value_to_tag<SentinelClientBodyObservation>&, const json::value& value) {
    const Reader in(value, "SentinelClientBodyObservation");
    return {
      .stream = in.required<bool>("stream"),
      .completed = in.required<bool>("completed"),
      .terminal_type = in.orNull<std::string>("terminal_type"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .framing_valid = in.required<bool>("framing_valid"),
      .error_code = in.nullable<std::string>("error_code"),
      .error_param = in.nullable<std::string>("error_param"),
      .terminal_body_base64 = in.nullable<std::string>("terminal_body_base64"),
      .terminal_body_truncated = in.orUndefined<bool>("terminal_body_truncated"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplaySettings& value) {
    out = Writer()
            .required("source", value.source)
            .required("provider_route", value.provider_route)
            .orNull("model_requested", value.model_requested)
            .orNull("reasoning_requested", value.reasoning_requested)
            .orNull("stream_requested", value.stream_requested)
            .orNull("stream_observed", value.stream_observed)
            .done();
  }

  SentinelReplaySettings tag_invoke(const json::value_to_tag<SentinelReplaySettings>&, const json::value& value) {
    const Reader in(value, "SentinelReplaySettings");
    return {
      .source = in.required<std::string>("source"),
      .provider_route = in.required<std::string>("provider_route"),
      .model_requested = in.orNull<std::string>("model_requested"),
      .reasoning_requested = in.orNull<std::string>("reasoning_requested"),
      .stream_requested = in.orNull<bool>("stream_requested"),
      .stream_observed = in.orNull<bool>("stream_observed"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayDownstream& value) {
    out = Writer()
            .orNull("terminal_type", value.terminal_type)
            .orNull("failure_kind", value.failure_kind)
            .orNull("error_code", value.error_code)
            .orNull("error_param", value.error_param)
            .orNull("body_base64", value.body_base64)
            .required("body_truncated", value.body_truncated)
            .done();
  }

  SentinelReplayDownstream tag_invoke(const json::value_to_tag<SentinelReplayDownstream>&, const json::value& value) {
    const Reader in(value, "SentinelReplayDownstream");
    return {
      .terminal_type = in.orNull<std::string>("terminal_type"),
      .failure_kind = in.orNull<std::string>("failure_kind"),
      .error_code = in.orNull<std::string>("error_code"),
      .error_param = in.orNull<std::string>("error_param"),
      .body_base64 = in.orNull<std::string>("body_base64"),
      .body_truncated = in.required<bool>("body_truncated"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayManifest& value) {
    out = Writer()
            .required("version", value.version)
            .required("capture_id", value.capture_id)
            .required("fingerprint", value.fingerprint)
            .required("case_group_digest", value.case_group_digest)
            .required("captured_at_ms", value.captured_at_ms)
            .required("expires_at_ms", value.expires_at_ms)
            .required("algorithm", value.algorithm)
            .required("compression", value.compression)
            .required("iv", value.iv)
            .required("chunk_count", value.chunk_count)
            .required("ciphertext_bytes", value.ciphertext_bytes)
            .orUndefined("stored_bytes", value.stored_bytes)
            .orUndefined("request_id", value.request_id)
            .done();
  }

  SentinelReplayManifest tag_invoke(const json::value_to_tag<SentinelReplayManifest>&, const json::value& value) {
    const Reader in(value, "SentinelReplayManifest");
    return {
      .version = in.required<double>("version"),
      .capture_id = in.required<std::string>("capture_id"),
      .fingerprint = in.required<std::string>("fingerprint"),
      .case_group_digest = in.required<std::string>("case_group_digest"),
      .captured_at_ms = in.required<double>("captured_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .algorithm = in.required<std::string>("algorithm"),
      .compression = in.required<std::string>("compression"),
      .iv = in.required<std::string>("iv"),
      .chunk_count = in.required<double>("chunk_count"),
      .ciphertext_bytes = in.required<double>("ciphertext_bytes"),
      .stored_bytes = in.orUndefined<double>("stored_bytes"),
      .request_id = in.orUndefined<std::string>("request_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ExportedSentinelReplayCapture& value) {
    out = Writer().required("manifest", value.manifest).required("chunks", value.chunks).done();
  }

  ExportedSentinelReplayCapture tag_invoke(const json::value_to_tag<ExportedSentinelReplayCapture>&, const json::value& value) {
    const Reader in(value, "ExportedSentinelReplayCapture");
    return {
      .manifest = in.required<SentinelReplayManifest>("manifest"),
      .chunks = in.required<std::vector<std::string>>("chunks"),
    };
  }

  rt::KvValue toKvValue(const SentinelReplayCaptureStatusRow& row) {
    rt::KvObject object;
    object.set("version", rt::KvValue(row.version));
    object.set("request_id", rt::KvValue(row.request_id));
    object.set("status", rt::KvValue(row.status));
    object.set("reason", textOrNull(row.reason));
    object.set("captured_at_ms", rt::KvValue(row.captured_at_ms));
    object.set("manifest_key", row.manifest_key ? rt::kvValueFromKey(*row.manifest_key) : rt::KvValue(nullptr));
    object.set("fingerprint", textOrNull(row.fingerprint));
    object.set("expires_at_ms", numberOrNull(row.expires_at_ms));
    return rt::KvValue(std::move(object));
  }

  rt::Bytes cloneBytes(std::span<const std::uint8_t> value) { return rt::Bytes(value.begin(), value.end()); }

  rt::Bytes concatBytes(std::span<const rt::Bytes> parts) {
    std::size_t length = 0;
    for (const rt::Bytes& part : parts) length += part.size();
    rt::Bytes output;
    output.reserve(length);
    for (const rt::Bytes& part : parts) output.insert(output.end(), part.begin(), part.end());
    return output;
  }

  rt::Bytes concatBytes(std::initializer_list<std::span<const std::uint8_t>> parts) {
    std::size_t length = 0;
    for (const std::span<const std::uint8_t> part : parts) length += part.size();
    rt::Bytes output;
    output.reserve(length);
    for (const std::span<const std::uint8_t> part : parts) output.insert(output.end(), part.begin(), part.end());
    return output;
  }

  rt::Bytes randomBytes(double length) {
    rt::Bytes bytes(static_cast<std::size_t>(length));
    rt::crypto::getRandomValues(std::span<std::uint8_t>(bytes));
    return bytes;
  }

  rt::KvKey requestStatusKey(const std::string& requestId) { return rt::KvKey{SENTINEL_REPLAY_REQUEST_PREFIX, requestId}; }

  std::string stableHeaderText(const SentinelCompatibilityHeaders& headers) {
    std::vector<const json::key_value_pair*> entries = rt::objectEntries(headers);
    rt::sort(entries, [](const json::key_value_pair* left, const json::key_value_pair* right) {
      return static_cast<double>(rt::localeCompare(std::string_view(left->key()), std::string_view(right->key())));
    });
    std::string text;
    for (std::size_t index = 0; index < entries.size(); ++index) {
      if (index > 0) text += "\n";
      text += std::string(entries[index]->key()) + ":" + rt::toString(entries[index]->value());
    }
    return text;
  }
}
