#pragma once
// Port of src/responses-stream.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/stream.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>

namespace uos::responses_stream {
  inline const rt::OrderedSet<std::string> RESPONSES_TERMINAL_EVENT_TYPES{"error", "response.completed", "response.failed", "response.incomplete"};

  // Readonly<{ raw; value; type; terminal }>. The TS compares events by object
  // identity in places (`===`, WeakSet and WeakMap keys): a copy of an event is
  // the same JS object, every event built anew is a new one. `identityToken`
  // carries that identity, so an event is a key of rt::WeakMap / rt::WeakSet.
  struct ResponsesStreamEvent {
    std::string raw;
    json::object value;
    std::string type;
    bool terminal = false;
    std::shared_ptr<const void> identityToken = std::make_shared<char>();

    const void* identity() const noexcept { return identityToken.get(); }
    std::weak_ptr<const void> weakIdentity() const noexcept { return identityToken; }
  };

  /** The error payload in a flat error, nested error or failed response. */
  const json::object* responsesFailurePayload(const ResponsesStreamEvent& event);

  // "malformed_event" | "premature_eof" | "read_error" | "inactivity_timeout" | "event_too_large"
  // | "upstream_http_4xx" | "upstream_http_5xx" | "upstream_http_error" | "empty_upstream_completion"
  using ResponsesStreamFailureKind = std::string;

  inline constexpr double MAX_RESPONSES_SSE_EVENT_BYTES = 16 * 1024 * 1024;

  // Keep serverless and proxy connections active while the provider is thinking.
  // SSE comments are ignored by OpenAI clients and do not change the wire schema.
  inline constexpr double SSE_KEEPALIVE_INTERVAL_MS = 15'000;

  // AsyncGenerator<ResponsesStreamEvent, unknown, unknown>
  using ResponsesStreamIterator = rt::AsyncGenerator<ResponsesStreamEvent>;

  // Readonly<{ first; iterator; cancel: (reason?: unknown) => Promise<void> }>.
  // `cancel(nullptr)` is `cancel()` (an undefined reason).
  struct PreflightedResponsesStream {
    ResponsesStreamEvent first;
    ResponsesStreamIterator iterator;
    std::function<rt::Task<void>(rt::Reason reason)> cancel;
  };

  // `ErrorOptions & { kind?: ResponsesStreamFailureKind }`. `cause` holds any thrown
  // value (an Error or not); a null Reason is no cause.
  struct ResponsesStreamErrorOptions {
    rt::Reason cause = nullptr;
    std::optional<ResponsesStreamFailureKind> kind = std::nullopt;
  };

  namespace detail {
    // rt::JsError::cause for a cause that is an Error (a copy of it), else null.
    inline std::shared_ptr<const rt::JsError> errorCause(const rt::Reason& cause) {
      if (!cause) return nullptr;
      try {
        std::rethrow_exception(cause);
      } catch (const rt::JsError& error) {
        return std::make_shared<rt::JsError>(error);
      } catch (...) {
      }
      return nullptr;
    }
  }

  class ResponsesStreamError : public rt::JsError {
  public:
    ResponsesStreamFailureKind kind;
    // `error.cause` exactly as given (rt::JsError::cause also holds it when it is an Error).
    rt::Reason causeValue;

    explicit ResponsesStreamError(std::string message_, ResponsesStreamErrorOptions options = {})
      : rt::JsError(std::move(message_), "ResponsesStreamError", detail::errorCause(options.cause)),
        kind(options.kind.value_or("read_error")),
        causeValue(std::move(options.cause)) {}
  };

  // `reason instanceof ResponsesStreamError` (C++ addition): the error a Reason
  // holds, or nullptr. The pointer lives as long as the Reason.
  const ResponsesStreamError* responsesStreamErrorOf(const rt::Reason& reason) noexcept;

  // A TS callback typed `(...args) => void | Promise<void>`: built from a function
  // returning void, or from one returning rt::Task<void> (an async function, started
  // when called). Empty is `undefined`. Copies share the function.
  template <class... A> class MaybeAsyncCallback {
  public:
    using Sync = std::function<void(A...)>;
    using Async = std::function<rt::Task<void>(A...)>;

    MaybeAsyncCallback() = default;
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, MaybeAsyncCallback> && std::is_invocable_v<std::decay_t<F>&, A...> &&
               std::is_void_v<std::invoke_result_t<std::decay_t<F>&, A...>>)
    MaybeAsyncCallback(F&& function) : sync_(std::make_shared<const Sync>(std::forward<F>(function))) {}
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, MaybeAsyncCallback> && std::is_invocable_v<std::decay_t<F>&, A...> &&
               std::is_same_v<std::invoke_result_t<std::decay_t<F>&, A...>, rt::Task<void>>)
    MaybeAsyncCallback(F&& function) : async_(std::make_shared<const Async>(std::forward<F>(function))) {}

    explicit operator bool() const noexcept { return sync_ || async_; }

    // `Promise.resolve(callback(...args))`: a synchronous callback runs now (its
    // throw propagates) and gives a fulfilled promise; an asynchronous one runs up to
    // its first suspension and gives its promise. Empty: a fulfilled promise.
    rt::Promise<void> operator()(A... args) const {
      if (async_) return rt::Promise<void>(run(async_, std::move(args)...));
      if (sync_) (*sync_)(std::move(args)...);
      return rt::Promise<void>::resolve();
    }

  private:
    static rt::Task<void> run(std::shared_ptr<const Async> function, A... args) {
      auto pending = (*function)(std::move(args)...);
      co_await std::move(pending);
    }
    std::shared_ptr<const Sync> sync_;
    std::shared_ptr<const Async> async_;
  };

  // The `options` of readResponsesStream.
  struct ReadResponsesStreamOptions {
    std::optional<double> firstEventTimeoutMs = std::nullopt;
    std::optional<double> inactivityTimeoutMs = std::nullopt;
    /** Runs for every non-empty raw read, including comments and partial SSE frames. */
    MaybeAsyncCallback<> onActivity = {};
    std::function<void(const ResponsesStreamEvent&)> onEvent = {};
  };

  ResponsesStreamIterator readResponsesStream(rt::ByteStream stream, std::optional<rt::AbortSignal> signal = std::nullopt, ReadResponsesStreamOptions options = {});

  // The `options` of preflightResponsesStream: Readonly<{ onActivity?: () => void | Promise<void> }>.
  struct PreflightResponsesStreamOptions {
    MaybeAsyncCallback<> onActivity = {};
  };

  // Overloads instead of default arguments, so a caller never builds an options
  // temporary inside its co_await expression.
  [[nodiscard]] rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream, std::optional<rt::AbortSignal> signal,
                                                                            PreflightResponsesStreamOptions options);
  [[nodiscard]] rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream, std::optional<rt::AbortSignal> signal);
  [[nodiscard]] rt::Task<PreflightedResponsesStream> preflightResponsesStream(rt::ByteStream upstream);

  // ProxyResponsesStreamOptions. The lifecycle callbacks are started and never awaited.
  struct ProxyResponsesStreamOptions {
    std::optional<rt::AbortSignal> signal = std::nullopt;
    std::optional<rt::AbortSignal> downstreamSignal = std::nullopt;
    MaybeAsyncCallback<ResponsesStreamEvent> onEvent = {};
    MaybeAsyncCallback<rt::Reason> onFailure = {};
    MaybeAsyncCallback<rt::Reason> onCancel = {};
    /**
     * Releases the upstream read when the downstream consumer cancels.  An
     * abandoned consumer cannot await an iterator parked on a pending upstream
     * read, so cancellation has to reach the source directly.
     */
    // `(reason?: unknown) => void`; a null Reason is undefined.
    std::function<void(rt::Reason reason)> abortUpstream = {};
  };

  rt::ByteStream proxyResponsesStreamIterator(ResponsesStreamIterator iterator, ProxyResponsesStreamOptions options = {},
                                              std::optional<ResponsesStreamEvent> initialEvent = std::nullopt);

  // The `options` of withSseKeepalive.
  struct WithSseKeepaliveOptions {
    std::optional<double> intervalMs = std::nullopt;
  };

  /**
   * Adds protocol-level SSE comments while an upstream stream is quiet. The
   * source remains incremental: provider bytes are forwarded as soon as they
   * arrive, and the heartbeat is only a small connection-preserving burst.
   */
  rt::ByteStream withSseKeepalive(rt::ByteStream source, WithSseKeepaliveOptions options = {});

  rt::ByteStream proxyResponsesStream(rt::ByteStream upstream, ProxyResponsesStreamOptions options = {});
}
