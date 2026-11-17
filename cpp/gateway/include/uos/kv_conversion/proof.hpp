#pragma once
// Port of src/kv-conversion/proof.ts.
/** Explicit offline acceptance tool. Ordinary Bun tests/CI never invoke or require Deno. */
#include "uos/kv_conversion/convert.hpp"
#include "uos/kv_conversion/typed_value.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <string>

namespace uos::kv_conversion::proof {
  // The object proveLegacyRoundtrip writes to proof.json and returns.
  struct LegacyRoundtripProof {
    convert::ConversionReceipt forward;
    convert::ConversionReceipt reverse;
    std::string deno_acceptance;
    typed_value::TypedValue latest_typed;
    // "no publication; source unchanged"
    std::string interruption;
    std::string retained_fixture;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const LegacyRoundtripProof& value);
  LegacyRoundtripProof tag_invoke(const json::value_to_tag<LegacyRoundtripProof>&, const json::value& value);

  // node:assert failures reject with an rt::JsError named "AssertionError" carrying Bun's message.
  [[nodiscard]] rt::Task<LegacyRoundtripProof> proveLegacyRoundtrip(std::string directory, std::string deno, std::string denoSha256);

  // The `if (import.meta.main)` block: reads rt::process::argv() (Bun.argv), runs the proof and
  // prints its summary with console.log. Its failure is the module's uncaught error (a C++ entry
  // point reports it and ends with status 1, as Bun does).
  [[nodiscard]] rt::Task<void> importMetaMain();
}
