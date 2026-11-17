#pragma once
// Port of src/deepseek/stream.ts: DeepSeek SSE stream parsing, split out of src/deepseek.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace uos::deepseek::stream {
  // "malformed_event" | "invalid_chunk" | "frame_too_large" | "premature_eof" | "read_error" | "inactivity_timeout"
  using DeepSeekStreamFailureKind = std::string;

  // `options?: ErrorOptions & { kind?: DeepSeekStreamFailureKind }`. `cause` std::nullopt is "no
  // cause property"; a set cause is the value exactly as given (a null Reason is `cause: undefined`).
  struct DeepSeekStreamErrorOptions {
    std::optional<rt::Reason> cause{};
    std::optional<DeepSeekStreamFailureKind> kind{};
  };

  class DeepSeekStreamError : public rt::JsError {
  public:
    DeepSeekStreamFailureKind kind;

    explicit DeepSeekStreamError(std::string message_) : DeepSeekStreamError(std::move(message_), DeepSeekStreamErrorOptions{}) {}
    DeepSeekStreamError(std::string message_, const DeepSeekStreamErrorOptions& options)
      : rt::JsError(std::move(message_), "DeepSeekStreamError"), kind(options.kind.value_or("read_error")) {
      if (options.cause) {
        // `super(message, { cause })`: rt::withCause's two members.
        try {
          if (*options.cause) std::rethrow_exception(*options.cause);
        } catch (const rt::JsError& inner) {
          cause = std::make_shared<rt::JsError>(inner);
        } catch (...) {
        }
        causeValue = *options.cause;
      }
    }
  };

  // `DeepSeekStreamFrame`: `{ kind: "comment"; text }`, `{ kind: "chunk"; value }`, `{ kind: "done" }`.
  struct DeepSeekStreamFrameComment {
    static constexpr std::string_view kind = "comment";
    std::string text;
    friend bool operator==(const DeepSeekStreamFrameComment&, const DeepSeekStreamFrameComment&) = default;
  };
  struct DeepSeekStreamFrameChunk {
    static constexpr std::string_view kind = "chunk";
    json::object value;
    friend bool operator==(const DeepSeekStreamFrameChunk&, const DeepSeekStreamFrameChunk&) = default;
  };
  struct DeepSeekStreamFrameDone {
    static constexpr std::string_view kind = "done";
    friend bool operator==(const DeepSeekStreamFrameDone&, const DeepSeekStreamFrameDone&) = default;
  };
  using DeepSeekStreamFrame = std::variant<DeepSeekStreamFrameComment, DeepSeekStreamFrameChunk, DeepSeekStreamFrameDone>;

  struct DeepSeekStreamOptions {
    /** Request-level cancellation, composed with the stream deadlines. */
    std::optional<rt::AbortSignal> signal{};
    /** Bounds the wait for the first upstream frame. */
    std::optional<double> firstEventTimeoutMs{};
    /** Bounds the gap between subsequent upstream frames. */
    std::optional<double> inactivityTimeoutMs{};
  };

  /**
   * Reads the upstream Chat Completions SSE body and yields validated frames.
   *
   * `data:` frames are the OpenAI Chat Completions stream contract and are
   * normalized before they leave this module. DeepSeek's documented
   * `: keep-alive` comment frames are forwarded verbatim, because they are what
   * keeps a long thinking turn from looking like an idle connection to an edge
   * proxy. Any other SSE field (`event:`, `id:`, `retry:`) is not part of the
   * Chat Completions contract and is not relayed.
   */
  // `options: DeepSeekStreamOptions = {}`. Nothing runs until the first next(), as for the TS async
  // generator.
  rt::AsyncGenerator<DeepSeekStreamFrame> iterateDeepSeekChatCompletionStream(rt::Response response, std::string requestedModel);
  rt::AsyncGenerator<DeepSeekStreamFrame> iterateDeepSeekChatCompletionStream(rt::Response response, std::string requestedModel, const DeepSeekStreamOptions& options);
}
