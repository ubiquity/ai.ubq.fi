#pragma once
// Port of src/responses-native-headers.ts.
#include "uos/openai_telemetry.hpp"
namespace uos::responses_native_headers {
  struct NativeHttpFailure { double status; json::object headers; bool headers_truncated; json::value error; bool body_complete; };
  void tag_invoke(const json::value_from_tag&, json::value& out, const NativeHttpFailure& value);
  NativeHttpFailure tag_invoke(const json::value_to_tag<NativeHttpFailure>&, const json::value& value);
  std::optional<NativeHttpFailure> originalNativeHttpFailure(std::optional<rt::Request> request);
  std::optional<openai_telemetry::UsageContext> nativeHeaderUsageContext(rt::Request req, const json::object& raw,
    const openai_telemetry::UsageContext* context);
  rt::Task<rt::Response> nativeFailureAfterEarlyHeaders(rt::Request req, rt::Response response, openai_telemetry::UsageContext context);
}
