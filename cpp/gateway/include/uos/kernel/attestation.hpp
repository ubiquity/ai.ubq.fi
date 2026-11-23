#pragma once
// Port of src/kernel/attestation.ts: kernel attestation verification, split out of src/auth.ts.
// The verifier keeps process-wide state like the TS module: the kernel public keys (a shared
// promise refreshed every 60 s or on demand) and the jti cache.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::kernel::attestation {
  // `getEnv(key): string | undefined`: process.env[key], undefined when reading throws.
  std::optional<std::string> getEnv(std::string_view key);

  rt::Task<void> reloadKernelPublicKeys();

  // `KernelAttestationPayload` (Readonly), the validated JWT payload verifyKernelAttestation returns.
  struct KernelAttestationPayload {
    std::string iss;  // "ubiquity-os-kernel"
    std::string aud;  // "ai.ubq.fi"
    double iat;
    double exp;
    std::string jti;
    std::string owner;
    std::string repo;
    std::optional<double> installation_id;
    std::string auth_token_sha256;
    std::string state_id;
    friend bool operator==(const KernelAttestationPayload&, const KernelAttestationPayload&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelAttestationPayload& value);
  KernelAttestationPayload tag_invoke(const json::value_to_tag<KernelAttestationPayload>&, const json::value& value);

  // `KernelAttestationFailure = { ok: false; response: Response }`
  struct KernelAttestationFailure {
    static constexpr bool ok = false;
    rt::Response response;
  };

  // `{ ok: true; payload: KernelAttestationPayload }`
  struct KernelAttestationSuccess {
    static constexpr bool ok = true;
    KernelAttestationPayload payload;
  };

  // The value verifyKernelAttestation returns: `{ ok: true; payload } | { ok: false; response }`.
  using KernelAttestationResult = std::variant<KernelAttestationSuccess, KernelAttestationFailure>;

  // The second parameter of verifyKernelAttestation: `{ token: string; owner?: string | null; repo?: string | null }`.
  struct VerifyKernelAttestationOptions {
    std::string token;
    rt::Nullable<std::string> owner{};
    rt::Nullable<std::string> repo{};
  };

  rt::Task<KernelAttestationResult> verifyKernelAttestation(rt::Request req, VerifyKernelAttestationOptions options);

  // `{ owner: string; repo: string }`: the repository getGitHubRepoHeaders reads from the request
  // and getKernelAttestationContext resolves from a verified attestation.
  struct GitHubRepo {
    std::string owner;
    std::string repo;
    friend bool operator==(const GitHubRepo&, const GitHubRepo&) = default;
  };

  // `{ owner: string; repo: string } | null`
  std::optional<GitHubRepo> getGitHubRepoHeaders(const rt::Request& req);

  // `token: string | null` (std::nullopt is null) -> `{ owner: string; repo: string } | null`
  rt::Task<std::optional<GitHubRepo>> getKernelAttestationContext(rt::Request req, std::optional<std::string> token);
}
