#pragma once
#include "uos/kv_conversion/typed_value.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace uos::kv_conversion::legacy_codec {
  inline constexpr std::string_view LEGACY_DENO_VERSION = "2.9.7";
  rt::Task<std::string> fileDigest(std::string path);

  // The codec `openLegacyCodec` resolves to (convert.ts: `Awaited<ReturnType<typeof
  // openLegacyCodec>>`). Copies drive the same child process.
  class LegacyCodec {
  public:
    // { version, binary, sha256 }
    struct Identity {
      std::string version;
      std::string binary;
      std::string sha256;
    };
    struct State;

    LegacyCodec(Identity identity_, std::shared_ptr<State> state);

    Identity identity;
    [[nodiscard]] rt::Task<typed_value::TypedValue> decode(std::vector<std::uint8_t> bytes) const;
    [[nodiscard]] rt::Task<std::vector<std::uint8_t>> encode(typed_value::TypedValue value) const;
    [[nodiscard]] rt::Task<void> close() const;
    [[nodiscard]] rt::Task<void> abort() const;

  private:
    std::shared_ptr<State> state_;
  };

  /** Hash pin and permissions are enforced before invoking the one-time codec child. */
  [[nodiscard]] rt::Task<LegacyCodec> openLegacyCodec(std::string binary, std::string expectedSha256, std::string cwd);
  [[nodiscard]] rt::Task<LegacyCodec> openLegacyCodec(std::string binary, std::string expectedSha256, std::string cwd, rt::AbortSignal signal);
}
