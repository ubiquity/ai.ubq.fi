#include "uos/request.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json_document.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <exception>
#include <optional>
#include <utility>
#include <vector>

namespace uos::request {
  namespace {
    struct RawBodyObservation {
      RawBodyObserver observer;
      RawBodyRejection onRejected;
      RawBodyStage stage;
    };

    rt::WeakMap<rt::Request, RawBodyObservation> rawBodyObservers;

    class RequestBodyTooLargeError : public rt::JsError {
    public:
      RequestBodyTooLargeError() : rt::JsError("Request body is too large", "RequestBodyTooLargeError") {}
    };

    bool isRequestBodyTooLargeError(const rt::Reason& error) {
      try {
        std::rethrow_exception(error);
      } catch (const RequestBodyTooLargeError&) {
        return true;
      } catch (...) {
        return false;
      }
    }
  }

  void observeRawBodyOnce(const rt::Request& req, RawBodyObserver observer, RawBodyRejection onRejected, RawBodyStage stage) {
    rawBodyObservers.set(req, RawBodyObservation{std::move(observer), std::move(onRejected), stage});
  }

  RawBodyRejection receivedRawBodyRejection(const rt::Request& req) {
    const auto pending = rawBodyObservers.get(req);
    return pending && pending->stage == RawBodyStage::received ? pending->onRejected : RawBodyRejection{};
  }

  bool captureRawBodyOnce(const rt::Request& req, const std::shared_ptr<rt::Bytes>& bytes, RawBodyStage stage) {
    const std::optional<RawBodyObservation> pending = rawBodyObservers.get(req);
    if (!pending || (stage == RawBodyStage::received && pending->stage != stage)) return false;
    rawBodyObservers.delete_(req);
    if (bytes->size() > static_cast<std::size_t>(MAX_ACCEPTED_JSON_BODY_BYTES)) {
      // The route accepted this body for inference, but the fixed replay storage
      // contract cannot carry it: report the omission instead of staying silent.
      if (pending->onRejected) pending->onRejected("body_over_limit");
      return false;
    }
    pending->observer(bytes);
    return true;
  }

  void discardRawBodyObserverOnce(const rt::Request& req) { rawBodyObservers.delete_(req); }

  bool rejectRawBodyOnce(const rt::Request& req, const std::string& reason) {
    const std::optional<RawBodyObservation> pending = rawBodyObservers.get(req);
    rawBodyObservers.delete_(req);
    if (!pending) return false;
    if (pending->onRejected) pending->onRejected(reason);
    return true;
  }

  namespace {
    std::optional<double> declaredContentLength(const rt::Request& req) {
      const std::optional<std::string> raw = req.headers().get("content-length");
      if (!raw) return std::nullopt;
      static const rt::Regex DECIMAL("^(?:0|[1-9]\\d*)$");
      if (!DECIMAL.test(*raw)) throw rt::JsError("Request Content-Length is invalid");
      const double parsed = rt::stringToNumber(*raw);
      if (!rt::Number::isSafeInteger(parsed)) throw rt::JsError("Request Content-Length is invalid");
      return parsed;
    }

    rt::Task<std::shared_ptr<rt::Bytes>> readBoundedRequestBody(rt::Request req, double maxBytes) {
      const std::optional<double> declared = declaredContentLength(req);
      if (declared && *declared > maxBytes) {
        if (std::optional<rt::ByteStream> body = req.body()) {
          rt::Promise<void> cancelled = rt::Promise<void>(body->cancel()).catch_(rt::ignoreRejection);
          co_await cancelled.wait();
        } else {
          co_await rt::microtask();
        }
        throw RequestBodyTooLargeError();
      }
      std::optional<rt::ByteStream> body = req.body();
      if (!body) {
        if (declared.value_or(0) > 0 && receivedRawBodyRejection(req)) rejectRawBodyOnce(req, "body_unavailable");
        co_return std::make_shared<rt::Bytes>();
      }
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      std::vector<rt::Bytes> chunks;
      double total = 0;
      std::shared_ptr<rt::Bytes> bytes;
      rt::Reason error;
      try {
        for (;;) {
          rt::ReadableStreamReadResult<rt::ByteChunk> step = co_await reader.read();
          if (step.done) break;
          rt::Bytes& value = *step.value;
          if (static_cast<double>(value.size()) > maxBytes - total) {
            rt::Promise<void> cancelled = rt::Promise<void>(reader.cancel()).catch_(rt::ignoreRejection);
            co_await cancelled.wait();
            throw RequestBodyTooLargeError();
          }
          total += static_cast<double>(value.size());
          chunks.push_back(std::move(value));
        }
        bytes = std::make_shared<rt::Bytes>(static_cast<std::size_t>(total));
        std::size_t offset = 0;
        for (rt::Bytes& chunk : chunks) {
          std::copy(chunk.begin(), chunk.end(), bytes->begin() + static_cast<std::ptrdiff_t>(offset));
          offset += chunk.size();
          std::fill(chunk.begin(), chunk.end(), std::uint8_t{0});
        }
      } catch (...) {
        error = std::current_exception();
      }
      if (error) {
        if (!isRequestBodyTooLargeError(error) && total > 0 && receivedRawBodyRejection(req)) {
          try { (void)rejectRawBodyOnce(req, "body_partial"); } catch (...) {}
        }
        for (rt::Bytes& chunk : chunks) std::fill(chunk.begin(), chunk.end(), std::uint8_t{0});
        rt::Promise<void> cancelled = rt::Promise<void>(reader.cancel(error)).catch_(rt::ignoreRejection);
        co_await cancelled.wait();
        reader.releaseLock();
        std::rethrow_exception(error);
      }
      reader.releaseLock();
      co_return bytes;
    }
  }

  rt::Task<JsonBodyReadResult> readJsonBodyWithLimit(rt::Request req, double maxBodyBytes, std::size_t maxDocumentBytes) {
    std::shared_ptr<rt::Bytes> bytes;
    bool captured = false;
    std::optional<JsonBodyReadResult> result;
    rt::Reason error;
    const auto reportReceivedStatus = receivedRawBodyRejection(req);
    const bool alreadyUsed = req.bodyUsed();
    try {
      bytes = co_await readBoundedRequestBody(req, maxBodyBytes);
      if (alreadyUsed && reportReceivedStatus) rejectRawBodyOnce(req, "body_unavailable");
      else if (!alreadyUsed) captured = captureRawBodyOnce(req, bytes, RawBodyStage::received);
      if (bytes->empty()) {
        if (captured && reportReceivedStatus) reportReceivedStatus("body_empty");
        result = JsonBodyReadResultNotOk{"empty"};
      } else {
        json::value parsed = rt::jsonParseWithLimit(rt::decodeUtf8(*bytes), maxDocumentBytes);
        if (!captured) captured = captureRawBodyOnce(req, bytes);
        result = JsonBodyReadResultOk{std::move(parsed)};
      }
    } catch (...) {
      error = std::current_exception();
    }
    rt::Reason escaped;
    if (error) {
      try {
        // A body the route refused at its own fixed cap is an explicit omission
        // reason, not a silently empty capture for the request that failed here.
        if (isRequestBodyTooLargeError(error)) rejectRawBodyOnce(req, "body_over_limit");
        else if (captured && reportReceivedStatus) reportReceivedStatus("body_invalid");
        else if (reportReceivedStatus) rejectRawBodyOnce(req, "body_unavailable");
        result = JsonBodyReadResultNotOk{isRequestBodyTooLargeError(error) ? "too_large" : "invalid"};
      } catch (...) {
        escaped = std::current_exception();
      }
    }
    discardRawBodyObserverOnce(req);
    if (bytes && !captured) std::fill(bytes->begin(), bytes->end(), std::uint8_t{0});
    if (escaped) std::rethrow_exception(escaped);
    co_return std::move(*result);
  }

  rt::Task<json::value> readJsonBody(rt::Request req, double maxBodyBytes, std::size_t maxDocumentBytes) {
    JsonBodyReadResult result = co_await readJsonBodyWithLimit(req, maxBodyBytes, maxDocumentBytes);
    if (JsonBodyReadResultOk* ok = std::get_if<JsonBodyReadResultOk>(&result)) co_return std::move(ok->value);
    co_return json::value(nullptr);
  }
}
