#pragma once

#include "uos/handler/http.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace uos::handler::failure_journal {
  // The caller supplies a fixed route label and the gateway-generated ID, never
  // any part of the original URL, credential, response error, or request body.
  inline rt::Response withApplicationFailure(const rt::Request& request, rt::Response response, std::string_view route,
                                              const std::string& requestId, std::optional<bool> authenticated = std::nullopt,
                                              std::string_view bodyEvidence = {}) {
    if (response.status() < 400) return response;
    constexpr std::array<std::string_view, 7> methods{"GET", "HEAD", "POST", "PUT", "PATCH", "DELETE", "OPTIONS"};
    const auto method = std::find(methods.begin(), methods.end(), request.method());
    json::object event{{"request_id", requestId}, {"route", route}, {"status", response.status()},
      {"method", method == methods.end() ? "OTHER" : *method}, {"body_present", request.body().has_value()},
      {"authenticated", authenticated ? json::value(*authenticated) : json::value(nullptr)}};
    if (!bodyEvidence.empty()) event.emplace("body_evidence", bodyEvidence);
    rt::console::info("[ai.ubq.fi] request_failure", rt::jsonStringify(event));
    return http::withRequestId(response, requestId);
  }
}
