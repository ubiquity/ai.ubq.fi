#pragma once
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/stream.hpp"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace uos::runtime::http_body {
  // ---- `import type { IncomingMessage } from "node:http"` -----------------------
  // rt provides no node:http yet. These declare the part of node:http's
  // IncomingMessage (a node:stream Readable and EventEmitter) this module uses;
  // the HTTP server implementation derives from IncomingMessage.

  // What an IncomingMessage event hands its listener: nothing ("end"), a chunk
  // ("data": a Buffer, or a string after setEncoding) or the thrown value
  // ("error").
  using IncomingEvent = std::variant<std::monostate, rt::ByteChunk, std::string, rt::Reason>;

  // A node:events listener with identity: removeListener() removes exactly the
  // listener that was added. Copies share the identity.
  class IncomingListener {
  public:
    IncomingListener() = default;
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, IncomingListener> && std::is_invocable_v<std::decay_t<F>&, const IncomingEvent&>)
    IncomingListener(F&& callback) : callback_(std::make_shared<std::function<void(const IncomingEvent&)>>(std::forward<F>(callback))) {}
    void operator()(const IncomingEvent& event) const { (*callback_)(event); }
    explicit operator bool() const noexcept { return callback_ != nullptr; }
    friend bool operator==(const IncomingListener& left, const IncomingListener& right) noexcept { return left.callback_ == right.callback_; }

  private:
    std::shared_ptr<std::function<void(const IncomingEvent&)>> callback_;
  };

  class IncomingMessage {
  public:
    virtual ~IncomingMessage() = default;
    // `incoming.pause()` / `incoming.resume()`: flowing-mode control of "data".
    virtual void pause() = 0;
    virtual void resume() = 0;
    // `incoming.on(event, listener)` / `incoming.once(event, listener)`.
    virtual void on(std::string_view event, const IncomingListener& listener) = 0;
    virtual void once(std::string_view event, const IncomingListener& listener) = 0;
    // `incoming.removeListener(event, listener)`, a once listener not yet called included.
    virtual void removeListener(std::string_view event, const IncomingListener& listener) = 0;
    // `incoming.readableEnded`: "end" was emitted.
    virtual bool readableEnded() const = 0;
  };

  // ---- src/runtime/http-body.ts ----------------------------------------------------

  class IncomingBody {
  public:
    // The flags and dispose function `createIncomingBody` closes over.
    struct State;

    IncomingBody(rt::ReadableStream<rt::ByteChunk> stream_, std::shared_ptr<State> state_);

    rt::ReadableStream<rt::ByteChunk> stream;
    // `body.cancelled` / `body.consumed`: getters over the live state.
    bool cancelled() const;
    bool consumed() const;
    // `body.dispose()`
    void dispose() const;

  private:
    std::shared_ptr<State> state;
  };

  /** Cancelling application input must leave the socket available for its response. */
  IncomingBody createIncomingBody(std::shared_ptr<IncomingMessage> incoming, rt::AbortSignal signal);
}
