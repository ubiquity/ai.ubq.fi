// The node:net Socket model ws runs on (allowHalfOpen false, autoDestroy), and
// tls.connect's client side over OpenSSL memory BIOs, with Node's certificate
// checks (verifyError, then checkServerIdentity) and their error texts. The
// trust store and the checks are shared with the fetch transport (tls.cpp).
#include "websocket_internal.hpp"

#include "tls_internal.hpp"

#include <boost/asio/error.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/post.hpp>

#include <openssl/err.h>
#include <openssl/ssl.h>

#include <stdexcept>
#include <utility>

namespace asio = boost::asio;

namespace uos::rt::ws::detail {
  namespace tls = ::uos::rt::detail::tls;

  namespace {
    Reason destroyedError() { return codedError("Cannot call write after a stream was destroyed", "ERR_STREAM_DESTROYED"); }

    Reason tlsFailure() {
      const unsigned long code = ERR_peek_last_error();
      char text[256] = {0};
      ERR_error_string_n(code, text, sizeof text);
      const char* reason = ERR_reason_error_string(code);
      std::string name = reason ? reason : "unknown";
      for (char& c : name) {
        if (c == ' ') c = '_';
        else if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
      }
      ERR_clear_error();
      return codedError(text, "ERR_SSL_" + name);
    }
  }

  struct Socket::Tls {
    SSL* ssl = nullptr;
    BIO* input = nullptr;
    BIO* output = nullptr;
    bool handshaking = true;
    bool shutdownSent = false;
    TlsOptions options;
    std::function<void(Reason)> done;
    std::deque<std::pair<Bytes, Callback>> plain;
    std::size_t plainBytes = 0;
    Tls() = default;
    Tls(const Tls&) = delete;
    Tls& operator=(const Tls&) = delete;
    ~Tls() {
      if (ssl) SSL_free(ssl);
    }
  };

  Socket::Socket(Stream stream, bool readable, bool writable)
    : stream_(std::move(stream)), readBuffer_(64 * 1024), readableAtStart_(readable), writableAtStart_(writable) {
    boost::system::error_code ignored;
    stream_.non_blocking(true, ignored);
  }

  Socket::~Socket() = default;

  bool Socket::readable() const noexcept { return readableAtStart_ && !readEnded_ && !destroyed_; }
  bool Socket::writable() const noexcept { return writableAtStart_ && !writeEnded_ && !destroyed_; }

  void Socket::setNoDelay() {
    boost::system::error_code ignored;
    stream_.set_option(asio::ip::tcp::no_delay(true), ignored);
  }

  // ---- reading -----------------------------------------------------------------

  void Socket::resume() {
    flowing_ = true;
    if ((!held_.empty() || endHeld_) && !deliveryScheduled_) {
      deliveryScheduled_ = true;
      nextTick([self = shared_from_this()] {
        self->deliveryScheduled_ = false;
        while (self->flowing_ && !self->held_.empty() && !self->destroyed_) {
          Bytes chunk = std::move(self->held_.front());
          self->held_.pop_front();
          auto listener = self->onData;
          if (listener) guarded([&] { listener(chunk); });
        }
        if (self->flowing_ && self->held_.empty() && self->endHeld_ && !self->destroyed_) {
          self->endHeld_ = false;
          self->handleEof();
        }
      });
    }
    startReading();
  }

  void Socket::pause() { flowing_ = false; }

  void Socket::unshift(Bytes chunk) {
    if (!chunk.empty()) held_.push_front(std::move(chunk));
  }

  void Socket::startReading() {
    if (reading_ || readEnded_ || destroyed_ || !readableAtStart_) return;
    reading_ = true;
    readMore();
  }

  void Socket::readMore() {
    stream_.async_read_some(asio::buffer(readBuffer_), [self = shared_from_this()](const boost::system::error_code& error, std::size_t count) {
      guarded([&] { self->onRead(error, count); });
    });
  }

  void Socket::onRead(const boost::system::error_code& error, std::size_t count) {
    if (destroyed_) return;
    if (error == asio::error::eof || (!error && count == 0)) {
      reading_ = false;
      if (tls_ && tls_->handshaking) {
        auto done = std::move(tls_->done);
        tls_->done = nullptr;
        if (done) done(codedError("Client network socket disconnected before secure TLS connection was established", "ECONNRESET"));
        return;
      }
      if (!flowing_ || !held_.empty()) {
        endHeld_ = true;
        return;
      }
      handleEof();
      return;
    }
    if (error) {
      reading_ = false;
      if (error == asio::error::operation_aborted) return;
      destroy(systemError(error.value(), "read", ""));
      return;
    }
    if (auto activity = onActivity) activity();
    if (tls_) {
      BIO_write(tls_->input, readBuffer_.data(), static_cast<int>(count));
      pumpTls();
    } else {
      deliver(BytesView(readBuffer_.data(), count));
    }
    if (!destroyed_ && !readEnded_) readMore();
    else reading_ = false;
  }

  void Socket::deliver(BytesView chunk) {
    if (!flowing_ || !held_.empty()) {
      held_.emplace_back(chunk.begin(), chunk.end());
      return;
    }
    auto listener = onData;
    if (listener) guarded([&] { listener(chunk); });
  }

  void Socket::handleEof() {
    if (readEnded_ || destroyed_) return;
    readEnded_ = true;
    if (auto listener = onEnd) guarded([&] { listener(); });
    if (destroyed_) return;
    // allowHalfOpen is false: the socket ends its side too.
    end();
    maybeDestroy();
  }

  // ---- writing -----------------------------------------------------------------

  void Socket::write(Bytes data, Callback cb) {
    if (destroyed_ || writeEnded_) {
      Reason error = destroyed_ ? destroyedError() : codedError("write after end", "ERR_STREAM_WRITE_AFTER_END");
      if (cb) nextTick([cb = std::move(cb), error] { cb(error); });
      return;
    }
    if (tls_) {
      tlsWritePlain(std::move(data), std::move(cb));
      return;
    }
    const std::size_t size = data.size();
    queueRaw(std::move(data), size, std::move(cb));
  }

  std::size_t Socket::writableLength() const noexcept { return queued_ + (tls_ ? tls_->plainBytes : 0); }

  void Socket::queueRaw(Bytes data, std::size_t accounted, Callback callback) {
    queued_ += accounted;
    queue_.push_back(Pending{std::move(data), 0, accounted, std::move(callback)});
    if (!writing_) flush();
  }

  void Socket::flush() {
    while (!queue_.empty() && !writing_ && !destroyed_) {
      Pending& front = queue_.front();
      if (front.offset < front.data.size()) {
        const auto buffer = asio::buffer(front.data.data() + front.offset, front.data.size() - front.offset);
        boost::system::error_code error;
        const std::size_t written = stream_.write_some(buffer, error);
        if (error == asio::error::would_block || error == asio::error::try_again) {
          // Closing can clear queue_ before the completion releases the buffer.
          writing_ = true;
          stream_.async_write_some(buffer, [self = shared_from_this(), data = std::move(front.data)](const boost::system::error_code& failure, std::size_t count) mutable {
            guarded([&] { self->onWritten(failure, count, std::move(data)); });
          });
          return;
        }
        if (error) {
          destroy(systemError(error.value(), "write", ""));
          return;
        }
        front.offset += written;
        if (auto activity = onActivity) activity();
        if (front.offset < front.data.size()) continue;
      }
      Callback callback = std::move(front.callback);
      queued_ -= front.accounted;
      queue_.pop_front();
      // A write the kernel took at once reports after the current microtasks
      // (Bun runs these callbacks after queueMicrotask callbacks queued with them).
      if (callback) asio::post(stream_.get_executor(), [callback = std::move(callback)] { guarded([&] { callback(nullptr); }); });
    }
    if (queue_.empty() && !writing_ && writeEnded_ && !writeFinished_ && !destroyed_) finishEnd();
  }

  void Socket::onWritten(const boost::system::error_code& error, std::size_t count, Bytes data) {
    writing_ = false;
    if (destroyed_) return;
    if (error) {
      if (error == asio::error::operation_aborted) return;
      destroy(systemError(error.value(), "write", ""));
      return;
    }
    if (auto activity = onActivity) activity();
    Pending& front = queue_.front();
    front.data = std::move(data);
    front.offset += count;
    if (front.offset == front.data.size()) {
      Callback callback = std::move(front.callback);
      queued_ -= front.accounted;
      queue_.pop_front();
      if (callback) callback(nullptr);
      if (destroyed_) return;
    }
    flush();
  }

  void Socket::end() {
    if (writeEnded_ || destroyed_) return;
    writeEnded_ = true;
    if (queue_.empty() && !writing_ && (!tls_ || (!tls_->handshaking && tls_->plain.empty()))) finishEnd();
  }

  void Socket::finishEnd() {
    if (writeFinished_ || destroyed_) return;
    if (tls_ && !tls_->shutdownSent) {
      // close_notify first; flush() comes back here once it is written.
      tls_->shutdownSent = true;
      SSL_shutdown(tls_->ssl);
      ERR_clear_error();
      drainTlsOutput();
      if (queue_.empty() && !writing_) finishEnd();
      return;
    }
    boost::system::error_code ignored;
    stream_.shutdown(asio::socket_base::shutdown_send, ignored);
    writeFinished_ = true;
    nextTick([self = shared_from_this()] {
      if (self->destroyed_) return;
      if (auto listener = self->onFinish) listener();
      self->maybeDestroy();
    });
  }

  void Socket::maybeDestroy() {
    if (readEnded_ && writeFinished_ && !destroyed_) destroy();
  }

  void Socket::destroy(Reason error) {
    if (destroyed_) return;
    destroyed_ = true;
    boost::system::error_code ignored;
    stream_.close(ignored);
    std::deque<Pending> pending = std::move(queue_);
    queue_.clear();
    queued_ = 0;
    for (Pending& item : pending) {
      if (item.callback) nextTick([callback = std::move(item.callback)] { callback(destroyedError()); });
    }
    if (tls_) {
      for (auto& item : tls_->plain) {
        if (item.second) nextTick([callback = std::move(item.second)] { callback(destroyedError()); });
      }
      tls_->plain.clear();
      tls_->plainBytes = 0;
    }
    asio::post(stream_.get_executor(), [self = shared_from_this(), error] { guarded([&] { self->emitCloseEvents(error); }); });
  }

  void Socket::emitCloseEvents(Reason error) {
    if (error) {
      if (auto listener = onError) guarded([&] { listener(error); });
    }
    if (auto listener = onClose) guarded([&] { listener(); });
    releaseListeners();
  }

  void Socket::shutdown() {
    destroyed_ = true;
    boost::system::error_code ignored;
    stream_.close(ignored);
    queue_.clear();
    if (tls_) {
      tls_->done = nullptr;
      tls_->plain.clear();
    }
    releaseListeners();
  }

  void Socket::releaseListeners() {
    onData = nullptr;
    onEnd = nullptr;
    onError = nullptr;
    onClose = nullptr;
    onFinish = nullptr;
    onActivity = nullptr;
  }

  // ---- TLS ---------------------------------------------------------------------

  void Socket::startTls(TlsOptions options, std::function<void(Reason)> done) {
    SSL_CTX* context = nullptr;
    if (options.ca) {
      context = tls::newContext();
      tls::addPemCertificates(SSL_CTX_get_cert_store(context), *options.ca);
    } else {
      context = tls::defaultContext();
      SSL_CTX_up_ref(context);
    }
    tls_ = std::make_unique<Tls>();
    tls_->ssl = SSL_new(context);
    SSL_CTX_free(context);
    if (!tls_->ssl) throw std::runtime_error("SSL_new failed");
    tls_->input = BIO_new(BIO_s_mem());
    tls_->output = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(tls_->input, -1);
    BIO_set_mem_eof_return(tls_->output, -1);
    SSL_set_bio(tls_->ssl, tls_->input, tls_->output);
    SSL_set_connect_state(tls_->ssl);
    const std::string servername = options.servername ? *options.servername : (tls::ipBytes(options.host) ? std::string() : options.host);
    if (!servername.empty()) SSL_set_tlsext_host_name(tls_->ssl, servername.c_str());
    tls_->options = std::move(options);
    tls_->done = std::move(done);
    startReading();
    pumpTls();
  }

  void Socket::tlsHandshakeStep() {
    const int result = SSL_do_handshake(tls_->ssl);
    if (result != 1) {
      const int failure = SSL_get_error(tls_->ssl, result);
      if (failure == SSL_ERROR_WANT_READ || failure == SSL_ERROR_WANT_WRITE) return;
      Reason error = tlsFailure();
      auto done = std::move(tls_->done);
      tls_->done = nullptr;
      if (done) done(error);
      return;
    }
    tls_->handshaking = false;
    Reason verifyError;
    const long verified = SSL_get_verify_result(tls_->ssl);
    if (verified != X509_V_OK) {
      auto [message, code] = tls::verifyErrorText(verified);
      CodedError error(message, code);
      error.errno_ = 0;
      verifyError = std::make_exception_ptr(std::move(error));
    } else {
      const std::string hostname = tls_->options.servername && !tls_->options.servername->empty() ? *tls_->options.servername : tls_->options.host;
      X509* certificate = SSL_get1_peer_certificate(tls_->ssl);
      if (certificate) {
        if (auto reason = tls::checkServerIdentity(hostname, certificate)) verifyError = codedError("Hostname/IP does not match certificate's altnames: " + *reason, "ERR_TLS_CERT_ALTNAME_INVALID");
        X509_free(certificate);
      } else {
        verifyError = codedError("Cert does not contain a DNS name", "ERR_TLS_CERT_ALTNAME_INVALID");
      }
    }
    auto done = std::move(tls_->done);
    tls_->done = nullptr;
    if (verifyError && tls_->options.rejectUnauthorized) {
      if (done) done(verifyError);
      return;
    }
    auto plain = std::move(tls_->plain);
    tls_->plain.clear();
    tls_->plainBytes = 0;
    for (auto& [data, callback] : plain) tlsWritePlain(std::move(data), std::move(callback));
    if (done) done(nullptr);
    if (writeEnded_ && queue_.empty() && !writing_ && !writeFinished_ && !destroyed_) finishEnd();
  }

  void Socket::tlsWritePlain(Bytes data, Callback callback) {
    if (tls_->handshaking) {
      tls_->plainBytes += data.size();
      tls_->plain.emplace_back(std::move(data), std::move(callback));
      return;
    }
    const std::size_t size = data.size();
    if (size > 0 && SSL_write(tls_->ssl, data.data(), static_cast<int>(size)) <= 0) {
      destroy(tlsFailure());
      if (callback) nextTick([callback = std::move(callback)] { callback(destroyedError()); });
      return;
    }
    Bytes cipher;
    while (const std::size_t pending = BIO_ctrl_pending(tls_->output)) {
      const std::size_t offset = cipher.size();
      cipher.resize(offset + pending);
      BIO_read(tls_->output, cipher.data() + offset, static_cast<int>(pending));
    }
    queueRaw(std::move(cipher), size, std::move(callback));
  }

  void Socket::pumpTls() {
    if (tls_->handshaking) {
      tlsHandshakeStep();
    }
    if (!tls_->handshaking && !destroyed_) {
      std::uint8_t plain[16 * 1024];
      for (;;) {
        const int count = SSL_read(tls_->ssl, plain, sizeof plain);
        if (count > 0) {
          deliver(BytesView(plain, static_cast<std::size_t>(count)));
          if (destroyed_) return;
          continue;
        }
        const int failure = SSL_get_error(tls_->ssl, count);
        if (failure == SSL_ERROR_WANT_READ || failure == SSL_ERROR_WANT_WRITE) break;
        if (failure == SSL_ERROR_ZERO_RETURN) {
          ERR_clear_error();
          if (!flowing_ || !held_.empty()) endHeld_ = true;
          else handleEof();
          break;
        }
        destroy(tlsFailure());
        return;
      }
    }
    if (destroyed_) return;
    drainTlsOutput();
  }

  void Socket::drainTlsOutput() {
    Bytes cipher;
    while (const std::size_t pending = BIO_ctrl_pending(tls_->output)) {
      const std::size_t offset = cipher.size();
      cipher.resize(offset + pending);
      BIO_read(tls_->output, cipher.data() + offset, static_cast<int>(pending));
    }
    if (!cipher.empty()) queueRaw(std::move(cipher), 0, nullptr);
  }
}
