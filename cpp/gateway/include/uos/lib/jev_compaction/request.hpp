#pragma once
// Port of lib/jev_compaction/request.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `src/request.ts`. Mechanical changes:
// Deno `.ts` specifiers and repository formatting (type aliases, not
// interfaces). See ./LICENSE and ./README.md.
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <optional>
#include <string>
#include <string_view>

namespace uos::lib::jev_compaction::request {
  inline constexpr std::string_view SYSTEM_ONE_URL = "https://api.typesafe.ai/v1/systemone";
  inline constexpr std::string_view DEFAULT_MODEL = "jev-latest";

  struct JevRequest {
    std::string url;
    // "POST"
    std::string method;
    // `Record<string, string>`
    rt::OrderedMap<std::string, std::string> headers;
    std::string body;
  };

  // The `params` argument of buildJevRequest.
  struct BuildJevRequestParams {
    std::string apiKey;
    std::optional<std::string> model;
    std::optional<std::string> baseUrl;
  };

  // The HTTP request for one Jev call, for any fetch-like transport.
  JevRequest buildJevRequest(const BuildJevRequestParams& params, const types::JevState& state, const types::JevQuestions& questions);

  // Validates a Jev response body; throws on anything but an `answers` object.
  types::JevResponse parseJevResponse(double status, bool ok, std::string_view text);

  // The `noul` probability of one answer; throws when it is not there. `answers` is
  // the response's `answers` member as received (nullptr when it is undefined).
  double noulAnswer(const json::value* answers, std::string_view name);
}
