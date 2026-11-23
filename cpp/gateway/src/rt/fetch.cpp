#include "uos/rt/fetch.hpp"
#include "uos/rt/abort.hpp"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace uos::rt {
  namespace {
    Fetch& globalFetchSlot() {
      static Fetch slot = defaultFetch();
      return slot;
    }

    std::shared_ptr<FetchTransport>& transportSlot() {
      static std::shared_ptr<FetchTransport> slot;
      return slot;
    }

    // Bun's fetch accepts these schemes (file: and data: are Bun extensions).
    bool fetchableScheme(const URL& url) {
      const std::string protocol = url.protocol();
      return protocol == "http:" || protocol == "https:" || protocol == "s3:" || protocol == "file:" || protocol == "data:";
    }

    URL fetchUrl(std::string_view input) {
      if (input.empty()) throw CodedTypeError("fetch() URL must not be a blank string.", "ERR_INVALID_URL");
      auto parsed = URL::parse(input);
      if (!parsed) throw CodedTypeError("fetch() URL is invalid", "ERR_INVALID_URL");
      return std::move(*parsed);
    }

    void requireFetchableScheme(const URL& url) {
      if (!fetchableScheme(url)) throw CodedTypeError("protocol must be http:, https: or s3:", "ERR_INVALID_ARG_VALUE");
    }

    // The Request fetch(input, init) sends (its headers already hold the body's
    // content type, as Bun sends them).
    Request outgoingRequest(RequestInfo& input, RequestInit& init);

    Request preparedRequest(RequestInfo& input, RequestInit& init) {
      Request request = outgoingRequest(input, init);
      (void)request.headers();
      return request;
    }

    Request outgoingRequest(RequestInfo& input, RequestInit& init) {
      if (auto* text = std::get_if<std::string>(&input)) {
        URL url = fetchUrl(*text);
        requireFetchableScheme(url);
        return Request(url, std::move(init));
      }
      if (auto* url = std::get_if<URL>(&input)) {
        requireFetchableScheme(*url);
        return Request(*url, std::move(init));
      }
      const Request& source = std::get<Request>(input);
      requireFetchableScheme(URL(source.url()));
      if (init.body) return Request(source, std::optional<RequestInit>(std::move(init)));
      if (source.bodyUsed()) throw CodedTypeError("Request body already used", "ERR_BODY_ALREADY_USED");
      init.body = BodyInit(nullptr);
      Request outgoing(source, std::optional<RequestInit>(std::move(init)));
      detail::transferBody(source, outgoing);
      return outgoing;
    }

    // After the response arrived, an abort errors its body with the reason.
    struct AbortableBody {
      std::optional<ReadableStreamDefaultReader<ByteChunk>> reader;
      std::weak_ptr<detail::StreamState<ByteChunk>> wrapper;
      AbortSignal signal;
      EventListener onAbort;
      bool finished = false;
      explicit AbortableBody(AbortSignal signal_) : signal(std::move(signal_)) {}
      ~AbortableBody() { finish(); }
      void finish() {
        if (finished) return;
        finished = true;
        signal.removeEventListener("abort", onAbort);
      }
    };

    Task<void> pullAbortable(std::shared_ptr<AbortableBody> body, ReadableStreamDefaultController<ByteChunk> controller) {
      ReadableStreamReadResult<ByteChunk> step;
      try {
        Task<ReadableStreamReadResult<ByteChunk>> reading = body->reader->read();
        step = co_await std::move(reading);
      } catch (...) {
        body->finish();
        controller.error(std::current_exception());
        co_return;
      }
      if (body->finished) co_return;
      if (step.done) {
        body->finish();
        controller.close();
        co_return;
      }
      controller.enqueue(std::move(*step.value));
    }

    Task<void> cancelAbortable(std::shared_ptr<AbortableBody> body, Reason reason) {
      body->finish();
      Task<void> cancelling = body->reader->cancel(std::move(reason));
      co_await std::move(cancelling);
    }

    void makeBodyAbortable(const Response& response, const AbortSignal& signal) {
      std::optional<ByteStream> source = response.body();
      if (!source) return;
      auto body = std::make_shared<AbortableBody>(signal);
      body->reader = source->getReader();
      std::weak_ptr<AbortableBody> weak = body;
      body->onAbort = EventListener([weak] {
        auto held = weak.lock();
        if (!held || held->finished) return;
        held->finish();
        const Reason reason = held->signal.reason();
        if (auto wrapper = held->wrapper.lock()) wrapper->error(reason);
        spawn(held->reader->cancel(reason), [](Reason) {});
      });
      UnderlyingSource<ByteChunk> wrapper;
      wrapper.pull = [body](ReadableStreamDefaultController<ByteChunk> controller) { return pullAbortable(body, controller); };
      wrapper.cancel = [body](Reason reason) { return cancelAbortable(body, std::move(reason)); };
      ByteStream stream(std::move(wrapper), QueuingStrategy{0});
      body->wrapper = stream.state();
      signal.addEventListener("abort", body->onAbort, AddEventListenerOptions{.once = true});
      response.setBodyStream(std::move(stream));
    }

    Task<Response> sendThrough(Request request, bool watchSignal) {
      auto transport = fetchTransport();
      if (!transport) throw std::logic_error("rt::fetch: no FetchTransport installed (rt::setFetchTransport)");
      const AbortSignal signal = request.signal();
      if (signal.aborted()) rethrow(signal.reason());
      Task<Response> sending = transport->send(std::move(request));
      auto ownedSend = detail::keepAlive<Response>(std::move(transport), std::move(sending));
      Task<Response> guarded = abortable(signal, std::move(ownedSend));
      Response response = co_await std::move(guarded);
      if (watchSignal) {
        if (signal.aborted()) {
          if (auto body = response.body()) {
            const Reason reason = signal.reason();
            UnderlyingSource<ByteChunk> errored;
            errored.start = [reason](ReadableStreamDefaultController<ByteChunk> controller) { controller.error(reason); };
            spawn(body->cancel(reason), [](Reason) {});
            response.setBodyStream(ByteStream(std::move(errored)));
          }
        } else {
          makeBodyAbortable(response, signal);
        }
      }
      co_return response;
    }

    Task<Response> rejected(Reason reason) {
      rethrow(reason);
      co_return Response();
    }

    Task<Response> bunFetch(RequestInfo input, RequestInit init) {
      const bool watchSignal = init.signal.has_value() || std::holds_alternative<Request>(input);
      std::optional<Request> request;
      try {
        request = preparedRequest(input, init);
        if ((request->method() == "GET" || request->method() == "HEAD") && request->body())
          throw CodedTypeError("fetch() request with GET/HEAD method cannot have body.", "ERR_INVALID_ARG_VALUE");
      } catch (...) {
        return rejected(std::current_exception());
      }
      return sendThrough(std::move(*request), watchSignal);
    }
  }

  // Call eagerly, pinning the function before it can replace itself. A
  // returned coroutine may also borrow the arguments passed to the function.
  Task<Response> Fetch::operator()(RequestInfo input, RequestInit init) const {
    auto function = function_;
    if (!function) throw std::logic_error("rt::Fetch: called an empty fetch value");
    auto args = std::make_shared<std::pair<RequestInfo, RequestInit>>(std::move(input), std::move(init));
    auto task = (*function)(std::move(args->first), std::move(args->second));
    return detail::keepAlive<Response>(std::pair{std::move(function), std::move(args)}, std::move(task));
  }

  Task<Response> Fetch::resolved(Response response) { co_return response; }

  Fetch globalFetch() { return globalFetchSlot(); }

  void setGlobalFetch(Fetch fetch) { globalFetchSlot() = std::move(fetch); }

  Task<Response> fetch(RequestInfo input, RequestInit init) { return globalFetchSlot()(std::move(input), std::move(init)); }

  void setFetchTransport(std::shared_ptr<FetchTransport> transport) { transportSlot() = std::move(transport); }

  std::shared_ptr<FetchTransport> fetchTransport() { return transportSlot(); }

  Fetch defaultFetch() {
    static const Fetch instance = Fetch([](RequestInfo input, RequestInit init) -> Task<Response> { return bunFetch(std::move(input), std::move(init)); });
    return instance;
  }

  Task<Response> MockTransport::send(Request request) {
    requests_.push_back(request);
    auto handler = handler_;
    auto input = std::make_shared<Request>(std::move(request));
    auto task = (*handler)(std::move(*input));
    return detail::keepAlive<Response>(std::pair{std::move(handler), std::move(input)}, std::move(task));
  }
}
