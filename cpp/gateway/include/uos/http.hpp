#pragma once
// Port of src/http.ts.
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>

namespace uos::http {
  inline constexpr std::array<std::string_view, 5> STANDARD_RATE_LIMIT_HEADERS{"RateLimit", "RateLimit-Policy", "RateLimit-Limit", "RateLimit-Remaining",
                                                                               "RateLimit-Reset"};

  // `req?: Request` is std::nullopt when undefined. The returned HeadersInit
  // object is given as the Headers it initializes.
  rt::Headers corsHeaders(std::optional<rt::Request> req = std::nullopt);

  rt::Response withCors(rt::Response response, std::optional<rt::Request> req = std::nullopt);

  rt::Response withoutBody(rt::Response response);

  // Inside uos::http this function hides the `uos::json` namespace alias: write
  // `::uos::json` there.
  rt::Response json(double status, const ::uos::json::value& body, rt::Headers extraHeaders = {});

  // The `options` of openaiError: { type?: string; param?: string | null; headers?: HeadersInit }.
  struct OpenaiErrorOptions {
    std::optional<std::string> type = std::nullopt;
    // Whether `param` is an own property, and its value: std::nullopt when the
    // property is absent; a present null or undefined value (the inner
    // std::nullopt) is written as `param: null`.
    std::optional<std::optional<std::string>> param = std::nullopt;
    std::optional<rt::Headers> headers = std::nullopt;
  };

  // `code?: string`: std::nullopt is undefined (left out of the JSON body).
  rt::Response openaiError(double status, std::string message, std::optional<std::string> code = std::nullopt, OpenaiErrorOptions options = {});

  rt::Response notFound();

  std::optional<std::string> getBearerToken(const rt::Request& req);
}
