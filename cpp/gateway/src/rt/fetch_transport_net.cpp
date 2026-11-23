// The fetch transport's connections: a TCP socket on the rt loop with an
// optional TLS client layer over OpenSSL memory BIOs, the per-loop keep-alive
// pool and the transport state.
#include "fetch_transport_internal.hpp"

#include "tls_internal.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/process.hpp"

#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>

#include <openssl/err.h>
#include <sys/socket.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace uos::rt::detail::fetch {
  namespace asio = boost::asio;

  namespace {
    ReadStatus statusOf(const boost::system::error_code& error, bool open) {
      if (!open || error == asio::error::operation_aborted) return ReadStatus::aborted;
      if (error == asio::error::eof) return ReadStatus::eof;
      return ReadStatus::failed;
    }

    FetchError handshakeError(const std::string& url) { return FetchError("unknown certificate verification error", "UNKNOWN_CERTIFICATE_VERIFICATION_ERROR", url); }

    constexpr unsigned char kAlpn[] = {8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
  }

  // ---- connection -----------------------------------------------------------------

  Connection::Connection(asio::ip::tcp::socket socket, std::string key_, bool secure_)
    : key(std::move(key_)), secure(secure_), socket_(std::move(socket)), readBuffer_(65536) {}

  Connection::~Connection() {
    close();
    if (ssl_) SSL_free(ssl_);
  }

  void Connection::close() noexcept {
    unwatchIdle();
    if (!open_) return;
    open_ = false;
    boost::system::error_code ignored;
    socket_.close(ignored);
    if (stopUpload) stopUpload();
  }

  void Connection::watchIdle(double ms) {
    unwatchIdle();
    std::weak_ptr<Connection> weak = weak_from_this();
    idleTimer_ = createExactTimer(
        [weak] {
          auto self = weak.lock();
          if (!self) return;
          self->timedOut_ = true;
          self->close();
        },
        ms);
    idleTimer_.unref();
  }

  void Connection::unwatchIdle() noexcept {
    clearInterval(idleTimer_);
    idleTimer_ = Timeout();
  }

  void Connection::touch() {
    if (idleTimer_.active()) idleTimer_.refresh();
  }

  Task<Connection::HandshakeResult> Connection::handshake(SSL_CTX* context, std::string host, bool rejectUnauthorized, std::string url) {
    HandshakeResult result;
    ssl_ = SSL_new(context);
    if (!ssl_) throw std::runtime_error("SSL_new failed");
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    BIO_set_mem_eof_return(rbio_, -1);
    BIO_set_mem_eof_return(wbio_, -1);
    SSL_set_bio(ssl_, rbio_, wbio_);
    SSL_set_connect_state(ssl_);
    if (!tls::ipBytes(host)) SSL_set_tlsext_host_name(ssl_, host.c_str());
    SSL_set_alpn_protos(ssl_, kAlpn, sizeof kAlpn);
    for (;;) {
      const int step = SSL_do_handshake(ssl_);
      const int failure = step == 1 ? SSL_ERROR_NONE : SSL_get_error(ssl_, step);
      auto flushing = flushTls();
      const bool flushed = co_await std::move(flushing);
      if (!flushed) {
        result.aborted = !open_;
        if (open_) result.failure = makeReason(closedError(url));
        close();
        co_return result;
      }
      if (failure == SSL_ERROR_NONE) break;
      if (failure != SSL_ERROR_WANT_READ) {
        ERR_clear_error();
        result.failure = makeReason(handshakeError(url));
        close();
        co_return result;
      }
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const auto buffer = asio::buffer(readBuffer_);
      const std::size_t count = co_await socket_.async_read_some(buffer, token);
      if (error || count == 0) {
        const ReadStatus status = statusOf(error, open_);
        result.aborted = status == ReadStatus::aborted;
        if (status == ReadStatus::eof) result.failure = makeReason(handshakeError(url));
        else if (status == ReadStatus::failed) result.failure = makeReason(closedError(url));
        close();
        co_return result;
      }
      touch();
      BIO_write(rbio_, readBuffer_.data(), static_cast<int>(count));
    }
    if (!rejectUnauthorized) co_return result;
    const long verified = SSL_get_verify_result(ssl_);
    if (verified != X509_V_OK) {
      const auto [message, reasonCode] = tls::verifyErrorText(verified);
      result.failure = makeReason(FetchError(message, reasonCode, url));
      close();
      co_return result;
    }
    X509* certificate = SSL_get1_peer_certificate(ssl_);
    const bool matches = certificate != nullptr && !tls::checkServerIdentity(host, certificate);
    if (certificate) X509_free(certificate);
    if (!matches) {
      result.failure = makeReason(verboseError("ERR_TLS_CERT_ALTNAME_INVALID", url));
      close();
    }
    co_return result;
  }

  Task<ReadStatus> Connection::fill() {
    // Cancelling a body releases its connection before the socket completion runs.
    return rt::detail::keepAlive<ReadStatus>(shared_from_this(), readSome());
  }

  Task<ReadStatus> Connection::readSome() {
    if (!open_) co_return ReadStatus::aborted;
    if (!ssl_) {
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const auto buffer = asio::buffer(readBuffer_);
      const std::size_t count = co_await socket_.async_read_some(buffer, token);
      if (!open_) co_return ReadStatus::aborted;
      if (error) co_return statusOf(error, open_);
      if (count == 0) co_return ReadStatus::eof;
      touch();
      input.append(reinterpret_cast<const char*>(readBuffer_.data()), count);
      co_return ReadStatus::data;
    }
    for (;;) {
      bool received = false;
      for (;;) {
        char plain[16384];
        const int count = SSL_read(ssl_, plain, sizeof plain);
        if (count > 0) {
          input.append(plain, static_cast<std::size_t>(count));
          received = true;
          continue;
        }
        const int failure = SSL_get_error(ssl_, count);
        if (failure == SSL_ERROR_WANT_READ) break;
        ERR_clear_error();
        if (received) co_return ReadStatus::data;
        co_return failure == SSL_ERROR_ZERO_RETURN ? ReadStatus::eof : ReadStatus::failed;
      }
      if (received) co_return ReadStatus::data;
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const auto buffer = asio::buffer(readBuffer_);
      const std::size_t count = co_await socket_.async_read_some(buffer, token);
      if (!open_) co_return ReadStatus::aborted;
      if (error) co_return statusOf(error, open_);
      if (count == 0) co_return ReadStatus::eof;
      touch();
      BIO_write(rbio_, readBuffer_.data(), static_cast<int>(count));
    }
  }

  Task<bool> Connection::write(std::shared_ptr<const void> owner, const std::uint8_t* data, std::size_t size) {
    (void)owner;
    if (!open_) co_return false;
    if (!ssl_) {
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const auto buffer = asio::buffer(data, size);
      co_await asio::async_write(socket_, buffer, token);
      if (error || !open_) co_return false;
      touch();
      co_return true;
    }
    std::size_t offset = 0;
    while (offset < size) {
      const std::size_t slice = std::min<std::size_t>(size - offset, 65536);
      if (SSL_write(ssl_, data + offset, static_cast<int>(slice)) <= 0) {
        ERR_clear_error();
        close();
        co_return false;
      }
      offset += slice;
      auto flushing = flushTls();
      const bool flushed = co_await std::move(flushing);
      if (!flushed) co_return false;
    }
    co_return open_;
  }

  Task<bool> Connection::flushTls() {
    std::vector<std::uint8_t> cipher;
    while (open_) {
      const std::size_t pending = BIO_ctrl_pending(wbio_);
      if (pending == 0) co_return true;
      cipher.resize(pending);
      const int taken = BIO_read(wbio_, cipher.data(), static_cast<int>(pending));
      if (taken <= 0) co_return true;
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const auto buffer = asio::buffer(cipher.data(), static_cast<std::size_t>(taken));
      co_await asio::async_write(socket_, buffer, token);
      if (error) co_return false;
      touch();
    }
    co_return false;
  }

  bool Connection::idleHealthy() {
    if (!open_ || !input.empty()) return false;
    const int descriptor = socket_.native_handle();
    if (!ssl_) {
      char byte = 0;
      const ssize_t count = ::recv(descriptor, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
      return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
    }
    for (;;) {
      std::uint8_t cipher[16384];
      const ssize_t count = ::recv(descriptor, cipher, sizeof cipher, MSG_DONTWAIT);
      if (count > 0) {
        BIO_write(rbio_, cipher, static_cast<int>(count));
        continue;
      }
      if (count == 0) return false;
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) break;
      return false;
    }
    if (BIO_ctrl_pending(rbio_) == 0 && SSL_pending(ssl_) == 0) return true;
    char plain = 0;
    const int read = SSL_read(ssl_, &plain, 1);
    if (read > 0) return false;
    const int failure = SSL_get_error(ssl_, read);
    ERR_clear_error();
    return failure == SSL_ERROR_WANT_READ && BIO_ctrl_pending(rbio_) == 0;
  }

  bool Connection::hasBufferedInput() const {
    if (!input.empty()) return true;
    return ssl_ && (SSL_pending(ssl_) > 0 || BIO_ctrl_pending(rbio_) > 0);
  }

  // ---- pool -------------------------------------------------------------------------

  Pool::Pool(std::size_t capacity, double keepAliveTimeoutMs) : timeoutMs_(keepAliveTimeoutMs), plain_(capacity), secure_(capacity) {}

  Pool::~Pool() { closeAll(); }

  std::shared_ptr<Connection> Pool::acquire(const std::string& key, bool secure) {
    const double now = loop().clock().monotonicNow();
    for (auto& slot : slots(secure)) {
      if (!slot || slot->key != key) continue;
      std::shared_ptr<Connection> connection = std::move(slot);
      slot.reset();
      if (now - connection->idleSince >= timeoutMs_ || !connection->idleHealthy()) {
        connection->close();
        continue;
      }
      connection->reused = true;
      arm();
      return connection;
    }
    return nullptr;
  }

  void Pool::release(std::shared_ptr<Connection> connection) {
    if (!connection->isOpen()) return;
    for (auto& slot : slots(connection->secure)) {
      if (slot) continue;
      connection->unwatchIdle();
      connection->idleSince = loop().clock().monotonicNow();
      connection->reused = false;
      slot = std::move(connection);
      arm();
      return;
    }
    connection->close();
  }

  std::size_t Pool::idleCount() const noexcept {
    std::size_t count = 0;
    for (const auto& slot : plain_) count += slot ? 1 : 0;
    for (const auto& slot : secure_) count += slot ? 1 : 0;
    return count;
  }

  void Pool::closeAll() noexcept {
    for (auto* list : {&plain_, &secure_}) {
      for (auto& slot : *list) {
        if (!slot) continue;
        slot->close();
        slot.reset();
      }
    }
    clearInterval(timer_);
    timer_ = Timeout();
  }

  // One unreferenced timer per pool fires at the earliest expiry (idle
  // connections never keep the loop running, as in Bun).
  void Pool::arm() {
    double earliest = std::numeric_limits<double>::infinity();
    for (const auto* list : {&plain_, &secure_}) {
      for (const auto& slot : *list) {
        if (slot) earliest = std::min(earliest, slot->idleSince + timeoutMs_);
      }
    }
    if (std::isinf(earliest)) {
      clearInterval(timer_);
      timer_ = Timeout();
      return;
    }
    if (timer_.active() && timerDeadline_ <= earliest) return;
    clearInterval(timer_);
    timerDeadline_ = earliest;
    std::weak_ptr<Pool> weak = weak_from_this();
    const double delay = std::max(0.0, std::ceil(earliest - loop().clock().monotonicNow()));
    timer_ = createExactTimer(
        [weak] {
          if (auto pool = weak.lock()) pool->sweep();
        },
        delay);
    timer_.unref();
  }

  void Pool::sweep() {
    const double now = loop().clock().monotonicNow();
    for (auto* list : {&plain_, &secure_}) {
      for (auto& slot : *list) {
        if (!slot || now - slot->idleSince < timeoutMs_) continue;
        slot->close();
        slot.reset();
      }
    }
    timer_ = Timeout();
    arm();
  }

  PoolRegistry::PoolRegistry(asio::execution_context& context) : asio::execution_context::service(context) {}

  std::shared_ptr<Pool> PoolRegistry::pool(std::uint64_t owner, std::size_t capacity, double keepAliveTimeoutMs) {
    auto& entry = pools_[owner];
    if (!entry) entry = std::make_shared<Pool>(capacity, keepAliveTimeoutMs);
    return entry;
  }

  void PoolRegistry::remove(std::uint64_t owner) noexcept { pools_.erase(owner); }

  void PoolRegistry::shutdown() {
    auto pools = std::move(pools_);
    pools_.clear();
    pools.clear();
  }

  // ---- transport state ----------------------------------------------------------

  ClientState::ClientState(HttpClientOptions options_) : options(std::move(options_)) {
    static std::uint64_t nextId = 0;
    id = ++nextId;
    userAgent = options.userAgent ? *options.userAgent : "Bun/" + std::string(bun::version);
    if (options.ca) {
      context = tls::newContext();
      tls::addPemCertificates(SSL_CTX_get_cert_store(context), *options.ca);
    } else {
      context = tls::defaultContext();
      SSL_CTX_up_ref(context);
    }
  }

  ClientState::~ClientState() {
    for (const LoopPool& entry : pools) {
      if (auto pool = entry.pool.lock()) {
        pool->closeAll();
        entry.registry->remove(id);
      }
    }
    SSL_CTX_free(context);
  }

  std::shared_ptr<Pool> ClientState::pool() {
    auto& registry = asio::use_service<PoolRegistry>(static_cast<asio::execution_context&>(loop().context()));
    for (const LoopPool& entry : pools) {
      if (entry.registry != &registry) continue;
      if (auto pool = entry.pool.lock()) return pool;
    }
    std::erase_if(pools, [](const LoopPool& entry) { return entry.pool.expired(); });
    std::shared_ptr<Pool> created = registry.pool(id, options.maxIdleConnections, options.keepAliveTimeoutMs);
    pools.push_back(LoopPool{&registry, created});
    return created;
  }

  std::shared_ptr<Pool> ClientState::existingPool() const {
    auto& executionContext = static_cast<asio::execution_context&>(loop().context());
    if (!asio::has_service<PoolRegistry>(executionContext)) return nullptr;
    auto& registry = asio::use_service<PoolRegistry>(executionContext);
    for (const LoopPool& entry : pools) {
      if (entry.registry != &registry) continue;
      if (auto pool = entry.pool.lock()) return pool;
    }
    return nullptr;
  }

  bool ClientState::rejectUnauthorized() const {
    if (!options.rejectUnauthorized) return false;
    const std::optional<std::string> variable = env::get("NODE_TLS_REJECT_UNAUTHORIZED");
    return !(variable && *variable == "0");
  }
}
