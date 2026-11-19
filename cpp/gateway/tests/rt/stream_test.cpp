// rt::ReadableStream. Expected logs marked "bun" were recorded by running the
// same scenario under Bun 1.4 (stream1.ts / stream2.ts, string chunks). The readPromise /
// cancelPromise turn counts ("@n": the microtask ticker had run n steps) were recorded with
// bun 1.4 by turns.ts: a ticker queued first, then the scenario, as below.
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include <cmath>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

using namespace uos;

namespace {
  using Strings = std::vector<std::string>;
  using Log = std::shared_ptr<Strings>;
  Log makeLog() { return std::make_shared<Strings>(); }
  using Controller = rt::ReadableStreamDefaultController<std::string>;
  using Stream = rt::ReadableStream<std::string>;
  using Step = rt::ReadableStreamReadResult<std::string>;

  void ticks(const Log& log, int n) {
    rt::spawn(rt::async([log, n]() -> rt::Task<void> {
      for (int i = 1; i <= n; ++i) {
        co_await rt::microtask();
        log->push_back("tick" + std::to_string(i));
      }
    }));
  }

  std::string desired(const Controller& controller) {
    auto size = controller.desiredSize();
    return size ? std::to_string(static_cast<long long>(*size)) : "null";
  }

  // JSON.stringify(reason) for values, "Name: message" for errors.
  std::string describe(const rt::Reason& reason) {
    if (!reason) return "undefined";
    if (!rt::isError(reason)) {
      auto value = rt::thrownValue(reason);
      return value ? json::serialize(*value) : "undefined";
    }
    return rt::errorName(reason).value_or("Error") + ": " + rt::errorMessage(reason).value_or("");
  }

  std::string stepText(const Step& step) { return step.done ? "{\"done\":true}" : "{\"value\":" + json::serialize(json::value(*step.value)) + ",\"done\":false}"; }

  // The probes' amsg(): "resolved <value>" or "rejected <Name: message | JSON>".
  template <class T> rt::Task<std::string> outcome(rt::Task<T> task) {
    std::string text;
    try {
      if constexpr (std::is_void_v<T>) {
        co_await std::move(task);
        text = "resolved undefined";
      } else {
        auto value = co_await std::move(task);
        text = "resolved " + stepText(value);
      }
    } catch (...) {
      text = "rejected " + describe(std::current_exception());
    }
    co_return text;
  }

  // The probes' msg(): a synchronous call's outcome.
  template <class F> std::string attempt(F call) {
    try {
      call();
      return "no-throw";
    } catch (...) {
      return describe(std::current_exception());
    }
  }
}

ASYNC_TEST("bun stream1: start runs in the constructor; pull runs a turn after start, or inside read() for highWaterMark 0") {
  auto log = makeLog();
  rt::UnderlyingSource<std::string> sourceA;
  sourceA.start = [log](Controller c) { log->push_back("A:start desired=" + desired(c)); };
  sourceA.pull = [log](Controller c) {
    log->push_back("A:pull desired=" + desired(c));
    c.enqueue("x");
    log->push_back("A:after-enqueue desired=" + desired(c));
  };
  Stream s(sourceA);
  rt::UnderlyingSource<std::string> sourceB;
  sourceB.start = [log](Controller c) { log->push_back("B:start desired=" + desired(c)); };
  sourceB.pull = [log](Controller c) {
    log->push_back("B:pull desired=" + desired(c));
    c.enqueue("y");
  };
  Stream s0(sourceB, {.highWaterMark = 0});
  ticks(log, 4);
  co_await rt::sleep(5);
  log->push_back("--- read B");
  auto rb = s0.getReader();
  auto p = rb.read();
  log->push_back("B:read-called");
  ticks(log, 3);
  auto v = co_await std::move(p);
  log->push_back("B:read=" + stepText(v));
  co_await rt::sleep(5);
  log->push_back("--- read A");
  auto ra = s.getReader();
  auto pa = ra.read();
  log->push_back("A:read-called");
  ticks(log, 3);
  auto va = co_await std::move(pa);
  log->push_back("A:read=" + stepText(va));
  co_await rt::sleep(5);
  EXPECT_EQ(*log, (Strings{"A:start desired=1", "B:start desired=0", "A:pull desired=1", "A:after-enqueue desired=0", "tick1", "tick2", "tick3", "tick4", "--- read B", "B:pull desired=0",
                           "B:read-called", "tick1", "B:read={\"value\":\"y\",\"done\":false}", "tick2", "tick3", "--- read A", "A:pull desired=1", "A:after-enqueue desired=0", "A:read-called",
                           "tick1", "A:read={\"value\":\"x\",\"done\":false}", "tick2", "tick3"}));
}

ASYNC_TEST("bun stream1: misuse errors carry Bun's messages") {
  Strings log;
  {
    std::optional<Controller> ctl;
    rt::UnderlyingSource<std::string> source;
    source.start = [&ctl](Controller c) { ctl = c; };
    Stream s(source);
    auto r = s.getReader();
    log.push_back("getReader twice: " + attempt([&] { (void)s.getReader(); }));
    log.push_back("cancel locked: " + co_await outcome(s.cancel(rt::thrown(json::value("x")))));
    log.push_back(std::string("locked=") + (s.locked() ? "true" : "false"));
    ctl->close();
    log.push_back("enqueue after close: " + attempt([&] { ctl->enqueue("z"); }));
    log.push_back("close after close: " + attempt([&] { ctl->close(); }));
    log.push_back("desired after close: " + desired(*ctl));
    log.push_back("read after close: " + co_await outcome(r.read()));
    log.push_back("closed: " + co_await outcome(r.closed().wait()));
    r.releaseLock();
    log.push_back("read after release: " + co_await outcome(r.read()));
    log.push_back("cancel after release: " + co_await outcome(r.cancel()));
    log.push_back("closed after release: " + co_await outcome(r.closed().wait()));
    log.push_back("releaseLock twice: " + attempt([&] { r.releaseLock(); }));
    log.push_back(std::string("locked=") + (s.locked() ? "true" : "false"));
  }
  {
    std::optional<Controller> ctl;
    rt::UnderlyingSource<std::string> source;
    source.start = [&ctl](Controller c) { ctl = c; };
    Stream s(source);
    auto r = s.getReader();
    auto pending = r.read();
    auto closed = r.closed();
    r.releaseLock();
    log.push_back("pending read on release: " + co_await outcome(std::move(pending)));
    log.push_back("closed on release: " + co_await outcome(closed.wait()));
    ctl->error(rt::makeReason(rt::JsError("boom")));
    log.push_back("desired after error: " + desired(*ctl));
    log.push_back("enqueue after error: " + attempt([&] { ctl->enqueue("q"); }));
    log.push_back("close after error: " + attempt([&] { ctl->close(); }));
    auto r2 = s.getReader();
    log.push_back("read errored: " + co_await outcome(r2.read()));
    log.push_back("closed errored: " + co_await outcome(r2.closed().wait()));
    log.push_back("cancel errored: " + co_await outcome(r2.cancel(rt::thrown(json::value("c")))));
    r2.releaseLock();
    log.push_back("stream.cancel errored(after release): " + co_await outcome(s.cancel(rt::thrown(json::value("c")))));
  }
  log.push_back("hwm -1: " + attempt([] { Stream({}, {.highWaterMark = -1}); }));
  log.push_back("hwm NaN: " + attempt([] { Stream({}, {.highWaterMark = std::nan("")}); }));
  log.push_back("start throws: " + attempt([] {
    rt::UnderlyingSource<std::string> source;
    source.start = [](Controller) { throw rt::JsError("st"); };
    Stream stream(source);
  }));
  EXPECT_EQ(log, (Strings{"getReader twice: TypeError: Invalid state: ReadableStream is locked",
                          "cancel locked: rejected TypeError: Cannot cancel a locked ReadableStream",
                          "locked=true",
                          "enqueue after close: TypeError: Invalid state: Controller is already closed",
                          "close after close: TypeError: Invalid state: Controller is already closed",
                          "desired after close: 0",
                          "read after close: resolved {\"done\":true}",
                          "closed: resolved undefined",
                          "read after release: rejected TypeError: Invalid state: The reader is not attached to a stream",
                          "cancel after release: rejected TypeError: Invalid state: The reader is not attached to a stream",
                          "closed after release: rejected TypeError: Invalid state: Reader released",
                          "releaseLock twice: no-throw",
                          "locked=false",
                          "pending read on release: rejected TypeError: Invalid state: Releasing reader",
                          "closed on release: rejected TypeError: Invalid state: Reader released",
                          "desired after error: null",
                          "enqueue after error: TypeError: Invalid state: Controller is already closed",
                          "close after error: TypeError: Invalid state: Controller is already closed",
                          "read errored: rejected Error: boom",
                          "closed errored: rejected Error: boom",
                          "cancel errored: rejected Error: boom",
                          "stream.cancel errored(after release): rejected Error: boom",
                          "hwm -1: RangeError: The queuing strategy's highWaterMark must be a non-negative, non-NaN number",
                          "hwm NaN: RangeError: The queuing strategy's highWaterMark must be a non-negative, non-NaN number",
                          "start throws: Error: st"}));
}

ASYNC_TEST("bun stream2: cancel, failing pull/start, close with queued chunks, highWaterMark 2") {
  auto log = makeLog();
  {
    rt::UnderlyingSource<std::string> source;
    source.cancel = [log](rt::Reason reason) -> rt::Task<void> {
      log->push_back("cancel cb reason=" + describe(reason));
      co_await rt::sleep(2);
      log->push_back("cancel cb settles");
    };
    Stream s(source, {.highWaterMark = 0});
    auto r = s.getReader();
    auto pending = r.read();
    auto closed = r.closed();
    auto c = r.cancel(rt::thrown(json::value("why")));
    log->push_back("after cancel call");
    log->push_back("pending read: " + co_await outcome(std::move(pending)));
    log->push_back("closed: " + co_await outcome(closed.wait()));
    log->push_back("cancel: " + co_await outcome(std::move(c)));
    log->push_back("read after cancel: " + co_await outcome(r.read()));
  }
  {
    rt::UnderlyingSource<std::string> source;
    source.start = [](Controller c) {
      c.enqueue("a");
      c.enqueue("b");
    };
    source.cancel = [log](rt::Reason reason) { log->push_back("cancel2 cb reason=" + rt::reasonToString(reason)); };
    Stream s(source);
    log->push_back("stream.cancel(): " + co_await outcome(s.cancel()));
    auto r = s.getReader();
    log->push_back("read after stream.cancel: " + co_await outcome(r.read()));
  }
  {
    rt::UnderlyingSource<std::string> throwing;
    throwing.pull = [](Controller) { throw rt::TypeError("pull-threw"); };
    Stream s(throwing);
    log->push_back("pull throws read: " + co_await outcome(s.getReader().read()));
    rt::UnderlyingSource<std::string> rejecting;
    rejecting.pull = [](Controller) -> rt::Task<void> {
      co_await rt::microtask();
      throw rt::JsError("pull-rejected");
    };
    Stream s2(rejecting);
    log->push_back("pull rejects read: " + co_await outcome(s2.getReader().read()));
    rt::UnderlyingSource<std::string> startRejects;
    startRejects.start = [](Controller) -> rt::Task<void> {
      throw rt::JsError("start-rejected");
      co_return;
    };
    Stream s3(startRejects);
    log->push_back("start rejects read: " + co_await outcome(s3.getReader().read()));
  }
  {
    rt::UnderlyingSource<std::string> source;
    source.start = [log](Controller c) {
      c.enqueue("1");
      c.close();
      log->push_back("desired after close with queue=" + desired(c));
    };
    Stream s(source);
    auto r = s.getReader();
    log->push_back("q1: " + co_await outcome(r.read()));
    log->push_back("q2: " + co_await outcome(r.read()));
  }
  {
    auto pulls = std::make_shared<int>(0);
    rt::UnderlyingSource<std::string> source;
    source.pull = [pulls](Controller c) -> rt::Task<void> {
      ++*pulls;
      co_await rt::microtask();
      c.enqueue(std::to_string(*pulls));
      if (*pulls == 4) c.close();
    };
    Stream s(source, {.highWaterMark = 2});
    co_await rt::sleep(5);
    log->push_back("hwm2 pulls before read=" + std::to_string(*pulls));
    auto r = s.getReader();
    std::string got;
    for (;;) {
      auto step = co_await r.read();
      if (step.done) break;
      got += (got.empty() ? "" : ",") + *step.value;
    }
    log->push_back("hwm2 got=[" + got + "] pulls=" + std::to_string(*pulls));
  }
  EXPECT_EQ(*log, (Strings{"cancel cb reason=\"why\"", "after cancel call", "pending read: resolved {\"done\":true}", "closed: resolved undefined", "cancel cb settles", "cancel: resolved undefined",
                           "read after cancel: resolved {\"done\":true}", "cancel2 cb reason=undefined", "stream.cancel(): resolved undefined", "read after stream.cancel: resolved {\"done\":true}",
                           "pull throws read: rejected TypeError: pull-threw", "pull rejects read: rejected Error: pull-rejected", "start rejects read: rejected Error: start-rejected",
                           "desired after close with queue=0", "q1: resolved {\"value\":\"1\",\"done\":false}", "q2: resolved {\"done\":true}", "hwm2 pulls before read=2", "hwm2 got=[1,2,3,4] pulls=4"}));
}

ASYNC_TEST("a source Task can borrow its controller across suspension") {
  rt::UnderlyingSource<std::string> source;
  source.pull = [](const Controller& controller) -> rt::Task<void> {
    co_await rt::microtask();
    controller.enqueue("chunk");
    controller.close();
  };
  const Stream stream(std::move(source));
  const auto reader = stream.getReader();
  const auto chunk = co_await reader.read();
  ASSERT(chunk.value.has_value());
  EXPECT_EQ(*chunk.value, "chunk");
  const auto closed = co_await reader.read();
  EXPECT(closed.done);
}

ASYNC_TEST("reader cancellation retains the stream when the source releases the reader") {
  using Reader = rt::ReadableStreamDefaultReader<std::string>;
  std::optional<Reader> reader;
  rt::UnderlyingSource<std::string> source;
  source.cancel = [&reader](rt::Reason) { reader->releaseLock(); };
  Stream stream(std::move(source));
  reader.emplace(stream.getReader());
  auto pending = reader->read();

  auto cancelled = reader->cancelPromise();
  EXPECT(!stream.locked());
  co_await cancelled.wait();
  const auto step = co_await std::move(pending);
  EXPECT(step.done);
}

ASYNC_TEST("an abandoned tee releases its source while a read is parked") {
  std::weak_ptr<rt::detail::StreamState<std::string>> weak;
  int pulls = 0;
  {
    rt::UnderlyingSource<std::string> source;
    source.pull = [&pulls](Controller) { ++pulls; };
    const Stream stream(std::move(source), rt::QueuingStrategy{0});
    weak = stream.state();
    const auto branches = stream.tee();
    co_await rt::sleep(1);
    EXPECT_EQ(pulls, 1);
  }
  EXPECT(weak.expired());
}

ASYNC_TEST("tee cancellation still settles from the source after the branches are released") {
  for (const bool fail : {false, true}) {
    std::optional<Controller> controller;
    rt::UnderlyingSource<std::string> source;
    source.start = [&controller](Controller active) { controller.emplace(std::move(active)); };
    const Stream stream(std::move(source), rt::QueuingStrategy{0});
    rt::Promise<void> cancelled;
    {
      const auto branches = stream.tee();
      cancelled = branches.first.cancelPromise();
      co_await rt::sleep(1);
      EXPECT(cancelled.pending());
    }
    if (fail) controller->error(rt::makeReason(rt::JsError("source failed")));
    else controller->close();
    co_await rt::sleep(1);
    EXPECT(cancelled.fulfilled());
  }
}

ASYNC_TEST("bun stream2: tee() gives both branches every chunk; both cancels cancel the source with [r1, r2]; a source error errors both") {
  auto log = makeLog();
  {
    auto pulls = std::make_shared<int>(0);
    rt::UnderlyingSource<std::string> source;
    source.pull = [pulls](Controller c) {
      ++*pulls;
      if (*pulls <= 2) c.enqueue("c" + std::to_string(*pulls));
      else c.close();
    };
    source.cancel = [log](rt::Reason reason) { log->push_back("tee source cancel reason=" + describe(reason)); };
    Stream s(source, {.highWaterMark = 0});
    auto branches = s.tee();
    auto& a = branches.first;
    auto& b = branches.second;
    log->push_back(std::string("tee locked source=") + (s.locked() ? "true" : "false"));
    auto ra = a.getReader();
    log->push_back("a1: " + co_await outcome(ra.read()));
    log->push_back("pulls after a1=" + std::to_string(*pulls));
    auto rb = b.getReader();
    log->push_back("b1: " + co_await outcome(rb.read()));
    log->push_back("a2: " + co_await outcome(ra.read()));
    log->push_back("a3: " + co_await outcome(ra.read()));
    log->push_back("b2: " + co_await outcome(rb.read()));
    log->push_back("b3: " + co_await outcome(rb.read()));
  }
  {
    rt::UnderlyingSource<std::string> source;
    source.cancel = [log](rt::Reason reason) { log->push_back("tee2 source cancel reason=" + describe(reason)); };
    Stream s(source);
    auto branches = s.tee();
    auto& a = branches.first;
    auto& b = branches.second;
    auto ca = a.cancel(rt::thrown(json::value("ra")));
    log->push_back("after cancel a (source not cancelled yet)");
    auto cb = b.cancel(rt::thrown(json::value("rb")));
    auto first = co_await outcome(std::move(ca));
    auto second = co_await outcome(std::move(cb));
    log->push_back("cancel a: " + first + " cancel b: " + second);
  }
  {
    std::optional<Controller> ctl;
    rt::UnderlyingSource<std::string> source;
    source.start = [&ctl](Controller c) { ctl = c; };
    Stream s(source);
    auto branches = s.tee();
    auto& a = branches.first;
    auto& b = branches.second;
    auto pa = a.getReader().read();
    ctl->error(rt::makeReason(rt::JsError("src-err")));
    auto first = co_await outcome(std::move(pa));
    auto second = co_await outcome(b.getReader().read());
    log->push_back("tee error a: " + first + " b: " + second);
  }
  EXPECT_EQ(*log, (Strings{"tee locked source=true", "a1: resolved {\"value\":\"c1\",\"done\":false}", "pulls after a1=2", "b1: resolved {\"value\":\"c1\",\"done\":false}",
                           "a2: resolved {\"value\":\"c2\",\"done\":false}", "a3: resolved {\"done\":true}", "b2: resolved {\"value\":\"c2\",\"done\":false}", "b3: resolved {\"done\":true}",
                           "after cancel a (source not cancelled yet)", "tee2 source cancel reason=[\"ra\",\"rb\"]", "cancel a: resolved undefined cancel b: resolved undefined",
                           "tee error a: rejected Error: src-err b: rejected Error: src-err"}));
}

ASYNC_TEST("bun stream2: async iteration reads every chunk; breaking out cancels the stream and releases the lock") {
  auto log = makeLog();
  rt::UnderlyingSource<std::string> source;
  source.start = [](Controller c) {
    c.enqueue("x");
    c.enqueue("y");
  };
  source.cancel = [log](rt::Reason reason) { log->push_back("iter cancel reason=" + describe(reason)); };
  Stream s(source);
  auto body = [log](std::string value) -> rt::Task<bool> {
    log->push_back("iter " + value);
    co_return false;
  };
  co_await rt::forAwait(s.values(), body);
  log->push_back(std::string("locked after break=") + (s.locked() ? "true" : "false"));
  EXPECT_EQ(*log, (Strings{"iter x", "iter cancel reason=undefined", "locked after break=false"}));

  log->clear();
  rt::UnderlyingSource<std::string> finite;
  finite.start = [](Controller c) {
    c.enqueue("1");
    c.enqueue("2");
    c.close();
  };
  Stream all(finite);
  auto collect = [log](std::string value) -> rt::Task<void> {
    log->push_back("chunk " + value);
    co_return;
  };
  co_await rt::forAwait(all.values(), collect);
  EXPECT_EQ(*log, (Strings{"chunk 1", "chunk 2"}));
  EXPECT(!all.locked());
}

ASYNC_TEST("byte streams: an SSE-style pull source relays Uint8Array chunks and closes; desiredSize follows the queue") {
  using Bytes = rt::ByteChunk;
  auto frames = std::make_shared<std::vector<std::string>>(std::vector<std::string>{"data: 1\n\n", "data: 2\n\n"});
  auto index = std::make_shared<std::size_t>(0);
  rt::UnderlyingSource<Bytes> source;
  source.pull = [frames, index](rt::ReadableStreamDefaultController<Bytes> controller) -> rt::Task<void> {
    co_await rt::sleep(1);
    if (*index == frames->size()) {
      controller.close();
      co_return;
    }
    const auto& frame = (*frames)[(*index)++];
    controller.enqueue(Bytes(frame.begin(), frame.end()));
  };
  rt::ReadableStream<Bytes> body(source, {.highWaterMark = 0});
  auto reader = body.getReader();
  std::string received;
  for (;;) {
    auto step = co_await reader.read();
    if (step.done) break;
    received.append(step.value->begin(), step.value->end());
  }
  EXPECT_EQ(received, "data: 1\n\ndata: 2\n\n");
  auto closed = reader.closed();
  EXPECT(closed.fulfilled());

  std::optional<rt::ReadableStreamDefaultController<Bytes>> ctl;
  rt::UnderlyingSource<Bytes> queued;
  queued.start = [&ctl](rt::ReadableStreamDefaultController<Bytes> c) { ctl = c; };
  rt::ReadableStream<Bytes> buffered(queued, {.highWaterMark = 2});
  EXPECT_EQ(ctl->desiredSize(), std::optional<double>(2));
  ctl->enqueue(Bytes{1});
  ctl->enqueue(Bytes{2});
  ctl->enqueue(Bytes{3});
  EXPECT_EQ(ctl->desiredSize(), std::optional<double>(-1));
  auto bufferedReader = buffered.getReader();
  auto first = co_await bufferedReader.read();
  EXPECT_EQ(*first.value, Bytes{1});
  EXPECT_EQ(ctl->desiredSize(), std::optional<double>(0));
}

ASYNC_TEST("disturbed() is the [[disturbed]] slot: set by a read or a cancel, not by getReader") {
  // No JS property shows it (Bun's node:stream isDisturbed answers false for web streams);
  // Body.bodyUsed reads it.
  auto make = [] {
    rt::UnderlyingSource<std::string> source;
    source.start = [](Controller c) {
      c.enqueue("a");
      c.close();
    };
    return Stream(source);
  };
  const Stream fresh = make();
  EXPECT(!fresh.disturbed());
  const Stream locked = make();
  auto reader = locked.getReader();
  EXPECT(locked.locked());
  EXPECT(!locked.disturbed());
  const Step first = co_await reader.read();
  EXPECT(!first.done);
  EXPECT(locked.disturbed());
  const Stream cancelled = make();
  co_await cancelled.cancel();
  EXPECT(cancelled.disturbed());
  const Stream copy = cancelled;
  EXPECT(copy.disturbed());
}

namespace {
  // The probes' ticker: one step per microtask turn, queued before the scenario runs.
  std::shared_ptr<int> startTicker() {
    auto tick = std::make_shared<int>(0);
    rt::spawn(rt::async([tick]() -> rt::Task<void> {
      for (int i = 0; i < 40; ++i) {
        co_await rt::microtask();
        ++*tick;
      }
    }));
    return tick;
  }

  // A stream whose controller the test keeps.
  struct Held {
    std::shared_ptr<std::optional<Controller>> controller;
    Stream stream;
  };
  Held heldStream() {
    auto slot = std::make_shared<std::optional<Controller>>();
    rt::UnderlyingSource<std::string> source;
    source.start = [slot](Controller controller) { *slot = controller; };
    return Held{.controller = slot, .stream = Stream(source)};
  }
}

ASYNC_TEST("bun turns: readPromise() settles when JS's read promise does, so a ready read wins a race against a rejected abort") {
  // A: new ReadableStream({ start(c) { c.enqueue("x") } }); race([reader.read(), Promise.reject(abort)])
  //    bun: read@1  race-fulfilled:{"value":"x","done":false}@2  await-race:{"value":"x","done":false}@2
  auto log = makeLog();
  Held held = heldStream();
  (*held.controller)->enqueue("x");
  co_await rt::sleep(1);
  auto reader = held.stream.getReader();
  auto abort = rt::Promise<Step>::reject(rt::makeReason(rt::JsError("abort")));
  abort.catch_([](const rt::Reason&) {});
  auto tick = startTicker();
  auto read = reader.readPromise();
  read.then([log, tick](const Step&) { log->push_back("read@" + std::to_string(*tick)); });
  std::vector<rt::Promise<Step>> racers{read, abort};
  auto race = rt::race(std::move(racers));
  race.then([log, tick](const Step& step) { log->push_back("race-fulfilled:" + stepText(step) + "@" + std::to_string(*tick)); },
            [log, tick](const rt::Reason&) { log->push_back("race-rejected@" + std::to_string(*tick)); });
  try {
    const Step step = co_await race.wait();
    log->push_back("await-race:" + stepText(step) + "@" + std::to_string(*tick));
  } catch (...) {
    log->push_back("await-race-rejected@" + std::to_string(*tick));
  }
  EXPECT_EQ(*log, (Strings{"read@1", "race-fulfilled:{\"value\":\"x\",\"done\":false}@2", "await-race:{\"value\":\"x\",\"done\":false}@2"}));
  // F: a closed stream's read wins too. bun: race-fulfilled:{"done":true}@2  await-race@2
  auto closedLog = makeLog();
  Held closed = heldStream();
  (*closed.controller)->close();
  co_await rt::sleep(1);
  auto closedReader = closed.stream.getReader();
  auto closedTick = startTicker();
  std::vector<rt::Promise<Step>> closedRacers{closedReader.readPromise(), abort};
  auto closedRace = rt::race(std::move(closedRacers));
  closedRace.then([closedLog, closedTick](const Step& step) { closedLog->push_back("race-fulfilled:" + stepText(step) + "@" + std::to_string(*closedTick)); },
                  [closedLog, closedTick](const rt::Reason&) { closedLog->push_back("race-rejected@" + std::to_string(*closedTick)); });
  co_await closedRace.wait();
  closedLog->push_back("await-race@" + std::to_string(*closedTick));
  EXPECT_EQ(*closedLog, (Strings{"race-fulfilled:{\"done\":true}@2", "await-race@2"}));
  // The read Task raced as rt::Promise(reader.read()) settles a turn later, so the abort wins:
  // the turn the modules compensated for before readPromise() existed.
  auto taskLog = makeLog();
  Held again = heldStream();
  (*again.controller)->enqueue("y");
  co_await rt::sleep(1);
  auto againReader = again.stream.getReader();
  std::vector<rt::Promise<Step>> taskRacers{rt::Promise<Step>(againReader.read()), abort};
  auto taskRace = rt::race(std::move(taskRacers));
  try {
    co_await taskRace.wait();
    taskLog->push_back("fulfilled");
  } catch (...) {
    taskLog->push_back("rejected");
  }
  EXPECT_EQ(*taskLog, (Strings{"rejected"}));
}

ASYNC_TEST("bun turns: a pending readPromise() settles with the enqueue, close or error, as JS's read promise does") {
  // B/C/D: reader.read() pending; at tick 3 enqueue("y") / close() / error(e).
  //   enqueue: enqueue@3  read:{"value":"y","done":false}@4  await-read@4  race@5
  //   close:   close@3  read:{"done":true}@4  await-read@4  race@5
  //   error:   error@3  read-rejected@4  await-read-rejected@4  race-rejected@5
  const std::vector<std::string> actions{"enqueue", "close", "error"};
  for (std::size_t index = 0; index < actions.size(); ++index) {
    const std::string action = actions[index];
    auto log = makeLog();
    Held held = heldStream();
    co_await rt::sleep(1);
    auto reader = held.stream.getReader();
    auto tick = startTicker();
    auto read = reader.readPromise();
    read.then([log, tick](const Step& step) { log->push_back("read:" + stepText(step) + "@" + std::to_string(*tick)); },
              [log, tick](const rt::Reason&) { log->push_back("read-rejected@" + std::to_string(*tick)); });
    rt::Promise<Step> never;
    std::vector<rt::Promise<Step>> racers{read, never};
    auto race = rt::race(std::move(racers));
    race.then([log, tick](const Step&) { log->push_back("race@" + std::to_string(*tick)); },
              [log, tick](const rt::Reason&) { log->push_back("race-rejected@" + std::to_string(*tick)); });
    auto controller = held.controller;
    rt::spawn(rt::async([log, tick, controller, action]() -> rt::Task<void> {
      co_await rt::microtask();
      co_await rt::microtask();
      co_await rt::microtask();
      log->push_back(action + "@" + std::to_string(*tick));
      if (action == "enqueue") (*controller)->enqueue("y");
      else if (action == "close") (*controller)->close();
      else (*controller)->error(rt::makeReason(rt::JsError("e")));
    }));
    try {
      co_await read.wait();
      log->push_back("await-read@" + std::to_string(*tick));
    } catch (...) {
      log->push_back("await-read-rejected@" + std::to_string(*tick));
    }
    co_await rt::sleep(5);
    if (action == "enqueue") EXPECT_EQ(*log, (Strings{"enqueue@3", "read:{\"value\":\"y\",\"done\":false}@4", "await-read@4", "race@5"}));
    else if (action == "close") EXPECT_EQ(*log, (Strings{"close@3", "read:{\"done\":true}@4", "await-read@4", "race@5"}));
    else EXPECT_EQ(*log, (Strings{"error@3", "read-rejected@4", "await-read-rejected@4", "race-rejected@5"}));
  }
  // E: a pending read against an abort rejected at tick 3 (then a late enqueue):
  //    bun: abort@3  race-rejected@5  await-race-rejected@5
  auto log = makeLog();
  Held held = heldStream();
  co_await rt::sleep(1);
  auto reader = held.stream.getReader();
  auto [abort, resolveAbort, rejectAbort] = rt::withResolvers<Step>();
  (void)resolveAbort;
  auto tick = startTicker();
  auto read = reader.readPromise();
  std::vector<rt::Promise<Step>> racers{read, abort};
  auto race = rt::race(std::move(racers));
  race.then([log, tick](const Step&) { log->push_back("race-fulfilled@" + std::to_string(*tick)); },
            [log, tick](const rt::Reason&) { log->push_back("race-rejected@" + std::to_string(*tick)); });
  auto controller = held.controller;
  auto reject = rejectAbort;
  rt::spawn(rt::async([log, tick, controller, reject]() -> rt::Task<void> {
    co_await rt::microtask();
    co_await rt::microtask();
    co_await rt::microtask();
    log->push_back("abort@" + std::to_string(*tick));
    reject(rt::makeReason(rt::JsError("a")));
    (*controller)->enqueue("late");
  }));
  try {
    co_await race.wait();
    log->push_back("await-race@" + std::to_string(*tick));
  } catch (...) {
    log->push_back("await-race-rejected@" + std::to_string(*tick));
  }
  EXPECT_EQ(*log, (Strings{"abort@3", "race-rejected@5", "await-race-rejected@5"}));
}

ASYNC_TEST("bun turns: cancelPromise() settles when JS's cancel promise does (synchronous cancel algorithm)") {
  // G: new ReadableStream({ cancel(reason) {...} }); reader.cancel("why")
  //    bun: cancel-algorithm@0  cancel-promise@2  await-cancel@2  race@3
  auto log = makeLog();
  auto tick = std::make_shared<int>(0);
  rt::UnderlyingSource<std::string> source;
  source.cancel = [log, tick](rt::Reason reason) { log->push_back("cancel-algorithm:" + describe(reason) + "@" + std::to_string(*tick)); };
  Stream stream(source);
  co_await rt::sleep(1);
  auto reader = stream.getReader();
  // The algorithm keeps the first counter (0): it runs synchronously inside cancel, before any turn.
  tick = startTicker();
  const rt::Promise<void> cancelled = reader.cancelPromise(rt::thrown(json::value("why")));
  cancelled.then([log, tick] { log->push_back("cancel-promise@" + std::to_string(*tick)); });
  rt::Promise<void> never;
  std::vector<rt::Promise<void>> racers{cancelled, never};
  auto race = rt::race(std::move(racers));
  race.then([log, tick] { log->push_back("race@" + std::to_string(*tick)); });
  co_await cancelled.wait();
  log->push_back("await-cancel@" + std::to_string(*tick));
  co_await rt::sleep(5);
  EXPECT_EQ(*log, (Strings{"cancel-algorithm:\"why\"@0", "cancel-promise@2", "await-cancel@2", "race@3"}));
  // After releaseLock() it is a rejected promise; stream.cancelPromise() on a locked stream too.
  reader.releaseLock();
  auto released = reader.cancelPromise();
  std::string releasedText;
  try {
    co_await released.wait();
  } catch (...) {
    releasedText = describe(std::current_exception());
  }
  EXPECT_EQ(releasedText, attempt([&] { rt::rethrow(rt::detail::streamError(rt::detail::StreamError::releasedCancel)); }));
  Stream locked;
  auto lockingReader = locked.getReader();
  auto lockedCancel = locked.cancelPromise();
  std::string lockedText;
  try {
    co_await lockedCancel.wait();
  } catch (...) {
    lockedText = describe(std::current_exception());
  }
  EXPECT_EQ(lockedText, attempt([&] { rt::rethrow(rt::detail::streamError(rt::detail::StreamError::cancelLocked)); }));
  // stream.cancelPromise() on an unlocked stream cancels it.
  Stream open;
  auto openCancel = open.cancelPromise();
  co_await openCancel.wait();
  EXPECT(open.disturbed());
}
