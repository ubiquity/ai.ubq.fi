#include "uos/rt/stream.hpp"

namespace uos::rt {
  namespace {
    // One element of the JS array [reason1, reason2] as a JSON value: the value
    // itself for a non-Error reason, String(error) for an Error.
    json::value arrayElement(const Reason& reason) {
      if (!reason) return nullptr;
      if (!isError(reason)) {
        auto value = thrownValue(reason);
        return value ? *value : json::value(nullptr);
      }
      return json::value(reasonToString(reason));
    }

    json::value compositeOf(const Reason& first, const Reason& second) { return json::array{arrayElement(first), arrayElement(second)}; }
  }

  TeeCancelReason::TeeCancelReason(Reason first, Reason second) : ThrownValue(compositeOf(first, second)), reason1(std::move(first)), reason2(std::move(second)) {}

  namespace detail {
    Reason streamError(StreamError which) {
      switch (which) {
        case StreamError::locked:
          return makeReason(TypeError("Invalid state: ReadableStream is locked"));
        case StreamError::cancelLocked:
          return makeReason(TypeError("Cannot cancel a locked ReadableStream"));
        case StreamError::enqueueClosed:
        case StreamError::closeClosed:
          return makeReason(TypeError("Invalid state: Controller is already closed"));
        case StreamError::releasedRead:
        case StreamError::releasedCancel:
          return makeReason(TypeError("Invalid state: The reader is not attached to a stream"));
        case StreamError::releasingReader:
          return makeReason(TypeError("Invalid state: Releasing reader"));
        case StreamError::readerReleased:
          return makeReason(TypeError("Invalid state: Reader released"));
        case StreamError::highWaterMark:
          return makeReason(RangeError("The queuing strategy's highWaterMark must be a non-negative, non-NaN number"));
      }
      return makeReason(TypeError("Invalid state"));
    }

    std::uint64_t nextReaderId() noexcept {
      static std::uint64_t next = 0;
      return ++next;
    }
  }

  template class ReadableStream<ByteChunk>;
  template class ReadableStreamDefaultReader<ByteChunk>;
  template class ReadableStreamDefaultController<ByteChunk>;
}
