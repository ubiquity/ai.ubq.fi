// WebSocket framing and validation for the gateway's event-driven transport.
#include "websocket_internal.hpp"

#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/text_codec.hpp"

#include <boost/beast/websocket/detail/frame.hpp>
#include <boost/beast/websocket/detail/mask.hpp>
#include <boost/beast/websocket/detail/prng.hpp>
#include <boost/endian/conversion.hpp>

#include <algorithm>
#include <string>
#include <utility>

namespace uos::rt::ws::detail {
  namespace {
    namespace beastWs = boost::beast::websocket::detail;

    bool isValidStatusCode(int code) {
      // IANA registers 1014 (Bad Gateway). Beast only recognizes it starting in Boost 1.92.
      return code == 1014 || (code >= 0 && code <= 4999 && beastWs::is_valid_close_code(static_cast<std::uint16_t>(code)));
    }

    bool validUtf8(BytesView bytes) {
      return bytes.empty() || beastWs::check_utf8(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    }

    // `Buffer.from(data)` for a string (UTF-8, a lone surrogate as U+FFFD), the bytes otherwise.
    Bytes toBuffer(const Data& data) {
      if (std::visit([](const auto& value) { return value.size(); }, data) > limits::kWebSocketMessageBytes)
        throw RangeError("Max payload size exceeded");
      if (const auto* text = std::get_if<std::string>(&data)) return encodeUtf8(*text);
      return std::get<Bytes>(data);
    }
  }

  // ---- Receiver ----------------------------------------------------------------

  Receiver::Receiver(Options options) : maxPayload_(limits::kWebSocketMessageBytes), isServer_(options.isServer) {
    const auto requested = toInt32(options.maxPayload);
    if (requested > 0) maxPayload_ = std::min(maxPayload_, static_cast<std::size_t>(requested));
  }

  void Receiver::write(BytesView chunk) {
    // A destroyed or ended Writable takes no more data (ws detaches its data
    // listener before either happens).
    if (errored_ || ending_) return;
    // `_write`: everything after a close frame is ignored.
    if (opcode_ == 0x08 && state_ == Step::getInfo) return;
    if (bufferOffset_ == buffer_.size()) {
      buffer_.clear();
      bufferOffset_ = 0;
    } else if (bufferOffset_ > 65536 && bufferOffset_ > buffer_.size() / 2) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(bufferOffset_));
      bufferOffset_ = 0;
    }
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    startLoop();
  }

  BytesView Receiver::consume(std::size_t count) {
    const auto bytes = BytesView(buffer_).subspan(bufferOffset_, count);
    bufferOffset_ += count;
    return bytes;
  }

  void Receiver::startLoop() {
    loop_ = true;
    do {
      switch (state_) {
        case Step::getInfo:
          getInfo();
          break;
        case Step::getPayloadLength16:
          getPayloadLength16();
          break;
        case Step::getPayloadLength64:
          getPayloadLength64();
          break;
        case Step::getMask:
          getMask();
          break;
        case Step::getData:
          getData();
          break;
      }
    } while (loop_);
  }

  void Receiver::getInfo() {
    if (buffer_.size() - bufferOffset_ < 2) {
      loop_ = false;
      return;
    }
    const BytesView buf = consume(2);
    if ((buf[0] & 0x30) != 0x00) return fail("RangeError", "RSV2 and RSV3 must be clear", true, 1002, "WS_ERR_UNEXPECTED_RSV_2_3");
    // RSV1 marks a compressed message; no extension is ever negotiated.
    if ((buf[0] & 0x40) == 0x40) return fail("RangeError", "RSV1 must be clear", true, 1002, "WS_ERR_UNEXPECTED_RSV_1");
    fin_ = (buf[0] & 0x80) == 0x80;
    opcode_ = buf[0] & 0x0f;
    payloadLength_ = buf[1] & 0x7f;
    if (opcode_ == 0x00) {
      if (!fragmented_) return fail("RangeError", "invalid opcode 0", true, 1002, "WS_ERR_INVALID_OPCODE");
      opcode_ = fragmented_;
    } else if (opcode_ == 0x01 || opcode_ == 0x02) {
      if (fragmented_) return fail("RangeError", "invalid opcode " + std::to_string(opcode_), true, 1002, "WS_ERR_INVALID_OPCODE");
    } else if (opcode_ > 0x07 && opcode_ < 0x0b) {
      if (!fin_) return fail("RangeError", "FIN must be set", true, 1002, "WS_ERR_EXPECTED_FIN");
      if (payloadLength_ > 0x7d || (opcode_ == 0x08 && payloadLength_ == 1))
        return fail("RangeError", "invalid payload length " + std::to_string(payloadLength_), true, 1002, "WS_ERR_INVALID_CONTROL_PAYLOAD_LENGTH");
    } else {
      return fail("RangeError", "invalid opcode " + std::to_string(opcode_), true, 1002, "WS_ERR_INVALID_OPCODE");
    }
    if (!fin_ && !fragmented_) fragmented_ = opcode_;
    masked_ = (buf[1] & 0x80) == 0x80;
    if (isServer_) {
      if (!masked_) return fail("RangeError", "MASK must be set", true, 1002, "WS_ERR_EXPECTED_MASK");
    } else if (masked_) {
      return fail("RangeError", "MASK must be clear", true, 1002, "WS_ERR_UNEXPECTED_MASK");
    }
    if (payloadLength_ == 126) state_ = Step::getPayloadLength16;
    else if (payloadLength_ == 127) state_ = Step::getPayloadLength64;
    else haveLength();
  }

  void Receiver::getPayloadLength16() {
    if (buffer_.size() - bufferOffset_ < 2) {
      loop_ = false;
      return;
    }
    const BytesView buf = consume(2);
    payloadLength_ = boost::endian::load_big_u16(buf.data());
    haveLength();
  }

  void Receiver::getPayloadLength64() {
    if (buffer_.size() - bufferOffset_ < 8) {
      loop_ = false;
      return;
    }
    const BytesView buf = consume(8);
    payloadLength_ = boost::endian::load_big_u64(buf.data());
    // The maximum safe integer in JavaScript is 2^53 - 1. An error is returned
    // if payload length is greater than this number.
    if (payloadLength_ >= (std::uint64_t{1} << 53))
      return fail("RangeError", "Unsupported WebSocket frame: payload length > 2^53 - 1", false, 1009, "WS_ERR_UNSUPPORTED_DATA_PAYLOAD_LENGTH");
    haveLength();
  }

  void Receiver::haveLength() {
    if (opcode_ < 0x08) {
      if (payloadLength_ > maxPayload_ - totalPayloadLength_)
        return fail("RangeError", "Max payload size exceeded", false, 1009, "WS_ERR_UNSUPPORTED_MESSAGE_LENGTH");
      if (++dataFrames_ > limits::kWebSocketFragments)
        return fail("RangeError", "Max fragment count exceeded", false, 1009, "WS_ERR_UNSUPPORTED_MESSAGE_LENGTH");
      totalPayloadLength_ += static_cast<std::size_t>(payloadLength_);
    }
    state_ = masked_ ? Step::getMask : Step::getData;
  }

  void Receiver::getMask() {
    if (buffer_.size() - bufferOffset_ < 4) {
      loop_ = false;
      return;
    }
    const BytesView buf = consume(4);
    beastWs::prepare_key(mask_, boost::endian::load_little_u32(buf.data()));
    state_ = Step::getData;
  }

  void Receiver::getData() {
    Bytes data;
    if (payloadLength_) {
      if (buffer_.size() - bufferOffset_ < payloadLength_) {
        loop_ = false;
        return;
      }
      const BytesView payload = consume(static_cast<std::size_t>(payloadLength_));
      data.assign(payload.begin(), payload.end());
      if (masked_) beastWs::mask_inplace(boost::asio::buffer(data), mask_);
    }
    if (opcode_ > 0x07) return controlMessage(std::move(data));
    if (!data.empty()) fragments_.push_back(std::move(data));
    dataMessage();
  }

  void Receiver::dataMessage() {
    if (!fin_) {
      state_ = Step::getInfo;
      return;
    }
    std::vector<Bytes> fragments = std::move(fragments_);
    totalPayloadLength_ = 0;
    dataFrames_ = 0;
    fragmented_ = 0;
    fragments_.clear();
    Bytes data;
    if (fragments.size() == 1) {
      data = std::move(fragments.front());
    } else {
      for (const Bytes& fragment : fragments) data.insert(data.end(), fragment.begin(), fragment.end());
    }
    const bool isBinary = opcode_ == 2;
    if (!isBinary && !validUtf8(data)) return fail("Error", "invalid UTF-8 sequence", true, 1007, "WS_ERR_INVALID_UTF8");
    // ws resets its state right after the listeners return; a listener that
    // throws must not leave the parser mid-frame here.
    state_ = Step::getInfo;
    auto listener = onMessage;
    if (listener) listener(std::move(data), isBinary);
  }

  void Receiver::controlMessage(Bytes data) {
    if (opcode_ == 0x08) {
      int code = 1005;
      // getInfo rejects a one-byte close payload before reaching this point.
      if (!data.empty()) {
        code = boost::endian::load_big_u16(data.data());
        if (!isValidStatusCode(code)) return fail("RangeError", "invalid status code " + std::to_string(code), true, 1002, "WS_ERR_INVALID_CLOSE_CODE");
        if (!validUtf8(BytesView(data).subspan(2))) return fail("Error", "invalid UTF-8 sequence", true, 1007, "WS_ERR_INVALID_UTF8");
        data.erase(data.begin(), data.begin() + 2);
      }
      loop_ = false;
      state_ = Step::getInfo;
      auto listener = onConclude;
      if (listener) listener(code, std::move(data));
      end();
      return;
    }
    state_ = Step::getInfo;
    auto listener = opcode_ == 0x09 ? onPing : onPong;
    if (listener) listener(std::move(data));
  }

  void Receiver::fail(std::string name, std::string message, bool prefix, int statusCode, std::string errorCode) {
    loop_ = false;
    errored_ = true;
    Reason error = codedError(prefix ? "Invalid WebSocket frame: " + message : std::move(message), std::move(errorCode), std::move(name));
    // Writable.destroy(error): "error" is emitted on the next tick.
    nextTick([self = shared_from_this(), error, statusCode] {
      self->errorEmitted_ = true;
      auto listener = self->onError;
      if (listener) guarded([&] { listener(error, statusCode); });
      auto finish = self->onFinish;
      if (finish) guarded([&] { finish(); });
    });
  }

  void Receiver::end() {
    if (errored_ || ending_) return;
    ending_ = true;
    nextTick([self = shared_from_this()] {
      self->finished_ = true;
      auto listener = self->onFinish;
      if (listener) guarded([&] { listener(); });
    });
  }

  void Receiver::removeAllListeners() {
    onConclude = nullptr;
    onError = nullptr;
    onMessage = nullptr;
    onPing = nullptr;
    onPong = nullptr;
    onFinish = nullptr;
  }

  // ---- Sender ------------------------------------------------------------------

  Sender::Sender(std::shared_ptr<Socket> socket) : socket_(std::move(socket)) {}

  Bytes Sender::frame(BytesView data, int opcode, bool fin, bool rsv1, bool mask) {
    if (data.size() > limits::kWebSocketMessageBytes) throw RangeError("Max payload size exceeded");
    const auto maskKey = mask ? beastWs::make_prng(true)() : 0;
    const beastWs::frame_header header{data.size(), maskKey, static_cast<beastWs::opcode>(opcode), fin, mask, rsv1, false, false};
    beastWs::fh_buffer serialized;
    beastWs::write(serialized, header);
    const auto bytes = serialized.data();
    const auto* start = static_cast<const std::uint8_t*>(bytes.data());
    Bytes target;
    target.reserve(bytes.size() + data.size());
    target.insert(target.end(), start, start + bytes.size());
    target.insert(target.end(), data.begin(), data.end());
    if (mask && !data.empty()) {
      beastWs::prepared_key key;
      beastWs::prepare_key(key, maskKey);
      beastWs::mask_inplace(boost::asio::buffer(target) + bytes.size(), key);
    }
    return target;
  }

  void Sender::close(std::optional<int> code, const std::optional<Data>& data, bool mask, Callback cb) {
    Bytes buf;
    if (!code) {
      // An empty close frame.
    } else if (!isValidStatusCode(*code)) {
      throw TypeError("First argument must be a valid error code number");
    } else {
      const bool empty = !data || std::visit([](const auto& value) { return value.empty(); }, *data);
      buf = {static_cast<std::uint8_t>(*code >> 8), static_cast<std::uint8_t>(*code & 0xff)};
      if (!empty) {
        const Bytes reason = toBuffer(*data);
        if (reason.size() > 123) throw RangeError("The message must not be greater than 123 bytes");
        buf.insert(buf.end(), reason.begin(), reason.end());
      }
    }
    sendFrame(frame(buf, 0x08, true, false, mask), std::move(cb));
  }

  void Sender::ping(const Data& data, bool mask, Callback cb) {
    const Bytes payload = toBuffer(data);
    if (payload.size() > 125) throw RangeError("The data size must not be greater than 125 bytes");
    sendFrame(frame(payload, 0x09, true, false, mask), std::move(cb));
  }

  void Sender::pong(const Data& data, bool mask, Callback cb) {
    const Bytes payload = toBuffer(data);
    if (payload.size() > 125) throw RangeError("The data size must not be greater than 125 bytes");
    sendFrame(frame(payload, 0x0a, true, false, mask), std::move(cb));
  }

  void Sender::send(const Data& data, bool binary, bool mask, bool fin, Callback cb) {
    const Bytes payload = toBuffer(data);
    if (payload.size() > limits::kWebSocketMessageBytes - messageBytes_) throw RangeError("Max payload size exceeded");
    if (dataFrames_ == limits::kWebSocketFragments) throw RangeError("Max fragment count exceeded");
    const int opcode = firstFragment_ ? (binary ? 2 : 1) : 0;
    auto encoded = frame(payload, opcode, fin, false, mask);
    firstFragment_ = fin;
    messageBytes_ = fin ? 0 : messageBytes_ + payload.size();
    dataFrames_ = fin ? 0 : dataFrames_ + 1;
    sendFrame(std::move(encoded), std::move(cb));
  }

  void Sender::sendFrame(Bytes frame, Callback cb) { socket_->write(std::move(frame), std::move(cb)); }
}
