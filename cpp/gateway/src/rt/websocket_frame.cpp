// ws's Receiver (lib/receiver.js) and Sender (lib/sender.js) without
// permessage-deflate, and the frame validation they share (lib/validation.js).
#include "websocket_internal.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace uos::rt::ws::detail {
  namespace {
    // `isValidStatusCode(code)`: allowed in a close frame.
    bool isValidStatusCode(int code) { return (code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006) || (code >= 3000 && code <= 4999); }

    std::uint32_t readUInt32BE(const Bytes& data, std::size_t offset) {
      return (static_cast<std::uint32_t>(data[offset]) << 24) | (static_cast<std::uint32_t>(data[offset + 1]) << 16) | (static_cast<std::uint32_t>(data[offset + 2]) << 8) |
             static_cast<std::uint32_t>(data[offset + 3]);
    }

    // `Buffer.from(data)` for a string (UTF-8, a lone surrogate as U+FFFD), the bytes otherwise.
    Bytes toBuffer(const Data& data) {
      if (const auto* text = std::get_if<std::string>(&data)) return encodeUtf8(*text);
      return std::get<Bytes>(data);
    }

    // The masking keys: ws draws them from an 8 KiB pool of random bytes refilled when used up.
    constexpr std::size_t RANDOM_POOL_SIZE = 8 * 1024;
    Bytes& randomPool() {
      static Bytes pool(RANDOM_POOL_SIZE);
      return pool;
    }
    std::size_t randomPoolPointer = RANDOM_POOL_SIZE;

    void nextMask(std::uint8_t (&mask)[4]) {
      if (randomPoolPointer == RANDOM_POOL_SIZE) {
        rt::crypto::getRandomValues(std::span<std::uint8_t>(randomPool()));
        randomPoolPointer = 0;
      }
      for (std::uint8_t& value : mask) value = randomPool()[randomPoolPointer++];
    }

    // An Error / RangeError from the receiver: its `code` and the close status ws sends for it.
    struct FrameError : CodedError {
      int statusCode;
      FrameError(std::string message_, std::string code_, std::string name_, int statusCode_) : CodedError(std::move(message_), std::move(code_), std::move(name_)), statusCode(statusCode_) {}
    };
  }

  // ---- Receiver ----------------------------------------------------------------

  Receiver::Receiver(Options options) : maxPayload_(toInt32(options.maxPayload)), isServer_(options.isServer), skipUTF8Validation_(options.skipUTF8Validation) {}

  void Receiver::write(BytesView chunk) {
    // A destroyed or ended Writable takes no more data (ws detaches its data
    // listener before either happens).
    if (errored_ || ending_) return;
    // `_write`: everything after a close frame is ignored.
    if (opcode_ == 0x08 && state_ == Step::getInfo) return;
    if (bufferOffset_ > 0 && bufferOffset_ == buffer_.size()) {
      buffer_.clear();
      bufferOffset_ = 0;
    }
    buffer_.insert(buffer_.end(), chunk.begin(), chunk.end());
    startLoop();
  }

  Bytes Receiver::consume(std::size_t count) {
    Bytes out(buffer_.begin() + static_cast<std::ptrdiff_t>(bufferOffset_), buffer_.begin() + static_cast<std::ptrdiff_t>(bufferOffset_ + count));
    bufferOffset_ += count;
    if (bufferOffset_ == buffer_.size()) {
      buffer_.clear();
      bufferOffset_ = 0;
    } else if (bufferOffset_ > 65536 && bufferOffset_ * 2 > buffer_.size()) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(bufferOffset_));
      bufferOffset_ = 0;
    }
    return out;
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
    const Bytes buf = consume(2);
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
    const Bytes buf = consume(2);
    payloadLength_ = (static_cast<std::uint64_t>(buf[0]) << 8) | buf[1];
    haveLength();
  }

  void Receiver::getPayloadLength64() {
    if (buffer_.size() - bufferOffset_ < 8) {
      loop_ = false;
      return;
    }
    const Bytes buf = consume(8);
    const std::uint32_t num = readUInt32BE(buf, 0);
    // The maximum safe integer in JavaScript is 2^53 - 1. An error is returned
    // if payload length is greater than this number.
    if (num > (1u << 21) - 1) return fail("RangeError", "Unsupported WebSocket frame: payload length > 2^53 - 1", false, 1009, "WS_ERR_UNSUPPORTED_DATA_PAYLOAD_LENGTH");
    payloadLength_ = (static_cast<std::uint64_t>(num) << 32) + readUInt32BE(buf, 4);
    haveLength();
  }

  void Receiver::haveLength() {
    if (payloadLength_ && opcode_ < 0x08) {
      totalPayloadLength_ += payloadLength_;
      if (maxPayload_ > 0 && totalPayloadLength_ > static_cast<std::uint64_t>(maxPayload_)) return fail("RangeError", "Max payload size exceeded", false, 1009, "WS_ERR_UNSUPPORTED_MESSAGE_LENGTH");
    }
    state_ = masked_ ? Step::getMask : Step::getData;
  }

  void Receiver::getMask() {
    if (buffer_.size() - bufferOffset_ < 4) {
      loop_ = false;
      return;
    }
    const Bytes buf = consume(4);
    std::copy(buf.begin(), buf.end(), mask_);
    state_ = Step::getData;
  }

  void Receiver::getData() {
    Bytes data;
    if (payloadLength_) {
      if (buffer_.size() - bufferOffset_ < payloadLength_) {
        loop_ = false;
        return;
      }
      data = consume(static_cast<std::size_t>(payloadLength_));
      if (masked_ && (mask_[0] | mask_[1] | mask_[2] | mask_[3]) != 0) {
        for (std::size_t index = 0; index < data.size(); ++index) data[index] ^= mask_[index & 3];
      }
    }
    if (opcode_ > 0x07) return controlMessage(std::move(data));
    if (!data.empty()) {
      messageLength_ = totalPayloadLength_;
      fragments_.push_back(std::move(data));
    }
    dataMessage();
  }

  void Receiver::dataMessage() {
    if (!fin_) {
      state_ = Step::getInfo;
      return;
    }
    std::vector<Bytes> fragments = std::move(fragments_);
    totalPayloadLength_ = 0;
    messageLength_ = 0;
    fragmented_ = 0;
    fragments_.clear();
    Bytes data;
    if (fragments.size() == 1) {
      data = std::move(fragments.front());
    } else {
      for (const Bytes& fragment : fragments) data.insert(data.end(), fragment.begin(), fragment.end());
    }
    const bool isBinary = opcode_ == 2;
    if (!isBinary && !skipUTF8Validation_ && !isUtf8(std::span<const std::uint8_t>(data))) return fail("Error", "invalid UTF-8 sequence", true, 1007, "WS_ERR_INVALID_UTF8");
    // ws resets its state right after the listeners return; a listener that
    // throws must not leave the parser mid-frame here.
    state_ = Step::getInfo;
    auto listener = onMessage;
    if (listener) listener(std::move(data), isBinary);
  }

  void Receiver::controlMessage(Bytes data) {
    if (opcode_ == 0x08) {
      if (data.empty()) {
        loop_ = false;
        state_ = Step::getInfo;
        auto listener = onConclude;
        if (listener) listener(1005, Bytes{});
        end();
      } else {
        const int code = (data[0] << 8) | data[1];
        if (!isValidStatusCode(code)) return fail("RangeError", "invalid status code " + std::to_string(code), true, 1002, "WS_ERR_INVALID_CLOSE_CODE");
        Bytes reason(data.begin() + 2, data.end());
        if (!skipUTF8Validation_ && !isUtf8(std::span<const std::uint8_t>(reason))) return fail("Error", "invalid UTF-8 sequence", true, 1007, "WS_ERR_INVALID_UTF8");
        loop_ = false;
        state_ = Step::getInfo;
        auto listener = onConclude;
        if (listener) listener(code, std::move(reason));
        end();
      }
      return;
    }
    state_ = Step::getInfo;
    auto listener = opcode_ == 0x09 ? onPing : onPong;
    if (listener) listener(std::move(data));
  }

  void Receiver::fail(std::string name, std::string message, bool prefix, int statusCode, std::string errorCode) {
    loop_ = false;
    errored_ = true;
    Reason error = std::make_exception_ptr(FrameError(prefix ? "Invalid WebSocket frame: " + message : std::move(message), std::move(errorCode), std::move(name), statusCode));
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
    std::uint8_t key[4] = {0, 0, 0, 0};
    std::size_t offset = 2;
    bool skipMasking = false;
    if (mask) {
      nextMask(key);
      skipMasking = (key[0] | key[1] | key[2] | key[3]) == 0;
      offset = 6;
    }
    const std::size_t dataLength = data.size();
    std::size_t payloadLength = dataLength;
    if (dataLength >= 65536) {
      offset += 8;
      payloadLength = 127;
    } else if (dataLength > 125) {
      offset += 2;
      payloadLength = 126;
    }
    Bytes target(offset + dataLength);
    target[0] = static_cast<std::uint8_t>(fin ? opcode | 0x80 : opcode);
    if (rsv1) target[0] |= 0x40;
    target[1] = static_cast<std::uint8_t>(payloadLength);
    if (payloadLength == 126) {
      target[2] = static_cast<std::uint8_t>(dataLength >> 8);
      target[3] = static_cast<std::uint8_t>(dataLength);
    } else if (payloadLength == 127) {
      const auto length = static_cast<std::uint64_t>(dataLength);
      for (int index = 0; index < 8; ++index) target[static_cast<std::size_t>(2 + index)] = static_cast<std::uint8_t>(length >> (8 * (7 - index)));
      target[2] = target[3] = 0;
    }
    std::copy(data.begin(), data.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
    if (!mask) return target;
    target[1] |= 0x80;
    std::copy(key, key + 4, target.begin() + static_cast<std::ptrdiff_t>(offset - 4));
    if (skipMasking) return target;
    for (std::size_t index = 0; index < dataLength; ++index) target[offset + index] ^= key[index & 3];
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
    int opcode = binary ? 2 : 1;
    if (firstFragment_) firstFragment_ = false;
    else opcode = 0;
    if (fin) firstFragment_ = true;
    const Bytes payload = toBuffer(data);
    sendFrame(frame(payload, opcode, fin, false, mask), std::move(cb));
  }

  void Sender::sendFrame(Bytes frame, Callback cb) { socket_->write(std::move(frame), std::move(cb)); }
}
