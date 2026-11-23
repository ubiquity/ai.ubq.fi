#pragma once
// Port of lib/jev_compaction/client.ts.
//
// Ported from 0x4007/fast-jev-compaction (MIT), commit
// c1eab5fdbd6bde7d67f2da070116496558836884, `src/client.ts`. Mechanical changes:
// Deno `.ts` specifiers, repository formatting, credential injected by the
// gateway instead of read from `process.env`, and an optional caller signal
// forwarded to the transport so cancellation stops the Jev call. See ./LICENSE.
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include <optional>
#include <string>

namespace uos::lib::jev_compaction::client {
  // `(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using JevFetch = rt::Fetch;

  struct JevClientOptions {
    // Resolved by the caller; an absent key fails the call explicitly.
    std::string apiKey;
    // Defaults to `jev-latest`.
    std::optional<std::string> model;
    // Defaults to the System One endpoint.
    std::optional<std::string> baseUrl;
    // Defaults to the global `fetch` (an empty JevFetch is undefined).
    JevFetch fetch;
    // Caller-owned abort signal, forwarded to the underlying request.
    std::optional<rt::AbortSignal> signal;
  };

  // Asks Jev over HTTP with the global `fetch` (or an injected one). The object must
  // outlive every ask() it starts (hold it in a std::shared_ptr, as a JevAsker).
  class JevClient : public types::JevAsker {
  public:
    explicit JevClient(const JevClientOptions& options);
    [[nodiscard]] rt::Task<types::JevResponse> ask(types::JevState state, types::JevQuestions questions) override;

  private:
    std::string apiKey;
    std::optional<std::string> model;
    std::optional<std::string> baseUrl;
    JevFetch fetcher;
    std::optional<rt::AbortSignal> signal;
  };
}
