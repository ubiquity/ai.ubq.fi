#include "uos/bounded_response_body.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::bounded_response_body {
  namespace {
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

    /**
     * `AbortSignal.reason` is untyped, so a caller can abort with an arbitrary
     * value.  A promise rejection must carry an `Error`: when the reason already is
     * one it is passed through unchanged, otherwise it is preserved as the abort
     * error's `cause` instead of being rejected verbatim.
     */
    rt::Reason abortError(const rt::Reason& reason) {
      if (rt::isError(reason)) return reason;
      rt::JsError error = rt::withCause(rt::JsError("Upstream response body read aborted."), reason);
      error.name = "AbortError";
      return rt::makeReason(std::move(error));
    }

    rt::Task<ReadResult> abortableRead(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, rt::AbortSignal signal) {
      const auto aborted = rt::withResolvers<ReadResult>();
      rt::EventListener onAbort;
      try {
        onAbort = rt::EventListener([reject = aborted.reject, signal] { reject(abortError(signal.reason())); });
        signal.addEventListener("abort", onAbort, {.once = true});
        if (signal.aborted()) onAbort();
      } catch (...) {
        aborted.reject(std::current_exception());
      }
      std::optional<ReadResult> result;
      rt::Reason failure;
      try {
        std::vector<rt::Promise<ReadResult>> racers{reader.readPromise(), aborted.promise};
        rt::Promise<ReadResult> race = rt::race(std::move(racers));
        result = co_await race.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", onAbort);
      // The private latch can no longer abort the read. Release its race reaction.
      aborted.resolve(ReadResult{.done = true, .value = std::nullopt});
      if (failure) {
        std::rethrow_exception(failure);
      }
      co_return std::move(*result);
    }

    // `bytes.slice(0, end)` on a Uint8Array (ToIntegerOrInfinity, a negative end counts from the length).
    rt::ByteChunk sliceBytes(const rt::ByteChunk& bytes, double end) {
      const double length = static_cast<double>(bytes.size());
      const double relativeEnd = std::isnan(end) ? 0 : std::trunc(end);
      const double last = relativeEnd < 0 ? std::max(length + relativeEnd, 0.0) : std::min(relativeEnd, length);
      return rt::ByteChunk(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(last));
    }
  }

  rt::Task<BoundedResponseBody> readBoundedResponseBody(rt::Response response, ReadBoundedResponseBodyOptions options) {
    const double maxBytes = options.maxBytes.value_or(BOUNDED_RESPONSE_BODY_MAX_BYTES);
    const double timeoutMs = options.timeoutMs.value_or(BOUNDED_RESPONSE_BODY_TIMEOUT_MS);
    const std::optional<rt::ByteStream> body = response.body();
    if (!body) {
      BoundedResponseBody empty{.bytes = {}, .complete = true};
      co_return empty;
    }
    const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();

    std::vector<rt::ByteChunk> chunks;
    double length = 0;
    bool complete = false;
    bool released = false;
    bool cancellationStarted = false;
    const auto releaseReader = [&]() {
      if (released) return;
      released = true;
      try {
        reader.releaseLock();
      } catch (...) {
        // The reader may already be released by a failing upstream stream.
      }
    };
    const auto cancelReader = [&]() {
      if (cancellationStarted) return;
      cancellationStarted = true;
      try {
        const bool nullish = !options.cancellationReason || options.cancellationReason->is_null();
        rt::Task<void> cancellation = reader.cancel(rt::thrown(nullish ? json::value("Bounded upstream body read incomplete") : *options.cancellationReason));
        releaseReader();
        rt::spawn(std::move(cancellation), rt::ignoreRejection);
      } catch (...) {
        releaseReader();
      }
    };

    if (!(timeoutMs >= 0) || std::isinf(timeoutMs))
      throw rt::TypeError("Value " + rt::numberToString(timeoutMs) + " is outside the range [0, 9007199254740991]");
    const rt::AbortController timeout;
    auto timer = rt::detail::createExactTimer([timeout] { timeout.abort(rt::timeoutErrorReason()); }, timeoutMs);
    timer.unref();
    const auto clearDeadline = rt::scopeExit([&] { rt::clearTimeout(timer); });
    const rt::AbortSignal deadline = options.signal ? rt::AbortSignal::any({*options.signal, timeout.signal()}) : timeout.signal();
    try {
      for (;;) {
        const ReadResult next = co_await abortableRead(reader, deadline);
        if (next.done) {
          complete = true;
          break;
        }
        const double remaining = maxBytes - length;
        rt::ByteChunk part = sliceBytes(*next.value, remaining);
        const std::size_t partLength = part.size();
        chunks.push_back(std::move(part));
        length += static_cast<double>(partLength);
        if (partLength != next.value->size()) break;
      }
    } catch (...) {
      // Callers choose whether a partial body is safe to use.  The shared
      // primitive deliberately does not expose error details across providers.
    }
    if (complete) releaseReader();
    else cancelReader();

    BoundedResponseBody result{.bytes = {}, .complete = complete};
    result.bytes.reserve(static_cast<std::size_t>(length));
    for (const rt::ByteChunk& chunk : chunks) result.bytes.insert(result.bytes.end(), chunk.begin(), chunk.end());
    co_return result;
  }

  rt::Task<BoundedResponseBody> readBoundedResponseBody(rt::Response response) {
    ReadBoundedResponseBodyOptions options;
    return readBoundedResponseBody(std::move(response), std::move(options));
  }
}
