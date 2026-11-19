#pragma once
// `fetch` as a value (`typeof fetch`), the process-wide `globalThis.fetch`,
// Bun's fetch front end (input validation, abort handling) and the transport
// seam the HTTP client stage plugs into. Reference: docs/rt/web.md.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <concepts>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt {
  // A JS fetch function value: `(input: RequestInfo | URL, init?: RequestInit)
  // => Promise<Response>`, the shape every injected fetcher in the TS has.
  // Copies share identity, so `globalThis.fetch === defaultFetch` ports as ==.
  class Fetch {
  public:
    using Function = std::function<Task<Response>(RequestInfo&& input, RequestInit&& init)>;

    // No function (JS `undefined`).
    Fetch() = default;
    // From a callable taking (RequestInfo, RequestInit) and returning
    // Task<Response> (an async function) or Response (a function returning an
    // already resolved promise).
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, Fetch> && std::is_invocable_v<std::decay_t<F>&, RequestInfo, RequestInit>)
    Fetch(F function) : function_(std::make_shared<const Function>(wrap(std::move(function)))) {}

    // `fetcher(input, init)`: calls the function now (like JS); std::logic_error
    // on an empty Fetch.
    [[nodiscard]] Task<Response> operator()(RequestInfo input, RequestInit init = {}) const;
    explicit operator bool() const noexcept { return function_ != nullptr; }
    friend bool operator==(const Fetch& left, const Fetch& right) noexcept { return left.function_ == right.function_; }

  private:
    template <class F> static Function wrap(F function) {
      using Result = std::invoke_result_t<F&, RequestInfo, RequestInit>;
      if constexpr (std::is_same_v<Result, Task<Response>>) {
        return Function(std::move(function));
      } else {
        static_assert(std::is_convertible_v<Result, Response>, "a fetch function returns Task<Response> or Response");
        auto held = std::make_shared<F>(std::move(function));
        return [held](RequestInfo input, RequestInit init) -> Task<Response> { return resolved((*held)(std::move(input), std::move(init))); };
      }
    }
    static Task<Response> resolved(Response response);
    std::shared_ptr<const Function> function_;
  };

  // `globalThis.fetch`: initially defaultFetch().
  Fetch globalFetch();
  // `globalThis.fetch = fetch`
  void setGlobalFetch(Fetch fetch);
  // `fetch(input, init)`: calls whatever globalThis.fetch holds now.
  [[nodiscard]] Task<Response> fetch(RequestInfo input, RequestInit init = {});

  // `const original = globalThis.fetch; globalThis.fetch = mock; try { ... }
  // finally { globalThis.fetch = original; }` as a scope.
  class GlobalFetchOverride {
  public:
    explicit GlobalFetchOverride(Fetch replacement) : original_(globalFetch()) { setGlobalFetch(std::move(replacement)); }
    ~GlobalFetchOverride() { setGlobalFetch(original_); }
    GlobalFetchOverride(const GlobalFetchOverride&) = delete;
    GlobalFetchOverride& operator=(const GlobalFetchOverride&) = delete;

  private:
    Fetch original_;
  };

  // The network seam: Bun's HTTP client. send() receives the validated Request
  // (absolute http:, https:, s3:, file: or data: URL; Bun's method; headers;
  // its body, readable through body()) and resolves with the Response once the
  // response head arrived, its body a stream fed as bytes arrive, url and
  // redirected recorded with setFetchResult. Redirects follow request.redirect()
  // as Bun does. Network failures reject with Bun's TypeErrors (see
  // docs/rt/web.md). Abort handling is done by the front end: on abort the Task
  // is abandoned and its pending Asio operations are cancelled.
  class FetchTransport {
  public:
    virtual ~FetchTransport() = default;
    [[nodiscard]] virtual Task<Response> send(Request request) = 0;
  };

  // The transport defaultFetch() sends through. None is installed until the
  // HTTP client stage installs one; a fetch without one throws std::logic_error.
  void setFetchTransport(std::shared_ptr<FetchTransport> transport);
  std::shared_ptr<FetchTransport> fetchTransport();

  // Bun's `fetch`: validates the input (CodedTypeError "fetch() URL must not be
  // a blank string." / "fetch() URL is invalid" (ERR_INVALID_URL), "protocol
  // must be http:, https: or s3:" (ERR_INVALID_ARG_VALUE), "fetch() request
  // with GET/HEAD method cannot have body." (ERR_INVALID_ARG_VALUE), "Request
  // body already used" (ERR_BODY_ALREADY_USED)), takes a Request input's body,
  // rejects with signal.reason when the signal is or becomes aborted before the
  // response, and errors the response body with signal.reason when it aborts
  // afterwards.
  Fetch defaultFetch();

  // A transport for tests: records every request and answers through a handler.
  class MockTransport : public FetchTransport {
  public:
    using Handler = std::function<Task<Response>(Request&& request)>;
    explicit MockTransport(Handler handler) : handler_(std::make_shared<const Handler>(std::move(handler))) {}
    Task<Response> send(Request request) override;
    // The requests sent so far, in order.
    const std::vector<Request>& requests() const { return requests_; }

  private:
    std::shared_ptr<const Handler> handler_;
    std::vector<Request> requests_;
  };
}
