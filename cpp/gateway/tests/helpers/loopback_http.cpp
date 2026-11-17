#include "tests/helpers/loopback_http.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <boost/asio/post.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <array>
#include <charconv>
#include <cstdint>
#include <optional>
#include <utility>

namespace uos::tests::helpers::loopback_http {
  namespace asio = boost::asio;

  namespace {
    std::string latin1Text(std::string_view bytes) {
      std::string out;
      for (const char byte : bytes) rt::appendCodePoint(out, static_cast<unsigned char>(byte));
      return out;
    }

    std::string latin1Bytes(std::string_view text) {
      std::string out;
      for (const std::uint32_t codePoint : rt::codePoints(text)) out.push_back(static_cast<char>(codePoint & 0xFF));
      return out;
    }

    std::string lowerAscii(std::string text) {
      for (char& character : text)
        if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
      return text;
    }

    struct Connection {
      asio::ip::tcp::socket socket;
      std::string buffer;
      explicit Connection(asio::any_io_executor executor) : socket(std::move(executor)) {}
      void close() {
        boost::system::error_code ignored;
        socket.close(ignored);
      }
    };

    // Reads more bytes into the buffer; false at the end of the stream.
    rt::Task<bool> fill(std::shared_ptr<Connection> connection) {
      std::array<char, 65536> chunk{};
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const std::size_t count = co_await connection->socket.async_read_some(asio::buffer(chunk), token);
      if (error) co_return false;
      connection->buffer.append(chunk.data(), count);
      co_return true;
    }

    struct BodyState {
      enum class Mode { none, length, chunked, close };
      enum class Step { size, data, dataEnd, trailer };
      std::shared_ptr<Connection> connection;
      Mode mode = Mode::none;
      std::uint64_t remaining = 0;
      Step step = Step::size;
    };

    rt::JsError closedEarly() { return rt::TypeError("The socket connection was closed unexpectedly. For more information, pass `verbose: true` in the second argument to fetch()"); }

    // The next decoded body chunk, std::nullopt at the end of the body.
    rt::Task<std::optional<rt::Bytes>> nextChunk(std::shared_ptr<BodyState> state) {
      std::string& buffer = state->connection->buffer;
      for (;;) {
        switch (state->mode) {
          case BodyState::Mode::none:
            co_return std::nullopt;
          case BodyState::Mode::close: {
            if (!buffer.empty()) {
              rt::Bytes chunk(buffer.begin(), buffer.end());
              buffer.clear();
              co_return chunk;
            }
            const bool more = co_await fill(state->connection);
            if (!more) co_return std::nullopt;
            break;
          }
          case BodyState::Mode::length: {
            if (state->remaining == 0) co_return std::nullopt;
            if (buffer.empty()) {
              const bool more = co_await fill(state->connection);
              if (!more) throw closedEarly();
              break;
            }
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(state->remaining, buffer.size()));
            rt::Bytes chunk(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(take));
            buffer.erase(0, take);
            state->remaining -= take;
            co_return chunk;
          }
          case BodyState::Mode::chunked: {
            if (state->step == BodyState::Step::size || state->step == BodyState::Step::trailer || state->step == BodyState::Step::dataEnd) {
              const std::size_t lineEnd = buffer.find("\r\n");
              if (lineEnd == std::string::npos) {
                const bool more = co_await fill(state->connection);
                if (!more) throw closedEarly();
                break;
              }
              const std::string line = buffer.substr(0, lineEnd);
              buffer.erase(0, lineEnd + 2);
              if (state->step == BodyState::Step::dataEnd) {
                state->step = BodyState::Step::size;
                break;
              }
              if (state->step == BodyState::Step::trailer) {
                if (line.empty()) {
                  state->mode = BodyState::Mode::none;
                  co_return std::nullopt;
                }
                break;
              }
              state->remaining = std::stoull(line.substr(0, line.find(';')), nullptr, 16);
              state->step = state->remaining == 0 ? BodyState::Step::trailer : BodyState::Step::data;
              break;
            }
            if (buffer.empty()) {
              const bool more = co_await fill(state->connection);
              if (!more) throw closedEarly();
              break;
            }
            const std::size_t take = static_cast<std::size_t>(std::min<std::uint64_t>(state->remaining, buffer.size()));
            rt::Bytes chunk(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(take));
            buffer.erase(0, take);
            state->remaining -= take;
            if (state->remaining == 0) state->step = BodyState::Step::dataEnd;
            co_return chunk;
          }
        }
      }
    }

    class LoopbackTransport final : public rt::FetchTransport {
    public:
      rt::Task<rt::Response> send(rt::Request request) override {
        const rt::URL url(request.url());
        if (url.protocol() != "http:") throw rt::TypeError("the loopback fetch transport speaks http: only");
        std::string hostname = url.hostname();
        if (rt::startsWith(hostname, "[")) hostname = hostname.substr(1, hostname.size() - 2);
        const unsigned short port = static_cast<unsigned short>(url.port().empty() ? 80 : std::stoi(url.port()));
        auto connection = std::make_shared<Connection>(rt::executor());
        const asio::ip::tcp::endpoint endpoint(asio::ip::make_address(hostname), port);
        boost::system::error_code error;
        auto connectToken = asio::redirect_error(asio::use_awaitable, error);
        co_await connection->socket.async_connect(endpoint, connectToken);
        if (error) throw rt::TypeError("Unable to connect. Is the computer able to access the url?");

        const bool chunked = lowerAscii(request.headers().get("transfer-encoding").value_or("")) == "chunked";
        std::string head = request.method() + " " + url.pathname() + url.search() + " HTTP/1.1\r\nHost: " + url.host() + "\r\n";
        for (const auto& [name, value] : request.headers()) {
          if (chunked && name == "content-length") continue;
          head += name + ": " + latin1Bytes(value) + "\r\n";
        }
        if (!request.headers().has("connection")) head += "Connection: keep-alive\r\n";
        if (chunked) {
          head += "\r\n";
          auto headToken = asio::redirect_error(asio::use_awaitable, error);
          co_await asio::async_write(connection->socket, asio::buffer(head), headToken);
          if (error) throw closedEarly();
          if (const auto stream = request.body()) {
            const auto reader = stream->getReader();
            for (;;) {
              const auto step = co_await reader.read();
              if (step.done) break;
              if (step.value->empty()) continue;
              std::array<char, 32> hex{};
              const auto converted = std::to_chars(hex.data(), hex.data() + hex.size(), step.value->size(), 16);
              const std::string frame = std::string(hex.data(), converted.ptr) + "\r\n" + std::string(step.value->begin(), step.value->end()) + "\r\n";
              auto chunkToken = asio::redirect_error(asio::use_awaitable, error);
              co_await asio::async_write(connection->socket, asio::buffer(frame), chunkToken);
              if (error) throw closedEarly();
            }
          }
          const std::string end = "0\r\n\r\n";
          auto endToken = asio::redirect_error(asio::use_awaitable, error);
          co_await asio::async_write(connection->socket, asio::buffer(end), endToken);
          if (error) throw closedEarly();
        } else {
          rt::Bytes body;
          if (const auto stream = request.body()) {
            const auto reader = stream->getReader();
            for (;;) {
              const auto step = co_await reader.read();
              if (step.done) break;
              body.insert(body.end(), step.value->begin(), step.value->end());
            }
          }
          const bool bodyless = request.method() == "GET" || request.method() == "HEAD";
          if ((!bodyless || !body.empty()) && !request.headers().has("content-length")) head += "Content-Length: " + std::to_string(body.size()) + "\r\n";
          const std::string wire = head + "\r\n" + std::string(body.begin(), body.end());
          auto writeToken = asio::redirect_error(asio::use_awaitable, error);
          co_await asio::async_write(connection->socket, asio::buffer(wire), writeToken);
          if (error) throw closedEarly();
        }

        int status = 0;
        std::string reason;
        rt::Headers headers;
        for (;;) {
          std::size_t end = connection->buffer.find("\r\n\r\n");
          while (end == std::string::npos) {
            const bool more = co_await fill(connection);
            if (!more) throw closedEarly();
            end = connection->buffer.find("\r\n\r\n");
          }
          const std::string block = connection->buffer.substr(0, end);
          connection->buffer.erase(0, end + 4);
          const std::vector<std::string> lines = rt::split(block, "\r\n");
          const std::size_t firstSpace = lines[0].find(' ');
          const std::size_t secondSpace = lines[0].find(' ', firstSpace + 1);
          status = std::stoi(lines[0].substr(firstSpace + 1, 3));
          reason = secondSpace == std::string::npos ? std::string() : latin1Text(lines[0].substr(secondSpace + 1));
          if (status >= 100 && status < 200) continue;
          for (std::size_t index = 1; index < lines.size(); ++index) {
            const std::size_t colon = lines[index].find(':');
            headers.append(lines[index].substr(0, colon), latin1Text(rt::trim(lines[index].substr(colon + 1))));
          }
          break;
        }
        auto state = std::make_shared<BodyState>();
        state->connection = connection;
        const std::optional<std::string> contentLength = headers.get("content-length");
        const std::optional<std::string> transferEncoding = headers.get("transfer-encoding");
        if (request.method() == "HEAD" || status == 204 || status == 304) state->mode = BodyState::Mode::none;
        else if (transferEncoding && lowerAscii(*transferEncoding).find("chunked") != std::string::npos) state->mode = BodyState::Mode::chunked;
        else if (contentLength) {
          state->mode = BodyState::Mode::length;
          state->remaining = std::stoull(*contentLength);
        } else {
          state->mode = BodyState::Mode::close;
        }
        rt::UnderlyingSource<rt::ByteChunk> source;
        source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) -> rt::Task<void> {
          auto reading = nextChunk(state);
          std::optional<rt::Bytes> chunk = co_await std::move(reading);
          if (!chunk) {
            state->connection->close();
            controller.close();
            co_return;
          }
          controller.enqueue(std::move(*chunk));
        };
        source.cancel = [state](rt::Reason) { state->connection->close(); };
        rt::QueuingStrategy strategy;
        strategy.highWaterMark = 0;
        const rt::ByteStream stream(std::move(source), strategy);
        rt::ResponseInit init;
        init.status = status;
        init.statusText = reason;
        init.headers = headers;
        rt::Response response(rt::BodyInit(stream), std::move(init));
        response.setFetchResult(request.url(), false);
        co_return response;
      }
    };
  }

  ScopedLoopbackFetch::ScopedLoopbackFetch() : previous_(rt::fetchTransport()) { rt::setFetchTransport(std::make_shared<LoopbackTransport>()); }

  ScopedLoopbackFetch::~ScopedLoopbackFetch() { rt::setFetchTransport(previous_); }

  // ---- RawClient -------------------------------------------------------------------------

  std::shared_ptr<RawClient> RawClient::connect(int port, std::string host) {
    auto client = std::make_shared<RawClient>(asio::ip::tcp::socket(rt::executor()));
    client->startConnect(asio::ip::tcp::endpoint(asio::ip::make_address(host), static_cast<unsigned short>(port)));
    return client;
  }

  RawClient::RawClient(asio::ip::tcp::socket socket) : socket_(std::move(socket)) {}

  void RawClient::on(const std::string& event, Handler handler) {
    const auto closing = closingListeners_.lock();
    (closing ? *closing : listeners_)[event].push_back(Entry{std::move(handler), false});
  }
  void RawClient::once(const std::string& event, Handler handler) {
    const auto closing = closingListeners_.lock();
    (closing ? *closing : listeners_)[event].push_back(Entry{std::move(handler), true});
  }

  void RawClient::emit(const std::string& event, const std::string& data) {
    const auto closing = closingListeners_.lock();
    auto& listeners = closing ? *closing : listeners_;
    const std::vector<Entry> snapshot = listeners[event];
    std::vector<Entry> kept;
    for (const Entry& entry : snapshot)
      if (!entry.once) kept.push_back(entry);
    listeners[event] = kept;
    for (const Entry& entry : snapshot) entry.handler(data);
  }

  void RawClient::startConnect(asio::ip::tcp::endpoint endpoint) {
    auto self = shared_from_this();
    socket_.async_connect(endpoint, [self](const boost::system::error_code& error) {
      if (self->destroyed_) return;
      if (error) return self->closeWith(error.message());
      self->connected_ = true;
      self->emit("connect", "");
      self->flush();
      self->read();
    });
  }

  void RawClient::read() {
    if (reading_ || paused_ || destroyed_ || !connected_) return;
    reading_ = true;
    auto self = shared_from_this();
    socket_.async_read_some(asio::buffer(buffer_), [self](const boost::system::error_code& error, std::size_t count) {
      self->reading_ = false;
      if (self->destroyed_) return;
      if (error) return self->closeWith(error == asio::error::eof ? std::string() : error.message());
      self->emit("data", latin1Text(std::string_view(self->buffer_.data(), count)));
      self->read();
    });
  }

  void RawClient::write(std::string data) {
    if (destroyed_) return;
    output_.push_back(std::move(data));
    flush();
  }

  void RawClient::flush() {
    if (writing_ || !connected_ || destroyed_ || output_.empty()) return;
    writing_ = true;
    auto self = shared_from_this();
    asio::async_write(socket_, asio::buffer(output_.front()), [self](const boost::system::error_code& error, std::size_t) {
      self->writing_ = false;
      if (self->destroyed_) return;
      if (error) return self->closeWith(error.message());
      self->output_.pop_front();
      self->flush();
    });
  }

  void RawClient::pause() { paused_ = true; }

  void RawClient::resume() {
    paused_ = false;
    read();
  }

  void RawClient::closeWith(const std::string& error) {
    if (closeEmitted_) return;
    destroyed_ = true;
    closeEmitted_ = true;
    boost::system::error_code ignored;
    socket_.close(ignored);
    const auto cleanup = rt::scopeExit([this] { listeners_.clear(); });
    if (!error.empty()) emit("error", error);
    emit("close", "");
  }

  void RawClient::destroy() {
    if (destroyed_) return;
    destroyed_ = true;
    boost::system::error_code ignored;
    socket_.close(ignored);
    auto self = shared_from_this();
    // The pending event owns its listeners. If the loop discards that event,
    // captures of the client must not leave a client/listener ownership cycle.
    auto listeners = std::make_shared<Listeners>(std::move(listeners_));
    closingListeners_ = listeners;
    asio::post(rt::executor(), [self, listeners] {
      if (self->closeEmitted_) return;
      self->closeEmitted_ = true;
      const auto cleanup = rt::scopeExit([self, listeners] { listeners->clear(); });
      self->emit("close", "");
    });
  }
}
