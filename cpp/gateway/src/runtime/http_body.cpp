#include "uos/runtime/http_body.hpp"

#include "uos/rt/prelude.hpp"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace uos::runtime::http_body {
  struct IncomingBody::State {
    std::shared_ptr<IncomingMessage> incoming;
    rt::AbortSignal signal;
    bool closed = false;
    bool cancelled = false;
    bool consumed = false;
    std::function<void()> cleanup = [] {};
    std::function<void()> dispose = [] {};
    // start(controller): the controller and the listeners its closures registered.
    std::optional<rt::ReadableStreamDefaultController<rt::ByteChunk>> controller;
    std::weak_ptr<rt::detail::StreamState<rt::ByteChunk>> streamState;
    IncomingListener dataListener;
    IncomingListener endListener;
    IncomingListener failedListener;
    rt::EventListener abortListener;

    State(std::shared_ptr<IncomingMessage> incoming_, rt::AbortSignal signal_) : incoming(std::move(incoming_)), signal(std::move(signal_)) {}

    std::optional<rt::ReadableStreamDefaultController<rt::ByteChunk>> liveController() const {
      if (controller) return controller;
      if (auto liveStream = streamState.lock()) return rt::ReadableStreamDefaultController<rt::ByteChunk>(std::move(liveStream));
      return std::nullopt;
    }

    void stop() {
      closed = true;
      // Successful EOF must leave native transport reads active for disconnects.
      if (!consumed) incoming->pause();
      cleanup();
    }
    void data(const IncomingEvent& chunk) {
      if (closed) return;
      const rt::ByteChunk* bytes = std::get_if<rt::ByteChunk>(&chunk);
      if (!bytes) {
        failed(rt::makeReason(rt::TypeError("HTTP request body did not provide bytes")));
        return;
      }
      const auto active = liveController();
      if (!active) return;
      active->enqueue(*bytes);
      if (active->desiredSize().value_or(0) <= 0) incoming->pause();
    }
    void end() {
      if (closed) return;
      const auto active = liveController();
      consumed = true;
      stop();
      if (active) active->close();
    }
    void failed(rt::Reason reason) {
      if (closed) return;
      const auto active = liveController();
      stop();
      if (active) active->error(std::move(reason));
    }
    void abort() { failed(signal.reason()); }
  };

  namespace {
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    // The reason an "error" event carries. A Reason holds no Uint8Array, so a
    // chunk stands in as the JSON array of its bytes.
    rt::Reason eventReason(const IncomingEvent& event) {
      if (const rt::Reason* reason = std::get_if<rt::Reason>(&event)) return *reason;
      if (const std::string* text = std::get_if<std::string>(&event)) return rt::thrown(json::value(*text));
      if (const rt::ByteChunk* bytes = std::get_if<rt::ByteChunk>(&event)) {
        json::array items;
        for (const std::uint8_t byte : *bytes) items.emplace_back(static_cast<std::int64_t>(byte));
        return rt::thrown(json::value(std::move(items)));
      }
      return nullptr;
    }

    // The underlying source's start(controller). The listeners hold the state
    // weakly: while the stream is readable its own algorithms keep the state
    // alive, and once it closed every closure returns at once.
    void start(const std::shared_ptr<IncomingBody::State>& state, Controller controller) {
      state->controller = std::move(controller);
      const std::weak_ptr<IncomingBody::State> weak = state;
      state->dataListener = IncomingListener([weak](const IncomingEvent& chunk) {
        if (const auto live = weak.lock()) live->data(chunk);
      });
      state->endListener = IncomingListener([weak](const IncomingEvent&) {
        if (const auto live = weak.lock()) live->end();
      });
      state->failedListener = IncomingListener([weak](const IncomingEvent& reason) {
        if (const auto live = weak.lock()) live->failed(eventReason(reason));
      });
      state->abortListener = rt::EventListener([weak] {
        if (const auto live = weak.lock()) live->abort();
      });
      IncomingBody::State* const self = state.get();
      state->cleanup = [self] {
        self->incoming->removeListener("data", self->dataListener);
        self->incoming->removeListener("end", self->endListener);
        self->incoming->removeListener("error", self->failedListener);
        self->signal.removeEventListener("abort", self->abortListener);
      };
      state->dispose = [self] { self->failed(rt::makeReason(rt::JsError("The HTTP response settled before the request body was consumed"))); };
      state->incoming->pause();
      state->incoming->on("data", state->dataListener);
      state->incoming->once("end", state->endListener);
      state->incoming->once("error", state->failedListener);
      state->signal.addEventListener("abort", state->abortListener, {.once = true});
      if (state->signal.aborted()) state->abort();
      else if (state->incoming->readableEnded()) state->end();
    }
  }

  IncomingBody::IncomingBody(rt::ReadableStream<rt::ByteChunk> stream_, std::shared_ptr<State> state_) : stream(std::move(stream_)), state(std::move(state_)) {}

  bool IncomingBody::cancelled() const { return state->cancelled; }

  bool IncomingBody::consumed() const { return state->consumed; }

  void IncomingBody::dispose() const { state->dispose(); }

  IncomingBody createIncomingBody(std::shared_ptr<IncomingMessage> incoming, rt::AbortSignal signal) {
    auto state = std::make_shared<IncomingBody::State>(std::move(incoming), std::move(signal));
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [state](Controller controller) { start(state, std::move(controller)); };
    source.pull = [state](Controller) {
      if (!state->closed) state->incoming->resume();
    };
    source.cancel = [state](rt::Reason) {
      state->cancelled = true;
      state->closed = true;
      state->incoming->pause();
      state->cleanup();
    };
    rt::ReadableStream<rt::ByteChunk> stream(std::move(source), rt::QueuingStrategy{.highWaterMark = 0});
    // The stream's algorithms own state. Keep its controller strongly only during
    // synchronous start, so an abandoned readable stream has no ownership cycle.
    state->streamState = stream.state();
    state->controller.reset();
    return IncomingBody(std::move(stream), std::move(state));
  }
}
