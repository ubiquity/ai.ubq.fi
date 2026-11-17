// Differential cases for src/runtime/http-body.ts (no direct bun test). Each
// scenario drives createIncomingBody with a fake IncomingMessage that follows
// node:events semantics; the expected event logs (event@microtask turn) were
// recorded by running the same scenarios under bun 1.4.0 with a node:events
// EventEmitter fake. `await helper()` of a scenario's async helpers is
// `co_await rt::Promise(helper()).wait()`, which takes the turn JS takes.
#include "uos/runtime/http_body.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::runtime::http_body::createIncomingBody;
using uos::runtime::http_body::IncomingBody;
using uos::runtime::http_body::IncomingEvent;
using uos::runtime::http_body::IncomingListener;
using uos::runtime::http_body::IncomingMessage;

namespace {
  using Reader = rt::ReadableStreamDefaultReader<rt::ByteChunk>;

  // Counts microtask turns while `live`, and records `event@turn`.
  struct Recorder {
    int tick = 0;
    bool live = true;
    std::string events;
    void log(const std::string& event) { events += (events.empty() ? "" : " | ") + event + "@" + std::to_string(tick); }
  };

  void tickOnce(const std::shared_ptr<Recorder>& recorder) {
    if (!recorder->live) return;
    recorder->tick += 1;
    rt::queueMicrotask([recorder] { tickOnce(recorder); });
  }

  // A node:http IncomingMessage stand-in with node:events semantics: emit()
  // calls a snapshot of the listeners in order, a once listener removes itself
  // before it runs, removeListener() removes the last matching registration.
  class FakeIncoming final : public IncomingMessage {
  public:
    explicit FakeIncoming(std::shared_ptr<Recorder> recorder_) : recorder(std::move(recorder_)) {}
    bool ended = false;

    void pause() override { recorder->log("pause"); }
    void resume() override { recorder->log("resume"); }
    void on(std::string_view event, const IncomingListener& listener) override { listeners[std::string(event)].push_back(std::make_shared<Registration>(Registration{listener, false})); }
    void once(std::string_view event, const IncomingListener& listener) override { listeners[std::string(event)].push_back(std::make_shared<Registration>(Registration{listener, true})); }
    void removeListener(std::string_view event, const IncomingListener& listener) override {
      auto& list = listeners[std::string(event)];
      for (auto position = list.rbegin(); position != list.rend(); ++position) {
        if ((*position)->listener == listener) {
          list.erase(std::next(position).base());
          return;
        }
      }
    }
    bool readableEnded() const override { return ended; }

    void emit(const std::string& event, const IncomingEvent& value) {
      const auto snapshot = listeners[event];
      for (const auto& registration : snapshot) {
        if (registration->once) {
          if (registration->fired) continue;
          registration->fired = true;
          auto& list = listeners[event];
          for (auto position = list.begin(); position != list.end(); ++position) {
            if (*position == registration) {
              list.erase(position);
              break;
            }
          }
        }
        registration->listener(value);
      }
    }
    std::string counts() {
      return "data=" + std::to_string(listeners["data"].size()) + " end=" + std::to_string(listeners["end"].size()) + " error=" + std::to_string(listeners["error"].size());
    }

  private:
    struct Registration {
      IncomingListener listener;
      bool once = false;
      bool fired = false;
    };
    std::shared_ptr<Recorder> recorder;
    std::map<std::string, std::vector<std::shared_ptr<Registration>>> listeners;
  };

  // `new Promise((r) => queueMicrotask(() => r()))`, awaited `count` times.
  rt::Task<void> hops(int count) {
    for (int index = 0; index < count; index += 1) {
      const rt::Promise<void> hop = rt::newPromise<void>([](const rt::Resolve<void>& resolve, const rt::Reject&) { rt::queueMicrotask([resolve] { resolve(); }); });
      co_await hop.wait();
    }
  }

  std::string show(const rt::ByteChunk& bytes) {
    std::string out = "[";
    for (std::size_t index = 0; index < bytes.size(); ++index) out += (index ? "," : "") + std::to_string(bytes[index]);
    return out + "]";
  }

  rt::Task<void> read(Reader reader, std::string label, std::shared_ptr<Recorder> recorder) {
    rt::Reason failure;
    try {
      const rt::ReadableStreamReadResult<rt::ByteChunk> step = co_await reader.read();
      recorder->log(label + " " + (step.done ? std::string("done") : show(*step.value)));
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) recorder->log(label + " rejected " + rt::reasonToString(failure));
  }

  std::string flags(const IncomingBody& body) { return std::string("cancelled=") + (body.cancelled() ? "true" : "false") + " consumed=" + (body.consumed() ? "true" : "false"); }
  std::string consumedFlag(const IncomingBody& body) { return std::string("consumed=") + (body.consumed() ? "true" : "false"); }

  std::shared_ptr<Recorder> startRecorder() {
    auto recorder = std::make_shared<Recorder>();
    rt::queueMicrotask([recorder] { tickOnce(recorder); });
    return recorder;
  }

  rt::Task<std::string> finish(std::shared_ptr<Recorder> recorder) {
    recorder->live = false;
    co_await rt::Promise<void>(hops(3)).wait();
    co_return recorder->events;
  }
}

ASYNC_TEST("an abandoned incoming body releases its unconsumed stream and message") {
  const auto recorder = std::make_shared<Recorder>();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const std::weak_ptr<IncomingMessage> weak = incoming;
  const rt::AbortController controller;
  {
    const IncomingBody body = createIncomingBody(incoming, controller.signal());
    incoming.reset();
  }
  co_await rt::microtask();
  co_await rt::microtask();
  EXPECT(weak.expired());
}

ASYNC_TEST("incoming body flows chunk by chunk and closes on end") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  recorder->log("created " + incoming->counts() + " " + flags(body));
  const Reader reader = body.stream.getReader();
  const rt::Promise<void> first(read(reader, "read1", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  recorder->log("emit data");
  incoming->emit("data", IncomingEvent(rt::ByteChunk{1, 2}));
  co_await first.wait();
  const rt::Promise<void> second(read(reader, "read2", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  recorder->log("emit end");
  incoming->emit("end", IncomingEvent());
  recorder->log("after end " + incoming->counts() + " " + consumedFlag(body));
  co_await second.wait();
  co_await rt::Promise<void>(read(reader, "read3", recorder)).wait();
  body.dispose();
  recorder->log("disposed " + incoming->counts() + " " + flags(body));
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | created data=1 end=1 error=1 cancelled=false consumed=false@0 | resume@1 | emit data@5 | pause@5 | read1 [1,2]@6 | resume@7 | emit end@12 | after end data=0 end=0 error=0 consumed=true@12 | read2 done@13 | read3 done@15 | disposed data=0 end=0 error=0 cancelled=false consumed=true@16"));
}

ASYNC_TEST("incoming body buffers chunks emitted before a read and pauses for each") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  co_await rt::Promise<void>(hops(2)).wait();
  recorder->log("emit two");
  incoming->emit("data", IncomingEvent(rt::ByteChunk{1}));
  incoming->emit("data", IncomingEvent(rt::ByteChunk{2, 3}));
  const Reader reader = body.stream.getReader();
  co_await rt::Promise<void>(read(reader, "read1", recorder)).wait();
  co_await rt::Promise<void>(read(reader, "read2", recorder)).wait();
  const rt::Promise<void> third(read(reader, "read3", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  incoming->emit("end", IncomingEvent());
  co_await third.wait();
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | emit two@5 | pause@5 | pause@5 | read1 [1]@6 | read2 [2,3]@8 | resume@9 | read3 done@15"));
}

ASYNC_TEST("incoming body of an already ended message is consumed at once") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  incoming->ended = true;
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  recorder->log("created " + incoming->counts() + " " + consumedFlag(body));
  co_await rt::Promise<void>(read(body.stream.getReader(), "read1", recorder)).wait();
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | created data=0 end=0 error=0 consumed=true@0 | read1 done@1"));
}

ASYNC_TEST("incoming body of an already aborted request errors with the abort reason") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  incoming->ended = true;
  const rt::AbortController controller;
  controller.abort(rt::thrown(json::value("gone")));
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  recorder->log("created " + incoming->counts() + " " + consumedFlag(body));
  co_await rt::Promise<void>(read(body.stream.getReader(), "read1", recorder)).wait();
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | pause@0 | created data=0 end=0 error=0 consumed=false@0 | read1 rejected gone@1"));
}

ASYNC_TEST("incoming body errors when the request aborts while reading") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  const Reader reader = body.stream.getReader();
  const rt::Promise<void> first(read(reader, "read1", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  recorder->log("abort");
  controller.abort(rt::makeReason(rt::JsError("client left")));
  recorder->log("after abort " + incoming->counts());
  co_await first.wait();
  incoming->emit("data", IncomingEvent(rt::ByteChunk{9}));
  recorder->log("late data ignored " + incoming->counts());
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | resume@1 | abort@5 | pause@5 | after abort data=0 end=0 error=0@5 | read1 rejected Error: client left@6 | late data ignored data=0 end=0 error=0@7"));
}

ASYNC_TEST("incoming body errors with the message's error event") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  const Reader reader = body.stream.getReader();
  const rt::Promise<void> first(read(reader, "read1", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  incoming->emit("error", IncomingEvent(rt::makeReason(rt::JsError("socket hang up"))));
  recorder->log("after error " + incoming->counts());
  co_await first.wait();
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | resume@1 | pause@5 | after error data=0 end=0 error=0@5 | read1 rejected Error: socket hang up@6"));
}

ASYNC_TEST("incoming body refuses a chunk that is not bytes") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  const Reader reader = body.stream.getReader();
  const rt::Promise<void> first(read(reader, "read1", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  incoming->emit("data", IncomingEvent(std::string("text")));
  recorder->log("after string " + incoming->counts());
  co_await first.wait();
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | resume@1 | pause@5 | after string data=0 end=0 error=0@5 | read1 rejected TypeError: HTTP request body did not provide bytes@6"));
}

ASYNC_TEST("incoming body cancellation pauses the message and marks the body cancelled") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  const Reader reader = body.stream.getReader();
  co_await rt::Promise<void>(hops(2)).wait();
  const rt::Promise<void> cancelled = rt::Promise<void>(reader.cancel(rt::thrown(json::value("stop")))).then([recorder] { recorder->log("cancel settled"); });
  co_await cancelled.wait();
  recorder->log("after cancel " + incoming->counts() + " " + flags(body));
  body.dispose();
  recorder->log(std::string("disposed cancelled=") + (body.cancelled() ? "true" : "false"));
  const std::string events = co_await finish(recorder);
  // Bun: "pause@0 | pause@5 | cancel settled@7 | after cancel ...@8 | disposed cancelled=true@8".
  // rt's reader.cancel() is a Task, and the promise made from it settles one turn
  // after the JS cancel promise, so the last three events come one turn later.
  EXPECT_EQ(events, std::string("pause@0 | pause@5 | cancel settled@8 | after cancel data=0 end=0 error=0 cancelled=true consumed=false@9 | disposed cancelled=true@9"));
}

ASYNC_TEST("incoming body disposed before consumption errors its stream") {
  auto recorder = startRecorder();
  auto incoming = std::make_shared<FakeIncoming>(recorder);
  const rt::AbortController controller;
  const IncomingBody body = createIncomingBody(incoming, controller.signal());
  const Reader reader = body.stream.getReader();
  const rt::Promise<void> first(read(reader, "read1", recorder));
  co_await rt::Promise<void>(hops(2)).wait();
  body.dispose();
  recorder->log("disposed " + incoming->counts() + " " + flags(body));
  co_await first.wait();
  incoming->emit("end", IncomingEvent());
  recorder->log("end ignored " + consumedFlag(body));
  const std::string events = co_await finish(recorder);
  EXPECT_EQ(events, std::string("pause@0 | resume@1 | pause@5 | disposed data=0 end=0 error=0 cancelled=false consumed=false@5 | read1 rejected Error: The HTTP response settled before the request body was consumed@6 | end ignored consumed=false@7"));
}
