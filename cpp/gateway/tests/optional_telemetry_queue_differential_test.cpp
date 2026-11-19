// Differential cases for src/optional-telemetry-queue.ts recorded from Bun 1.4.0:
// measureJsonPayloadBytes on JSON shapes the TS tests do not cover, and microtask- and
// timer-level traces of the queue (snapshot after every `await null` / timer tick),
// which pin the turn on which each write, release, drain retirement and close lands.
#include "uos/optional_telemetry_queue.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "helpers/live_allocations.hpp"

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
using optional_telemetry_queue::createOptionalTelemetryQueue;
using optional_telemetry_queue::measureJsonPayloadBytes;
using optional_telemetry_queue::OptionalTelemetryQueue;
using optional_telemetry_queue::OptionalTelemetryQueueOptions;
using optional_telemetry_queue::OptionalTelemetryQueueSnapshot;

namespace {
  const char* const TRACES = R"TRACES({"immediate":[["write",0],["enqueue",0,"enqueued",[1,1,0,0,1,true,0,false,false,0,false,1,0,0,0,0,0,0,null]],["enqueue",1,"enqueued",[2,2,1,0,1,true,0,false,false,1,false,2,0,0,0,0,0,0,null]],["enqueue",2,"enqueued",[3,3,2,0,1,true,0,false,false,2,false,3,0,0,0,0,0,0,null]],["enqueue",3,"enqueued",[4,4,3,0,1,true,0,false,false,3,false,4,0,0,0,0,0,0,null]],["enqueue",4,"enqueued",[5,5,4,0,1,true,0,false,false,4,false,5,0,0,0,0,0,0,null]],["same",true],["step",0,false,[5,5,4,0,1,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["step",1,false,[4,4,4,0,0,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["step",2,false,[4,4,4,0,0,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["step",3,false,[4,4,4,0,0,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["step",4,false,[4,4,4,0,0,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["write",1],["write",2],["step",5,false,[4,4,2,0,2,true,0,false,false,6,false,5,0,1,0,0,0,0,null]],["step",6,false,[4,4,2,0,2,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["step",7,false,[2,2,2,0,0,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["step",8,false,[2,2,2,0,0,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["step",9,false,[2,2,2,0,0,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["step",10,false,[2,2,2,0,0,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["write",3],["write",4],["step",11,false,[2,2,0,0,2,true,0,false,false,6,false,5,2,1,0,0,0,0,null]],["step",12,false,[2,2,0,0,2,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",13,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",14,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",15,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",16,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",17,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",18,false,[0,0,0,null,0,true,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",19,false,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["first settled",[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",20,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",21,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",22,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",23,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",24,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",25,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",26,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",27,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",28,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["step",29,true,[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]],["done",[0,0,0,null,0,false,0,false,false,6,false,5,3,2,0,0,0,0,null]]],"rejecting":[["write",0],["enqueue",0,"enqueued"],["enqueue",1,"enqueued"],["enqueue",2,"enqueued"],["enqueue",3,"enqueued"],["enqueue",4,"enqueued"],["enqueue",5,"enqueued"],["enqueue",6,"enqueued"],["step",0,[7,7,6,0,1,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["step",1,[6,6,6,0,0,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["step",2,[6,6,6,0,0,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["step",3,[6,6,6,0,0,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["step",4,[6,6,6,0,0,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["write",1],["step",5,[6,6,5,0,1,true,0,false,false,6,false,7,0,1,0,0,0,0,"Error"]],["step",6,[6,6,5,0,1,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["step",7,[5,5,5,0,0,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["step",8,[5,5,5,0,0,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["step",9,[5,5,5,0,0,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["step",10,[5,5,5,0,0,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["write",2],["step",11,[5,5,4,0,1,true,0,false,false,6,false,7,0,2,0,0,0,0,"TypeError"]],["step",12,[5,5,4,0,1,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["step",13,[4,4,4,0,0,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["step",14,[4,4,4,0,0,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["step",15,[4,4,4,0,0,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["step",16,[4,4,4,0,0,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["write",3],["step",17,[4,4,3,0,1,true,0,false,false,6,false,7,0,3,0,0,0,0,"string"]],["step",18,[4,4,3,0,1,true,0,false,false,6,false,7,0,4,0,0,0,0,"undefined"]],["step",19,[3,3,3,0,0,true,0,false,false,6,false,7,0,4,0,0,0,0,"undefined"]],["step",20,[3,3,3,0,0,true,0,false,false,6,false,7,0,4,0,0,0,0,"undefined"]],["step",21,[3,3,3,0,0,true,0,false,false,6,false,7,0,4,0,0,0,0,"undefined"]],["step",22,[3,3,3,0,0,true,0,false,false,6,false,7,0,4,0,0,0,0,"undefined"]],["write",4],["step",23,[3,3,2,0,1,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["step",24,[2,2,2,0,0,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["step",25,[2,2,2,0,0,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["step",26,[2,2,2,0,0,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["step",27,[2,2,2,0,0,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["write",5],["step",28,[2,2,1,0,1,true,0,false,false,6,false,7,0,5,0,0,0,0,"SyntaxError"]],["step",29,[2,2,1,0,1,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["step",30,[1,1,1,0,0,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["step",31,[1,1,1,0,0,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["step",32,[1,1,1,0,0,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["step",33,[1,1,1,0,0,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["write",6],["step",34,[1,1,0,0,1,true,0,false,false,6,false,7,0,6,0,0,0,0,"object"]],["step",35,[1,1,0,0,1,true,0,false,false,6,false,7,0,7,0,0,0,0,"RangeError"]],["step",36,[0,0,0,null,0,true,0,false,false,6,false,7,0,7,0,0,0,0,"RangeError"]],["step",37,[0,0,0,null,0,true,0,false,false,6,false,7,0,7,0,0,0,0,"RangeError"]],["step",38,[0,0,0,null,0,true,0,false,false,6,false,7,0,7,0,0,0,0,"RangeError"]],["step",39,[0,0,0,null,0,true,0,false,false,6,false,7,0,7,0,0,0,0,"RangeError"]],["done",[0,0,0,null,0,false,0,false,false,7,false,7,0,7,0,0,0,0,"RangeError"]]],"timers":[["write start",0],["enqueue",0,"enqueued"],["enqueue",1,"enqueued"],["enqueue",2,"enqueued"],["enqueue",3,"enqueued"],["tick",0,false,[4,4,3,5,1,true,0,false,false,4,true,4,0,0,0,0,0,0,null]],["write end",0],["write start",1],["tick",1,false,[3,3,2,10,1,true,0,false,false,4,true,4,1,0,0,0,0,0,null]],["tick",2,false,[3,3,2,15,1,true,0,false,false,4,true,4,1,0,0,0,0,0,null]],["tick",3,false,[3,3,2,20,1,true,0,false,false,4,true,4,1,0,0,0,0,0,null]],["tick",4,false,[3,3,2,25,1,true,0,false,false,4,true,4,1,0,0,0,0,0,null]],["write end",1],["write start",2],["closed",[2,2,1,25,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",5,true,[2,2,1,30,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",6,true,[2,2,1,35,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",7,true,[2,2,1,40,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",8,true,[2,2,1,45,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",9,true,[2,2,1,50,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["tick",10,true,[2,2,1,55,1,false,1,true,true,4,true,4,2,0,0,0,0,0,null]],["write end",2],["tick",11,true,[1,1,1,60,0,false,1,false,true,4,true,4,3,0,0,0,0,0,null]],["tick",12,true,[1,1,1,65,0,false,1,false,true,4,true,4,3,0,0,0,0,0,null]],["tick",13,true,[1,1,1,70,0,false,1,false,true,4,true,4,3,0,0,0,0,0,null]],["tick",14,true,[1,1,1,75,0,false,1,false,true,4,true,4,3,0,0,0,0,0,null]],["tick",15,true,[1,1,1,80,0,false,1,false,true,4,true,4,3,0,0,0,0,0,null]],["enqueue after close","dropped_closed"],["flush after close",[1,1,1,80,0,false,1,false,true,4,true,4,3,0,0,0,0,1,null]]],"throwingClock":[["write",1],["enqueue","enqueued"],["enqueue","enqueued"],["after failing drain",[1,1,1,0,0,false,0,false,false,2,false,2,1,1,0,0,0,0,"RangeError"]],["after failing flush",[1,1,1,0,0,false,0,false,false,2,false,2,1,2,0,0,0,0,"RangeError"]],["write",2],["step",0,[1,1,0,0,1,true,0,false,false,2,false,2,1,2,0,0,0,0,"RangeError"]],["step",1,[1,1,0,0,1,true,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]],["step",2,[0,0,0,null,0,true,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]],["step",3,[0,0,0,null,0,true,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]],["step",4,[0,0,0,null,0,true,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]],["step",5,[0,0,0,null,0,true,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]],["done",[0,0,0,null,0,false,0,false,false,2,false,2,1,3,0,0,0,0,"RangeError"]]],"aging":[["bounds",{"maxEntries":256,"maxBytes":262144,"maxAgeMs":100,"maxConcurrentWrites":1,"maxDrainWaitMs":5000}],["write",{"a":1}],["enqueue","enqueued",[1,7,0,0,1,true,0,false,false,0,false,1,0,0,0,0,0,0,null]],["enqueue","enqueued",[2,10,1,0,1,true,0,false,false,1,false,2,0,0,0,0,0,0,null]],["enqueue","enqueued",[3,17,2,0,1,true,0,false,false,2,false,3,0,0,0,0,0,0,null]],["enqueue","enqueued",[4,21,3,0,1,true,0,false,false,3,false,4,0,0,0,0,0,0,null]],["enqueue","enqueued",[5,43,4,0,1,true,0,false,false,4,false,5,0,0,0,0,0,0,null]],["step",0,[5,43,4,150,1,true,0,false,false,4,false,5,1,0,0,0,0,0,null]],["step",1,[4,36,4,150,0,true,0,false,false,4,false,5,1,0,0,0,0,0,null]],["step",2,[4,36,4,150,0,true,0,false,false,4,false,5,1,0,0,0,0,0,null]],["step",3,[4,36,4,150,0,true,0,false,false,4,false,5,1,0,0,0,0,0,null]],["step",4,[4,36,4,150,0,true,0,false,false,4,false,5,1,0,0,0,0,0,null]],["step",5,[0,0,0,null,0,true,0,false,false,4,false,5,1,0,0,0,4,0,null]],["step",6,[0,0,0,null,0,true,0,false,false,4,false,5,1,0,0,0,4,0,null]],["step",7,[0,0,0,null,0,false,0,false,false,4,false,5,1,0,0,0,4,0,null]],["write","late"],["enqueue late","enqueued",[1,6,0,0,1,true,0,false,false,4,false,6,1,0,0,0,4,0,null]],["late step",0,[1,6,0,50,1,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",1,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",2,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",3,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",4,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",5,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",6,[0,0,0,null,0,true,0,false,false,4,false,6,2,0,0,0,4,0,null]],["late step",7,[0,0,0,null,0,false,0,false,false,4,false,6,2,0,0,0,4,0,null]],["done",[0,0,0,null,0,false,0,false,false,4,false,6,2,0,0,0,4,0,null]]]})TRACES";

  const json::array& expectedTrace(std::string_view name) {
    static const json::object traces = rt::jsonParse(TRACES).as_object();
    return *rt::getArray(traces, name);
  }

  void expectTrace(const json::array& actual, std::string_view name) {
    const json::array& expected = expectedTrace(name);
    const std::size_t count = std::max(actual.size(), expected.size());
    for (std::size_t index = 0; index < count; ++index) {
      const json::value* left = rt::get(actual, index);
      const json::value* right = rt::get(expected, index);
      if (left == nullptr || right == nullptr || !rt::deepEqual(*left, *right)) {
        ::uos::rt::test::report(__FILE__, __LINE__,
                                std::string(name) + " trace differs at entry " + std::to_string(index) + "\n      actual:   " +
                                  (left ? rt::jsonStringify(*left) : std::string("<missing>")) + "\n      expected: " +
                                  (right ? rt::jsonStringify(*right) : std::string("<missing>")),
                                false);
        return;
      }
    }
  }

  json::value number(double value) { return rt::makeNumber(value); }

  json::value summary(const OptionalTelemetryQueueSnapshot& s) {
    json::array out;
    out.push_back(rt::makeNumber(s.retained_entries));
    out.push_back(number(s.retained_bytes));
    out.push_back(rt::makeNumber(s.queued_entries));
    out.push_back(s.oldest_retained_age_ms ? number(*s.oldest_retained_age_ms) : json::value(nullptr));
    out.push_back(rt::makeNumber(s.writes_in_flight));
    out.push_back(json::value(s.drain_in_progress));
    out.push_back(rt::makeNumber(s.drain_timeouts));
    out.push_back(json::value(s.dispatch_stalled));
    out.push_back(json::value(s.shutdown_incomplete));
    out.push_back(rt::makeNumber(s.coalesced_flushes));
    out.push_back(json::value(s.closed));
    out.push_back(rt::makeNumber(s.enqueued));
    out.push_back(rt::makeNumber(s.delivered));
    out.push_back(rt::makeNumber(s.failed));
    out.push_back(rt::makeNumber(s.dropped_by_entries));
    out.push_back(rt::makeNumber(s.dropped_by_bytes));
    out.push_back(rt::makeNumber(s.dropped_by_age));
    out.push_back(rt::makeNumber(s.dropped_after_closed));
    out.push_back(s.last_error_class ? json::value(*s.last_error_class) : json::value(nullptr));
    return out;
  }

  json::value row(std::initializer_list<json::value> items) { return json::array(items.begin(), items.end()); }

  using Log = std::shared_ptr<json::array>;

  rt::Task<bool> timedWrite(Log log, double entry) {
    log->push_back(row({"write start", number(entry)}));
    co_await rt::sleep(10 * (entry + 1));
    log->push_back(row({"write end", number(entry)}));
    co_return true;
  }
}

TEST("measureJsonPayloadBytes matches the recorded payload sizes") {
  const json::value nan = rt::Number::NaN;
  const std::vector<std::pair<json::value, std::optional<double>>> cases = {
    {rt::makeNumber(0.0), 1},
    {rt::makeNumber(-0.0), 1},
    {rt::makeNumber(1e21), 5},
    {rt::makeNumber(0.1), 3},
    {nan, std::nullopt},
    {json::value(rt::Number::NEGATIVE_INFINITY), std::nullopt},
    {json::value("é😀"), 8},
    {rt::jsonParse(R"("\ud800x")"), 9},
    {rt::jsonParse(R"("\u0001\n ")"), 13},
    {rt::jsonParse(R"({"a":[1,"two",{"b":null,"c":true}],"d":"ü"})"), 44},
    {rt::jsonParse(R"({"2":1,"b":2,"1":[3]})"), 21},
    {json::value(nullptr), 4},
    {json::value(json::object()), 2},
    {json::value(json::array()), 2},
    {rt::jsonParse(R"({"b":1,"10":2,"9":3})"), 20},
  };
  for (const auto& [value, expected] : cases) {
    const double measured = measureJsonPayloadBytes(value);
    if (expected) EXPECT_EQ(measured, *expected);
    else EXPECT(rt::Number::isNaN(measured));
  }
  // `{ a: [1, { b: NaN }] }` and `{ "2": 1, b: 2, "1": [Infinity] }`: nested non-finite numbers.
  json::object inner;
  inner["b"] = rt::Number::NaN;
  json::object nested;
  nested["a"] = json::array{json::value(1), json::value(std::move(inner))};
  EXPECT(rt::Number::isNaN(measureJsonPayloadBytes(json::value(nested))));
  json::object keyed = rt::jsonParse(R"({"2":1,"b":2,"1":[0]})").as_object();
  keyed["1"].as_array()[0] = rt::Number::POSITIVE_INFINITY;
  EXPECT(rt::Number::isNaN(measureJsonPayloadBytes(json::value(keyed))));
}

ASYNC_TEST("successful queue waits release their timeout promises on the running loop") {
  const auto completeWrite = []() -> rt::Task<void> {
    namespace queue = optional_telemetry_queue::detail;
    const auto state = std::make_shared<queue::QueueState<int>>();
    state->inFlight.emplace_back(rt::Promise<void>::resolve(), queue::PendingEntry<int>{.entry = 1});
    const bool progressed = co_await queue::waitForProgress(state, 60'000);
    EXPECT(progressed);
  };
  auto checking = tests::helpers::expectStableLiveAllocations(completeWrite, 128);
  co_await std::move(checking);
}

ASYNC_TEST("queue trace: immediately settling writes with two writers and coalesced flushes") {
  auto log = std::make_shared<json::array>();
  OptionalTelemetryQueueOptions<double> options;
  options.write = [log](double entry) {
    log->push_back(row({"write", number(entry)}));
    return rt::Promise<bool>::resolve(rt::mod(entry, 3) != 0);
  };
  options.bounds.maxEntries = 8;
  options.bounds.maxBytes = 4096;
  options.bounds.maxAgeMs = 60000;
  options.bounds.maxConcurrentWrites = 2;
  options.now = [] { return 5000.0; };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  for (int index = 0; index < 5; index += 1) {
    const std::string outcome = queue.enqueue(index);
    log->push_back(row({"enqueue", number(index), json::value(outcome), summary(queue.snapshot())}));
  }
  const rt::Promise<void> first = queue.flush();
  const rt::Promise<void> second = queue.flush();
  log->push_back(row({"same", json::value(first == second)}));
  auto firstDone = std::make_shared<bool>(false);
  (void)first.then([firstDone, log, queue] {
    *firstDone = true;
    log->push_back(row({"first settled", summary(queue.snapshot())}));
  });
  for (int step = 0; step < 30; step += 1) {
    co_await rt::microtask();
    log->push_back(row({"step", number(step), json::value(*firstDone), summary(queue.snapshot())}));
  }
  co_await first.wait();
  log->push_back(row({"done", summary(queue.snapshot())}));
  expectTrace(*log, "immediate");
}

ASYNC_TEST("queue trace: rejecting and throwing writers with Error and non-Error reasons") {
  auto log = std::make_shared<json::array>();
  const std::vector<rt::Reason> reasons = {
    rt::makeReason(rt::JsError("e")),
    rt::makeReason(rt::TypeError("t")),
    rt::thrown(json::value("text")),
    rt::Reason(),  // undefined
    rt::thrown(json::value(7)),
    rt::thrown(json::value(nullptr)),
    rt::makeReason(rt::RangeError("r")),
  };
  OptionalTelemetryQueueOptions<double> options;
  options.write = [log, reasons](double entry) {
    log->push_back(row({"write", number(entry)}));
    const rt::Reason reason = reasons.at(static_cast<std::size_t>(entry));
    if (entry == 4) throw rt::SyntaxError("sync throw");
    return rt::Promise<bool>::reject(reason);
  };
  options.bounds.maxConcurrentWrites = 1;
  options.now = [] { return 9000.0; };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  for (std::size_t index = 0; index < reasons.size(); index += 1) {
    const std::string outcome = queue.enqueue(static_cast<double>(index));
    log->push_back(row({"enqueue", number(static_cast<double>(index)), json::value(outcome)}));
  }
  for (int step = 0; step < 40; step += 1) {
    co_await rt::microtask();
    log->push_back(row({"step", number(step), summary(queue.snapshot())}));
  }
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  log->push_back(row({"done", summary(queue.snapshot())}));
  expectTrace(*log, "rejecting");
}

ASYNC_TEST("queue trace: timer-driven writes, the absolute drain bound and close while writes are in flight") {
  auto log = std::make_shared<json::array>();
  auto clock = std::make_shared<double>(100000);
  OptionalTelemetryQueueOptions<double> options;
  options.write = [log](double entry) { return rt::Promise<bool>(timedWrite(log, entry)); };
  options.bounds.maxEntries = 6;
  options.bounds.maxBytes = 4096;
  options.bounds.maxAgeMs = 60000;
  options.bounds.maxConcurrentWrites = 1;
  options.bounds.maxDrainWaitMs = 25;
  options.now = [clock] { return *clock; };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  for (int index = 0; index < 4; index += 1) {
    const std::string outcome = queue.enqueue(index);
    log->push_back(row({"enqueue", number(index), json::value(outcome)}));
  }
  const rt::Promise<void> closing = queue.close();
  auto closed = std::make_shared<bool>(false);
  (void)closing.then([closed, log, queue] {
    *closed = true;
    log->push_back(row({"closed", summary(queue.snapshot())}));
  });
  for (int step = 0; step < 16; step += 1) {
    co_await rt::sleep(5);
    *clock += 5;
    log->push_back(row({"tick", number(step), json::value(*closed), summary(queue.snapshot())}));
  }
  const std::string late = queue.enqueue(9);
  log->push_back(row({"enqueue after close", json::value(late)}));
  const rt::Promise<void> flushed = queue.flush();
  co_await flushed.wait();
  log->push_back(row({"flush after close", summary(queue.snapshot())}));
  expectTrace(*log, "timers");
}

ASYNC_TEST("queue trace: a clock that throws inside the drain loop rejects the drain operation") {
  auto log = std::make_shared<json::array>();
  auto failing = std::make_shared<bool>(false);
  auto releases = std::make_shared<std::deque<rt::Resolve<bool>>>();
  OptionalTelemetryQueueOptions<double> options;
  options.write = [log, releases](double entry) {
    log->push_back(row({"write", number(entry)}));
    return rt::newPromise<bool>([releases](rt::Resolve<bool> resolve, rt::Reject) { releases->push_back(resolve); });
  };
  options.bounds.maxConcurrentWrites = 1;
  options.now = [failing] {
    if (*failing) throw rt::RangeError("clock failed");
    return 2000.0;
  };
  const auto release = [releases](bool value) {
    if (releases->empty()) return;
    const rt::Resolve<bool> next = releases->front();
    releases->pop_front();
    next(value);
  };
  const OptionalTelemetryQueue<double> queue = createOptionalTelemetryQueue(options);
  const std::string first = queue.enqueue(1);
  log->push_back(row({"enqueue", json::value(first)}));
  const std::string second = queue.enqueue(2);
  log->push_back(row({"enqueue", json::value(second)}));
  const rt::Promise<void> flushed = queue.flush();
  *failing = true;
  release(true);
  co_await flushed.wait();
  *failing = false;
  log->push_back(row({"after failing drain", summary(queue.snapshot())}));
  *failing = true;
  const rt::Promise<void> again = queue.flush();
  co_await again.wait();
  *failing = false;
  log->push_back(row({"after failing flush", summary(queue.snapshot())}));
  const rt::Promise<void> last = queue.flush();
  for (int step = 0; step < 6; step += 1) {
    co_await rt::microtask();
    log->push_back(row({"step", number(step), summary(queue.snapshot())}));
    release(false);
  }
  co_await last.wait();
  log->push_back(row({"done", summary(queue.snapshot())}));
  expectTrace(*log, "throwingClock");
}

ASYNC_TEST("queue trace: default measure, invalid bounds and age drops at dispatch and before enqueue") {
  auto log = std::make_shared<json::array>();
  auto clock = std::make_shared<double>(1000);
  auto releases = std::make_shared<std::deque<rt::Resolve<bool>>>();
  OptionalTelemetryQueueOptions<json::value> options;
  options.write = [log, releases](json::value entry) {
    log->push_back(row({"write", entry}));
    return rt::newPromise<bool>([releases](rt::Resolve<bool> resolve, rt::Reject) { releases->push_back(resolve); });
  };
  options.bounds.maxEntries = 3.5;
  options.bounds.maxBytes = -1;
  options.bounds.maxAgeMs = 100;
  options.bounds.maxConcurrentWrites = 1;
  options.bounds.maxDrainWaitMs = rt::Number::NaN;
  options.now = [clock] { return *clock; };
  const auto release = [releases](bool value) {
    if (releases->empty()) return;
    const rt::Resolve<bool> next = releases->front();
    releases->pop_front();
    next(value);
  };
  const OptionalTelemetryQueue<json::value> queue = createOptionalTelemetryQueue(options);
  const auto bounds = queue.snapshot().bounds;
  json::object boundsJson;
  boundsJson["maxEntries"] = number(bounds.maxEntries);
  boundsJson["maxBytes"] = number(bounds.maxBytes);
  boundsJson["maxAgeMs"] = number(bounds.maxAgeMs);
  boundsJson["maxConcurrentWrites"] = number(bounds.maxConcurrentWrites);
  boundsJson["maxDrainWaitMs"] = number(bounds.maxDrainWaitMs);
  log->push_back(row({"bounds", json::value(std::move(boundsJson))}));
  const std::vector<json::value> entries = {rt::jsonParse(R"({"a":1})"), json::value("x"), rt::jsonParse("[1,2,3]"), json::value(nullptr), rt::jsonParse(R"({"deep":{"😀":"é"}})")};
  for (const json::value& entry : entries) {
    const std::string outcome = queue.enqueue(entry);
    log->push_back(row({"enqueue", json::value(outcome), summary(queue.snapshot())}));
  }
  *clock += 150;
  release(true);
  for (int step = 0; step < 8; step += 1) {
    co_await rt::microtask();
    log->push_back(row({"step", number(step), summary(queue.snapshot())}));
  }
  const std::string late = queue.enqueue(json::value("late"));
  log->push_back(row({"enqueue late", json::value(late), summary(queue.snapshot())}));
  *clock += 50;
  release(true);
  for (int step = 0; step < 8; step += 1) {
    co_await rt::microtask();
    log->push_back(row({"late step", number(step), summary(queue.snapshot())}));
    release(true);
  }
  log->push_back(row({"done", summary(queue.snapshot())}));
  expectTrace(*log, "aging");
}
