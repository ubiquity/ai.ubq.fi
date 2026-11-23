#pragma once
#include "uos/rt/kv.hpp"
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace uos::kv_conversion::cursors {
  // "deno-to-bun" | "bun-to-deno". The TS tests only for "deno-to-bun"; any other
  // text takes the bun-to-deno path, so the direction stays a string.
  using ConversionDirection = std::string;

  /** These known keys use ASCII string parts only; arbitrary database keys stay opaque. */
  std::vector<std::uint8_t> packStringKey(const std::vector<std::string>& parts);

  // { value: unknown; changed: boolean }
  struct TranscodePersistedCursorResult {
    rt::KvValue value{};
    bool changed = false;
  };

  /**
   * Translate only the two persisted opaque cursor fields; typed-key cursors are unchanged.
   * Throws rt::JsError ("Malformed replay reaper cursor", "Invalid legacy cursor", ...) and
   * the JSON.parse rt::SyntaxError / rt::TypeError of a malformed Bun cursor, as the TS does.
   */
  TranscodePersistedCursorResult transcodePersistedCursor(std::span<const std::uint8_t> key, const rt::KvValue& value, const ConversionDirection& direction);
}
