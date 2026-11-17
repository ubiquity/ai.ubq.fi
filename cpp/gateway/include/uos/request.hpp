#pragma once
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <variant>

namespace uos::request {
  // `RawBodyObserver` and `RawBodyRejection` are module-private in the TS; they are the
  // parameter types of observeRawBodyOnce. The observer receives the route's own buffer
  // (a JS Uint8Array reference), so a capture that keeps it sees later zeroing.
  using RawBodyObserver = std::function<void(std::shared_ptr<rt::Bytes> bytes)>;
  /** Reports why an accepted body never reached its observer, bounded to the fixed reasons. */
  using RawBodyRejection = std::function<void(const std::string& reason)>;  // "body_over_limit" | "body_unavailable"

  inline constexpr std::int64_t MAX_ACCEPTED_JSON_BODY_BYTES = 32 * 1'024 * 1'024;

  struct JsonBodyReadResultOk {
    static constexpr bool ok = true;
    json::value value;
    friend bool operator==(const JsonBodyReadResultOk&, const JsonBodyReadResultOk&) = default;
  };
  struct JsonBodyReadResultNotOk {
    static constexpr bool ok = false;
    std::string kind;  // "empty" | "invalid" | "too_large"
    friend bool operator==(const JsonBodyReadResultNotOk&, const JsonBodyReadResultNotOk&) = default;
  };
  using JsonBodyReadResult = std::variant<JsonBodyReadResultOk, JsonBodyReadResultNotOk>;

  // An empty onRejected is an omitted (undefined) callback.
  void observeRawBodyOnce(const rt::Request& req, RawBodyObserver observer, RawBodyRejection onRejected = {});

  /** Transfer one accepted raw body to its observer under the fixed replay cap. */
  bool captureRawBodyOnce(const rt::Request& req, const std::shared_ptr<rt::Bytes>& bytes);

  void discardRawBodyObserverOnce(const rt::Request& req);

  /**
   * Report that the accepted body never reached its observer because the route
   * read itself stopped at the fixed cap. Returns whether an observer was
   * waiting, so a non-inference read stays a no-op.
   */
  bool rejectRawBodyOnce(const rt::Request& req, const std::string& reason);

  // The body limit counts wire bytes. The document limit counts decoded JSON bytes.
  [[nodiscard]] rt::Task<JsonBodyReadResult> readJsonBodyWithLimit(
      rt::Request req, double maxBodyBytes, std::size_t maxDocumentBytes = rt::limits::kJsonDocumentBytes);

  [[nodiscard]] rt::Task<json::value> readJsonBody(
      rt::Request req, double maxBodyBytes = rt::limits::kHttpBodyBytes, std::size_t maxDocumentBytes = rt::limits::kJsonDocumentBytes);
}
