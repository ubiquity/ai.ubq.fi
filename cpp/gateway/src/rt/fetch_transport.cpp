// rt::HttpClientTransport: Bun's fetch client around HTTP/1.1 exchanges.
// One exchange writes the request (concurrently with reading the response, so
// an early response is seen), parses the head and hands the connection to the
// response body stream, which reads it as the consumer pulls (a chunk is
// delivered as soon as it arrives) and returns it to the keep-alive pool once
// the body ended cleanly. Around it: redirects, the stale-connection retry of
// idempotent requests and Bun's error texts.
#include "fetch_transport_internal.hpp"

#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <boost/asio/cancellation_state.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/this_coro.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/system/system_error.hpp>

#include <cstdio>

namespace uos::rt::detail::fetch {
  namespace asio = boost::asio;

  namespace {
    // The send Task was abandoned by the front end (abort): Asio cancelled the
    // pending operation. Nobody observes this exception.
    [[noreturn]] void abandon() { throw boost::system::system_error(asio::error::operation_aborted); }

    std::string latin1Text(std::string_view bytes) {
      std::string out;
      out.reserve(bytes.size());
      for (const char byte : bytes) appendCodePoint(out, static_cast<unsigned char>(byte));
      return out;
    }

    bool isSecure(const URL& url) { return url.protocol() == "https:"; }

    // Bun retries a request whose reused keep-alive connection failed before a
    // response head arrived when the method is idempotent.
    bool retryable(const std::string& method) {
      return method == "GET" || method == "HEAD" || method == "PUT" || method == "DELETE" || method == "OPTIONS" || method == "TRACE";
    }

    struct RequestBody {
      enum class Kind : std::uint8_t { none, bytes, stream };
      Kind kind = Kind::none;
      std::shared_ptr<const Bytes> bytes;
      std::optional<ByteStream> stream;
    };

    // The request's body: a buffered body (string, bytes, URLSearchParams,
    // FormData, Blob) is sent with Content-Length, a ReadableStream chunked.
    // transferBody hands the body to a holder and marks only a buffered body
    // used, which tells the two apart.
    Task<RequestBody> takeBody(Request request) {
      RequestBody body;
      Request holder(request.url());
      transferBody(request, holder);
      if (request.bodyUsed()) {
        auto reading = holder.bytes();
        Bytes bytes = co_await std::move(reading);
        body.kind = RequestBody::Kind::bytes;
        body.bytes = std::make_shared<const Bytes>(std::move(bytes));
        co_return body;
      }
      std::optional<ByteStream> stream = holder.body();
      if (!stream) co_return body;
      body.kind = RequestBody::Kind::stream;
      body.stream = std::move(stream);
      co_return body;
    }

    struct Target {
      std::string host;
      std::uint16_t port = 0;
      bool secure = false;
      // "<scheme>//<host>:<port>"
      std::string key;
    };

    Target targetOf(const URL& url) {
      Target target;
      target.secure = isSecure(url);
      const std::string hostname = url.hostname();
      target.host = hostname.size() >= 2 && hostname.front() == '[' ? hostname.substr(1, hostname.size() - 2) : hostname;
      const std::string port = url.port();
      target.port = port.empty() ? static_cast<std::uint16_t>(target.secure ? 443 : 80) : static_cast<std::uint16_t>(std::stoi(port));
      target.key = url.protocol() + "//" + hostname + ":" + std::to_string(target.port);
      return target;
    }

    // getaddrinfo on Asio's resolver thread; an abort cancels it.
    Task<std::optional<std::vector<asio::ip::tcp::endpoint>>> resolve(std::string host, std::uint16_t port) {
      auto resolver = std::make_shared<asio::ip::tcp::resolver>(rt::executor());
      asio::cancellation_state state = co_await asio::this_coro::cancellation_state;
      asio::cancellation_slot slot = state.slot();
      std::weak_ptr<asio::ip::tcp::resolver> weak = resolver;
      if (slot.is_connected()) {
        slot.assign([weak](asio::cancellation_type) {
          if (auto held = weak.lock()) held->cancel();
        });
      }
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const std::string service = std::to_string(port);
      const asio::ip::tcp::resolver::results_type results = co_await resolver->async_resolve(host, service, token);
      if (slot.is_connected()) slot.clear();
      if (error == asio::error::operation_aborted) abandon();
      if (error || results.empty()) co_return std::nullopt;
      std::vector<asio::ip::tcp::endpoint> endpoints;
      for (const auto& entry : results) endpoints.push_back(entry.endpoint());
      co_return endpoints;
    }

    // A new connection: DNS unless the host is an IP address, TCP connect to
    // each address in turn, then TLS for https:.
    Task<std::shared_ptr<Connection>> connect(std::shared_ptr<ClientState> client, Target target, std::string url) {
      std::vector<asio::ip::tcp::endpoint> endpoints;
      boost::system::error_code invalid;
      const asio::ip::address address = asio::ip::make_address(target.host, invalid);
      if (!invalid) {
        endpoints.emplace_back(address, target.port);
      } else {
        auto resolving = resolve(target.host, target.port);
        std::optional<std::vector<asio::ip::tcp::endpoint>> resolved = co_await std::move(resolving);
        if (!resolved) throw dnsError(target.host, url);
        endpoints = std::move(*resolved);
      }
      std::optional<asio::ip::tcp::socket> connected;
      for (const asio::ip::tcp::endpoint& endpoint : endpoints) {
        asio::ip::tcp::socket socket(rt::executor());
        boost::system::error_code error;
        auto token = asio::redirect_error(asio::use_awaitable, error);
        co_await socket.async_connect(endpoint, token);
        if (error == asio::error::operation_aborted) abandon();
        if (error) continue;
        boost::system::error_code ignored;
        socket.set_option(asio::ip::tcp::no_delay(true), ignored);
        connected.emplace(std::move(socket));
        break;
      }
      if (!connected) throw connectError(url);
      auto connection = std::make_shared<Connection>(std::move(*connected), target.key, target.secure);
      connection->watchIdle(client->options.timeoutMs);
      if (target.secure) {
        auto shaking = connection->handshake(client->context, target.host, client->rejectUnauthorized(), url);
        const Connection::HandshakeResult result = co_await std::move(shaking);
        if (connection->timedOut()) rethrow(timeoutErrorReason());
        if (result.aborted) abandon();
        if (result.failure) rethrow(result.failure);
      }
      co_return connection;
    }

    Task<std::shared_ptr<Connection>> acquire(std::shared_ptr<ClientState> client, Target target, std::string url) {
      std::shared_ptr<Pool> pool = client->pool();
      if (std::shared_ptr<Connection> pooled = pool->acquire(target.key, target.secure)) {
        pooled->watchIdle(client->options.timeoutMs);
        co_return pooled;
      }
      auto connecting = connect(client, target, url);
      std::shared_ptr<Connection> connection = co_await std::move(connecting);
      co_return connection;
    }

    // ---- writing the request ----------------------------------------------------

    struct WriterState {
      // The request was written in full.
      bool done = false;
      // The request body stream failed (the exchange fails with its reason).
      Reason bodyFailure;
      std::optional<ReadableStreamDefaultReader<ByteChunk>> reader;
    };

    std::string chunkSizeLine(std::size_t size) {
      char text[32];
      std::snprintf(text, sizeof text, "%zx\r\n", size);
      return text;
    }

    Task<void> writeRequest(std::shared_ptr<Connection> connection, std::shared_ptr<const std::string> head, RequestBody body, std::shared_ptr<WriterState> state) {
      try {
        const auto* headBytes = reinterpret_cast<const std::uint8_t*>(head->data());
        if (body.kind == RequestBody::Kind::bytes && body.bytes->size() <= 65536) {
          auto whole = std::make_shared<std::string>(*head);
          whole->append(reinterpret_cast<const char*>(body.bytes->data()), body.bytes->size());
          auto writing = connection->write(whole, reinterpret_cast<const std::uint8_t*>(whole->data()), whole->size());
          const bool written = co_await std::move(writing);
          state->done = written;
          co_return;
        }
        auto writingHead = connection->write(head, headBytes, head->size());
        const bool headWritten = co_await std::move(writingHead);
        if (!headWritten) co_return;
        if (body.kind == RequestBody::Kind::bytes) {
          auto writingBody = connection->write(body.bytes, body.bytes->data(), body.bytes->size());
          const bool bodyWritten = co_await std::move(writingBody);
          state->done = bodyWritten;
          co_return;
        }
        if (body.kind == RequestBody::Kind::stream) {
          state->reader = body.stream->getReader();
          const ReadableStreamDefaultReader<ByteChunk> reader = *state->reader;
          for (;;) {
            ReadableStreamReadResult<ByteChunk> step;
            Reason failure;
            try {
              auto reading = reader.read();
              step = co_await std::move(reading);
            } catch (...) {
              failure = std::current_exception();
            }
            if (failure) {
              state->bodyFailure = failure;
              connection->close();
              co_return;
            }
            if (step.done) break;
            if (!connection->isOpen()) co_return;
            if (step.value->empty()) continue;
            auto framed = std::make_shared<std::string>(chunkSizeLine(step.value->size()));
            framed->append(reinterpret_cast<const char*>(step.value->data()), step.value->size());
            framed->append("\r\n");
            auto writingChunk = connection->write(framed, reinterpret_cast<const std::uint8_t*>(framed->data()), framed->size());
            const bool chunkWritten = co_await std::move(writingChunk);
            if (!chunkWritten) co_return;
          }
          auto last = std::make_shared<const std::string>("0\r\n\r\n");
          auto writingLast = connection->write(last, reinterpret_cast<const std::uint8_t*>(last->data()), last->size());
          const bool lastWritten = co_await std::move(writingLast);
          state->done = lastWritten;
          co_return;
        }
        state->done = true;
      } catch (...) {
        state->bodyFailure = std::current_exception();
        connection->close();
      }
    }

    // ---- one exchange -------------------------------------------------------------

    struct Exchange {
      std::shared_ptr<Connection> connection;
      ResponseHead head;
      FramingRule framing;
      std::shared_ptr<WriterState> writer;
      // The response and the request allow the connection to be reused.
      bool keepAlive = false;
    };

    // A reused keep-alive connection failed before a response head arrived.
    struct StaleConnection {};

    bool requestClosesConnection(const Headers& headers) {
      const std::optional<std::string> value = headers.get("connection");
      if (!value) return false;
      ResponseHead probe;
      probe.fields.emplace_back("connection", *value);
      return probe.connectionHas("close");
    }

    Task<std::shared_ptr<Exchange>> transact(std::shared_ptr<ClientState> client, std::shared_ptr<Connection> connection, std::string method, URL url, Headers headers,
                                             RequestBody body, std::string originalUrl, AbortSignal signal) {
      const RequestFraming framing =
          body.kind == RequestBody::Kind::bytes ? RequestFraming::length : body.kind == RequestBody::Kind::stream ? RequestFraming::chunked : RequestFraming::none;
      const std::uint64_t length = body.kind == RequestBody::Kind::bytes ? body.bytes->size() : 0;
      auto head = std::make_shared<const std::string>(requestHead(method, url, headers, framing, length, client->userAgent, acceptEncoding()));
      auto writer = std::make_shared<WriterState>();
      bool completed = false;
      // A failed or abandoned exchange closes its connection; on abort the
      // request body stream is cancelled with the signal's reason.
      auto cleanup = scopeExit([&completed, connection, writer, signal] {
        if (completed) return;
        connection->close();
        if (writer->reader && signal.aborted()) spawn(writer->reader->cancel(signal.reason()), ignoreRejection);
      });
      spawn(writeRequest(connection, head, body, writer));
      auto exchange = std::make_shared<Exchange>();
      for (;;) {
        const HeadParse parsed = parseResponseHead(connection->input.view());
        if (parsed.status == HeadStatus::complete) {
          connection->input.consume(parsed.length);
          const int status = parsed.head.status;
          if (status == 101) throw verboseError("UnrequestedUpgrade", originalUrl);
          if (status >= 100 && status < 200) continue;
          exchange->head = parsed.head;
          break;
        }
        if (parsed.status == HeadStatus::malformed) throw verboseError("Malformed_HTTP_Response", originalUrl);
        if (connection->input.size() >= kMaxResponseHead) throw verboseError("ResponseHeadersTooLarge", originalUrl);
        auto filling = connection->fill();
        const ReadStatus status = co_await std::move(filling);
        if (status == ReadStatus::data) continue;
        if (writer->bodyFailure) rethrow(writer->bodyFailure);
        if (connection->timedOut()) rethrow(timeoutErrorReason());
        if (status == ReadStatus::aborted) abandon();
        if (connection->reused) throw StaleConnection{};
        throw closedError(originalUrl);
      }
      exchange->framing = responseFraming(method, exchange->head);
      if (exchange->framing.error) throw verboseError(*exchange->framing.error, originalUrl);
      const bool responseKeepsAlive = exchange->head.minorVersion >= 1 ? !exchange->head.connectionHas("close") : exchange->head.connectionHas("keep-alive");
      exchange->keepAlive = responseKeepsAlive && !requestClosesConnection(headers) && exchange->framing.framing != Framing::close;
      exchange->connection = connection;
      exchange->writer = writer;
      completed = true;
      co_return exchange;
    }

    // An exchange on a pooled or new connection, retried while a reused
    // connection turns out stale (idempotent methods, replayable bodies).
    Task<std::shared_ptr<Exchange>> exchange(std::shared_ptr<ClientState> client, std::string method, URL url, Headers headers, RequestBody body, std::string originalUrl,
                                             AbortSignal signal) {
      const Target target = targetOf(url);
      for (;;) {
        auto acquiring = acquire(client, target, originalUrl);
        std::shared_ptr<Connection> connection = co_await std::move(acquiring);
        auto transacting = transact(client, connection, method, url, headers, body, originalUrl, signal);
        bool stale = false;
        try {
          std::shared_ptr<Exchange> result = co_await std::move(transacting);
          co_return result;
        } catch (const StaleConnection&) {
          stale = true;
        }
        if (stale && (!retryable(method) || body.kind == RequestBody::Kind::stream)) throw closedError(originalUrl);
      }
    }

    // ---- the response body ----------------------------------------------------------

    struct BodyPump {
      std::weak_ptr<Pool> pool;
      std::shared_ptr<Connection> connection;
      Framing framing = Framing::empty;
      std::uint64_t remaining = 0;
      ChunkedDecoder chunked;
      std::unique_ptr<ContentDecoder> decoder;
      std::shared_ptr<WriterState> writer;
      bool keepAlive = false;
      std::string url;
      bool finished = false;
      bool cancelled = false;
      BodyPump() = default;
      BodyPump(const BodyPump&) = delete;
      BodyPump& operator=(const BodyPump&) = delete;
      ~BodyPump() {
        if (connection) connection->close();
      }
    };

    std::shared_ptr<BodyPump> pumpFor(const std::shared_ptr<ClientState>& client, const Exchange& exchange, std::string url) {
      auto pump = std::make_shared<BodyPump>();
      pump->pool = client->pool();
      pump->connection = exchange.connection;
      pump->framing = exchange.framing.framing;
      pump->remaining = exchange.framing.length;
      pump->writer = exchange.writer;
      pump->keepAlive = exchange.keepAlive;
      pump->url = std::move(url);
      return pump;
    }

    // The body ended: the connection goes back to the pool when nothing else
    // arrived on it and the request was written in full; otherwise it closes.
    void finishBody(BodyPump& pump) {
      pump.finished = true;
      std::shared_ptr<Connection> connection = std::move(pump.connection);
      if (!connection) return;
      if (pump.keepAlive && pump.writer->done && connection->isOpen() && !connection->hasBufferedInput()) {
        if (auto pool = pump.pool.lock()) {
          pool->release(std::move(connection));
          return;
        }
      }
      connection->close();
    }

    // Moves the body bytes the framing allows out of the connection's input;
    // true once the body ended. Throws InvalidHTTPResponse for bad chunking.
    bool takeFramed(BodyPump& pump, std::string& raw) {
      ByteQueue& input = pump.connection->input;
      switch (pump.framing) {
        case Framing::empty: return true;
        case Framing::length: {
          const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(pump.remaining, input.size()));
          raw.append(input.view().substr(0, take));
          input.consume(take);
          pump.remaining -= take;
          return pump.remaining == 0;
        }
        case Framing::chunked: {
          std::size_t offset = 0;
          const ChunkedDecoder::Result result = pump.chunked.decode(input.view(), offset, raw);
          input.consume(offset);
          if (result == ChunkedDecoder::Result::invalid) throw verboseError("InvalidHTTPResponse", pump.url);
          return result == ChunkedDecoder::Result::done;
        }
        case Framing::close:
          raw.append(input.view());
          input.clear();
          return false;
      }
      return true;
    }

    void enqueueText(const ReadableStreamDefaultController<ByteChunk>& controller, const std::string& text) {
      if (!text.empty()) controller.enqueue(ByteChunk(text.begin(), text.end()));
    }

    Task<void> pullBody(std::shared_ptr<BodyPump> pump, ReadableStreamDefaultController<ByteChunk> controller) {
      try {
        for (;;) {
          if (pump->cancelled || pump->finished || !pump->connection) co_return;
          std::string raw;
          const bool ended = takeFramed(*pump, raw);
          std::string out;
          if (pump->decoder) {
            if (!raw.empty()) pump->decoder->write(raw, out);
            if (ended) pump->decoder->finish(out);
          } else {
            out = std::move(raw);
          }
          if (ended) {
            finishBody(*pump);
            enqueueText(controller, out);
            controller.close();
            co_return;
          }
          if (!out.empty()) {
            enqueueText(controller, out);
            co_return;
          }
          auto filling = pump->connection->fill();
          const ReadStatus status = co_await std::move(filling);
          if (pump->cancelled) co_return;
          if (status == ReadStatus::data) continue;
          if (pump->connection->timedOut()) rethrow(timeoutErrorReason());
          if (status == ReadStatus::aborted) co_return;
          if (status == ReadStatus::eof && pump->framing == Framing::close) {
            std::string tail;
            if (pump->decoder) pump->decoder->finish(tail);
            pump->finished = true;
            pump->connection->close();
            pump->connection.reset();
            enqueueText(controller, tail);
            controller.close();
            co_return;
          }
          throw closedError(pump->url);
        }
      } catch (...) {
        pump->finished = true;
        if (pump->connection) {
          pump->connection->close();
          pump->connection.reset();
        }
        throw;
      }
    }

    Response makeResponse(const std::shared_ptr<ClientState>& client, const Exchange& exchange, std::string url) {
      Headers headers;
      for (const auto& [name, value] : exchange.head.fields) headers.append(name, latin1Text(value));
      std::shared_ptr<BodyPump> pump = pumpFor(client, exchange, url);
      UnderlyingSource<ByteChunk> source;
      if (pump->framing == Framing::empty) {
        finishBody(*pump);
        source.start = [](ReadableStreamDefaultController<ByteChunk> controller) { controller.close(); };
      } else {
        pump->decoder = contentDecoder(exchange.head.field("content-encoding"), pump->url);
        source.pull = [pump](ReadableStreamDefaultController<ByteChunk> controller) { return pullBody(pump, controller); };
        source.cancel = [pump](Reason) {
          pump->cancelled = true;
          if (pump->connection) {
            pump->connection->close();
            pump->connection.reset();
          }
        };
      }
      const ByteStream stream(std::move(source));
      ResponseInit init;
      init.status = exchange.head.status;
      init.statusText = decodeUtf8(std::string_view(exchange.head.reason), TextDecoderOptions{.fatal = false, .ignoreBOM = true});
      init.headers = std::move(headers);
      return Response(BodyInit(stream), std::move(init));
    }

    // Reads and drops a redirect response's body so its connection can be reused.
    Task<void> drain(std::shared_ptr<ClientState> client, std::shared_ptr<Exchange> exchange, std::string url) {
      std::shared_ptr<BodyPump> pump = pumpFor(client, *exchange, url);
      if (pump->framing == Framing::close) co_return;
      for (;;) {
        std::string ignored;
        if (takeFramed(*pump, ignored)) {
          finishBody(*pump);
          co_return;
        }
        auto filling = pump->connection->fill();
        const ReadStatus status = co_await std::move(filling);
        if (status == ReadStatus::data) continue;
        if (pump->connection->timedOut()) rethrow(timeoutErrorReason());
        if (status == ReadStatus::aborted) abandon();
        throw closedError(url);
      }
    }

    bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }
  }

  Task<Response> send(std::shared_ptr<ClientState> client, Request request) {
    const std::string originalUrl = request.url();
    URL url(originalUrl);
    if (url.protocol() != "http:" && url.protocol() != "https:")
      throw TypeError("rt::HttpClientTransport speaks http: and https: only (" + url.protocol() + " URLs are not supported)");
    std::string method = request.method();
    Headers headers = request.headers();
    const std::string mode = request.redirect();
    const AbortSignal signal = request.signal();
    auto taking = takeBody(request);
    RequestBody body = co_await std::move(taking);
    int redirects = 0;
    for (;;) {
      auto exchanging = exchange(client, method, url, headers, body, originalUrl, signal);
      std::shared_ptr<Exchange> current = co_await std::move(exchanging);
      const int status = current->head.status;
      if (isRedirect(status) && mode == "error") {
        current->connection->close();
        throw verboseError("UnexpectedRedirect", originalUrl);
      }
      const std::optional<std::string> location = isRedirect(status) && mode == "follow" ? current->head.field("location") : std::nullopt;
      if (location) {
        std::optional<URL> target = URL::parse(latin1Text(*location), url.href());
        if (!target) {
          current->connection->close();
          throw FetchError("Redirect URL in Location header is invalid.", "RedirectURLInvalid", originalUrl);
        }
        if (target->protocol() != "http:" && target->protocol() != "https:") {
          current->connection->close();
          throw verboseError("UnsupportedRedirectProtocol", originalUrl);
        }
        if (redirects == 126) {
          current->connection->close();
          throw FetchError("The response redirected too many times. For more information, pass `verbose: true` in the second argument to fetch()", "TooManyRedirects",
                           originalUrl);
        }
        const bool toGet = (status == 303 && method != "HEAD") || ((status == 301 || status == 302) && method == "POST");
        if (toGet) {
          method = "GET";
          body = RequestBody();
          for (const char* name : {"content-type", "content-encoding", "content-language", "content-location"}) headers.delete_(name);
        } else if (body.kind == RequestBody::Kind::stream) {
          current->connection->close();
          throw TypeError("Request body is a ReadableStream and cannot be replayed for this redirect");
        }
        if (target->origin() != url.origin()) {
          for (const char* name : {"authorization", "cookie", "proxy-authorization"}) headers.delete_(name);
        }
        auto draining = drain(client, current, originalUrl);
        co_await std::move(draining);
        url = std::move(*target);
        ++redirects;
        continue;
      }
      Response response = makeResponse(client, *current, originalUrl);
      response.setFetchResult(redirects > 0 ? url.href() : originalUrl, redirects > 0);
      co_return response;
    }
  }
}

namespace uos::rt {
  HttpClientTransport::HttpClientTransport() : HttpClientTransport(HttpClientOptions()) {}

  HttpClientTransport::HttpClientTransport(HttpClientOptions options) : state_(std::make_shared<detail::fetch::ClientState>(std::move(options))) {}

  HttpClientTransport::~HttpClientTransport() = default;

  Task<Response> HttpClientTransport::send(Request request) { return detail::fetch::send(state_, std::move(request)); }

  std::size_t HttpClientTransport::idleConnections() const {
    const std::shared_ptr<detail::fetch::Pool> pool = state_->existingPool();
    return pool ? pool->idleCount() : 0;
  }

  void HttpClientTransport::closeIdleConnections() {
    if (const std::shared_ptr<detail::fetch::Pool> pool = state_->existingPool()) pool->closeAll();
  }

  std::shared_ptr<HttpClientTransport> installHttpClientTransport() { return installHttpClientTransport(HttpClientOptions()); }

  std::shared_ptr<HttpClientTransport> installHttpClientTransport(HttpClientOptions options) {
    auto transport = std::make_shared<HttpClientTransport>(std::move(options));
    setFetchTransport(transport);
    return transport;
  }
}
