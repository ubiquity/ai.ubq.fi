#pragma once
// Forward header of src/provider/claude-code.ts for the w04c1 import cycle: the two callback
// types that claude-code-credentials, -accounts, -models, -usage and capacity take from
// claude-code.ts, and the `{ kind, secret }` credential shape (ClaudeCodeAuthHeaders, which
// claude-code-models also uses for `Pick<ClaudeCodeResolvedCredential, "kind" | "secret">`).
// They are defined here, in claude_code's own namespace, so those headers need not include
// uos/provider/claude_code.hpp. claude_code.hpp includes this header, so the names read the same
// through either.
#include "uos/rt/fetch.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::provider::claude_code {
  // `export type ClaudeCodeFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`.
  // A `fetcher?: ClaudeCodeFetch` option is an rt::Fetch whose empty value is undefined (the
  // global fetch at call time).
  using ClaudeCodeFetch = rt::Fetch;

  /** Environment lookup seam so tests never touch process state. */
  // `(name: string) => string | null | undefined`. Every caller folds null and undefined together
  // (`env(name) ?? undefined`), so std::nullopt stands for both. An `env?: ClaudeCodeEnvLookup`
  // argument whose function is empty is undefined (the module's process.env lookup).
  using ClaudeCodeEnvLookup = std::function<std::optional<std::string>(const std::string& name)>;

  // `export type ClaudeCodeAuthHeaders = Readonly<{ kind: ClaudeCodeCredentialKind; secret: string }>`
  // (declared after ClaudeCodeResponsesResult in claude-code.ts). It is also the C++ type of
  // claude-code-models' `Pick<ClaudeCodeResolvedCredential, "kind" | "secret">`: TS passes a whole
  // resolved credential where this shape is expected, C++ callers copy the two members. `kind` is
  // claude_code_credentials::ClaudeCodeCredentialKind, an alias of std::string that this header
  // cannot name (claude_code_credentials.hpp includes it).
  struct ClaudeCodeAuthHeaders {
    std::string kind;  // ClaudeCodeCredentialKind: "api_key" | "oauth"
    std::string secret;
    friend bool operator==(const ClaudeCodeAuthHeaders&, const ClaudeCodeAuthHeaders&) = default;
  };
}
