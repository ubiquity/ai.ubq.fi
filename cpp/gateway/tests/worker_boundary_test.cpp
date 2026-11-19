// Port of tests/worker-boundary.test.ts.
//
// The TypeScript runs real loopback servers with Bun.serve and reaches them
// through the global fetch. Here rt::defaultFetch() (Bun's fetch front end:
// validation, manual redirects, abort propagation into the upstream body) sends
// through an rt::MockTransport whose handlers are those servers, routed by
// origin; an unknown origin fails like a refused connection.
#include "uos/authority/admission_state.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/worker/entrypoint.hpp"
#include "uos/worker/transport.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::authority::contracts;
  using uos::authority::admission_state::createAuthorityState;
  using uos::authority::admission_state::PortableAuthority;
  using namespace uos::worker::transport;
  using namespace uos::worker::entrypoint;

  // `await assert.rejects(promise, /pattern/)`: String(error) must match.
  template <class T> rt::Task<void> rejectsMatching(rt::Task<T> task, const char* pattern, const char* file, int line) {
    bool rejected = false;
    std::string text;
    try {
      co_await std::move(task);
    } catch (...) {
      rejected = true;
      text = rt::reasonToString(std::current_exception());
    }
    if (!rejected) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/", false);
    else if (!rt::Regex(pattern).test(text)) rt::test::report(file, line, std::string("expected a rejection matching /") + pattern + "/, got " + text, false);
  }
#define ASSERT_REJECTS(task, pattern) co_await rejectsMatching((task), pattern, __FILE__, __LINE__)

  template <class T> rt::Task<T> resolvedTask(T value) { co_return value; }

  rt::ByteChunk encode(std::string_view text) { return rt::encodeUtf8(text); }

  // ---- loopback "servers"

  using Handler = std::function<rt::Task<rt::Response>(rt::Request request)>;
  struct Network {
    std::map<std::string, Handler> servers;
    int nextPort = 41000;
  };
  rt::Task<rt::Response> route(std::shared_ptr<Network> network, rt::Request request) {
    const std::string origin = rt::URL(request.url()).origin();
    const auto found = network->servers.find(origin);
    if (found == network->servers.end()) throw rt::TypeError("Unable to connect. Is the computer able to access the url?");
    const Handler handler = found->second;
    rt::Task<rt::Response> handling = handler(request);
    rt::Response response = co_await std::move(handling);
    co_return response;
  }
  // `Bun.serve({ hostname: "127.0.0.1", port: 0, fetch })` -> its origin; `server.stop()` erases it.
  std::string serve(const std::shared_ptr<Network>& network, Handler handler) {
    const std::string origin = "http://127.0.0.1:" + std::to_string(network->nextPort++);
    network->servers[origin] = std::move(handler);
    return origin;
  }
  struct Loopback {
    std::shared_ptr<Network> network = std::make_shared<Network>();
    std::shared_ptr<rt::FetchTransport> previous = rt::fetchTransport();
    Loopback() {
      auto routed = network;
      rt::setFetchTransport(std::make_shared<rt::MockTransport>([routed](rt::Request request) { return route(routed, std::move(request)); }));
    }
    ~Loopback() { rt::setFetchTransport(previous); }
    Loopback(const Loopback&) = delete;
    Loopback& operator=(const Loopback&) = delete;
  };

  // `fetch` as the capability's `(request: Request) => Promise<Response>`.
  rt::Task<rt::Response> globalFetch(rt::Request request) { return rt::fetch(rt::RequestInfo(std::move(request))); }

  // ---- observe(response)

  struct Observation {
    rt::Response response;
    rt::Resolve<TerminalEvidence> resolve;
    std::optional<rt::ReadableStreamDefaultReader<rt::ByteChunk>> reader;
    std::string text;
    rt::TextDecoder decoder;
  };

  TerminalEvidence evidence(const Observation& state, std::string outcome) {
    return TerminalEvidence{.outcome = std::move(outcome), .providerRequestId = state.response.headers().get("x-request-id"), .spendMicrocredits = std::nullopt, .fallback = std::nullopt};
  }

  rt::Task<void> observePull(std::shared_ptr<Observation> state, rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
    rt::Reason failure;
    try {
      std::optional<rt::ReadableStreamReadResult<rt::ByteChunk>> chunk;
      if (state->reader) {
        rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> reading = state->reader->read();
        chunk = co_await std::move(reading);
      } else {
        co_await rt::microtask();
      }
      if (!chunk || chunk->done) {
        state->resolve(evidence(*state, rt::includes(state->text, "\"status\":\"completed\"") || rt::includes(state->text, "\"finish_reason\":\"stop\"") ? "completed" : "incomplete"));
        controller.close();
        co_return;
      }
      state->text += state->decoder.decode(*chunk->value, {.stream = true});
      if (rt::length(state->text) > 32'768) throw rt::JsError("synthetic observation limit");
      if (rt::includes(state->text, "response.completed")) state->resolve(evidence(*state, "completed"));
      controller.enqueue(std::move(*chunk->value));
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      state->resolve(evidence(*state, "cancelled"));
      controller.error(failure);
    }
  }

  rt::Task<void> observeCancel(std::shared_ptr<Observation> state, rt::Reason reason) {
    state->resolve(evidence(*state, "cancelled"));
    if (state->reader) {
      rt::Task<void> cancelling = state->reader->cancel(reason);
      co_await std::move(cancelling);
    } else {
      co_await rt::microtask();
    }
  }

  UpstreamResult observe(rt::Response response) {
    auto [terminal, resolve, reject] = rt::withResolvers<TerminalEvidence>();
    (void)reject;
    auto state = std::make_shared<Observation>(Observation{.response = response, .resolve = resolve, .reader = std::nullopt, .text = "", .decoder = rt::TextDecoder()});
    if (const std::optional<rt::ByteStream> body = response.body()) state->reader = body->getReader();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { return observePull(state, std::move(controller)); };
    source.cancel = [state](rt::Reason reason) { return observeCancel(state, std::move(reason)); };
    const rt::ByteStream body(std::move(source), rt::QueuingStrategy{.highWaterMark = 0});
    rt::ResponseInit init;
    init.status = static_cast<double>(response.status());
    init.statusText = response.statusText();
    init.headers = response.headers();
    return UpstreamResult{.response = rt::Response(rt::BodyInit(body), std::move(init)), .terminal = terminal};
  }

  // ---- fixture

  struct FixtureState {
    AuthorityState state;
    double revision = 0;
    double calls = 0;
    bool outage = false;
    double outageAfterRevision = rt::Number::POSITIVE_INFINITY;
    std::vector<rt::Promise<void>> tasks;
  };

  struct OutageStore : AuthorityStore {
    std::shared_ptr<FixtureState> fixture;
    explicit OutageStore(std::shared_ptr<FixtureState> fixture_) : fixture(std::move(fixture_)) {}
    rt::Promise<AuthoritySnapshot> read() override {
      if (fixture->outage || fixture->revision >= fixture->outageAfterRevision) throw rt::JsError("synthetic authority outage");
      return rt::Promise<AuthoritySnapshot>::resolve(AuthoritySnapshot{.revision = fixture->revision, .state = fixture->state});
    }
    rt::Promise<bool> compareAndSwap(double expected, AuthorityState value) override {
      if (expected != fixture->revision) return rt::Promise<bool>::resolve(false);
      fixture->state = std::move(value);
      fixture->revision += 1;
      return rt::Promise<bool>::resolve(true);
    }
  };

  // `upstream.attempt = ...` / `upstream.dispatch = ...` assigned on the instance.
  struct PatchableTransport : UpstreamTransport {
    std::shared_ptr<HttpUpstreamTransport> inner;
    std::function<rt::Task<UpstreamAttempt>(ValidatedGatewayInput, std::optional<AccountBinding>)> attemptOverride;
    std::function<rt::Task<UpstreamResult>(ValidatedGatewayInput, UpstreamAttempt, rt::AbortSignal, std::function<rt::Task<void>()>)> dispatchOverride;
    explicit PatchableTransport(std::shared_ptr<HttpUpstreamTransport> inner_) : inner(std::move(inner_)) {}
    rt::Task<ValidatedGatewayInput> validate(GatewayRoute route, json::object body) override { return inner->validate(std::move(route), std::move(body)); }
    rt::Task<UpstreamAttempt> attempt(ValidatedGatewayInput input, std::optional<AccountBinding> account) override {
      if (attemptOverride) return attemptOverride(std::move(input), std::move(account));
      return inner->attempt(std::move(input), std::move(account));
    }
    rt::Task<UpstreamResult> dispatch(ValidatedGatewayInput input, UpstreamAttempt attempt, rt::AbortSignal signal, std::function<rt::Task<void>()> onDispatched) override {
      if (dispatchOverride) return dispatchOverride(std::move(input), std::move(attempt), std::move(signal), std::move(onDispatched));
      return inner->dispatch(std::move(input), std::move(attempt), std::move(signal), std::move(onDispatched));
    }
  };

  rt::Task<rt::Response> serveFixture(std::shared_ptr<FixtureState> fixture, rt::Request request) {
    fixture->calls += 1;
    EXPECT_EQ(request.headers().get("authorization"), std::optional<std::string>("Bearer synthetic-upstream"));
    rt::Task<json::value> reading = request.json();
    const json::value body = co_await std::move(reading);
    if (!rt::truthy(rt::get(body, "stream"))) {
      rt::ResponseInit init;
      init.headers = rt::Headers{{"x-request-id", "fixture-upstream"}};
      co_return rt::Response::json(rt::jsonParse(R"({"id":"fixture-id","status":"completed","choices":[{"message":{"role":"assistant","content":"pong"},"finish_reason":"stop"}]})"),
                                   std::move(init));
    }
    auto timer = std::make_shared<std::optional<rt::Timeout>>();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [timer](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      controller.enqueue(encode("event: response.output_text.delta\ndata: {\"delta\":\"pong\"}\n\n"));
      *timer = rt::setTimeout(
        [controller] {
          controller.enqueue(encode("event: response.completed\ndata: {\"response\":{\"status\":\"completed\"}}\n\n"));
          controller.close();
        },
        30);
    };
    source.cancel = [timer](rt::Reason) { rt::clearTimeout(*timer); };
    rt::ResponseInit init;
    init.headers = rt::Headers{{"content-type", "text/event-stream"}, {"x-request-id", "fixture-upstream"}};
    co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
  }

  struct Fixture {
    std::shared_ptr<FixtureState> shared;
    std::shared_ptr<Network> network;
    std::string origin;
    WorkerBoundary boundary;
    std::shared_ptr<PatchableTransport> upstream;
    BoundaryLifetime lifetime;
    rt::Task<void> drain() const {
      auto state = shared;
      while (!state->tasks.empty()) {
        std::vector<rt::Promise<void>> batch = std::exchange(state->tasks, {});
        co_await rt::all(std::move(batch)).wait();
      }
    }
    void stop() const { network->servers.erase(origin); }
    double calls() const { return shared->calls; }
    AuthorityState& state() const { return shared->state; }
    void outage() const { shared->outage = true; }
    void outageAfterRevision(double value) const { shared->outageAfterRevision = value; }
  };

  Fixture fixture(const std::shared_ptr<Network>& network) {
    const AuthorityScope scope{.namespace_ = "synthetic-boundary", .epoch = 1};
    auto shared = std::make_shared<FixtureState>();
    shared->state = createAuthorityState(scope,
                                         {AccountBinding{
                                           .accountId = "a",
                                           .fingerprint = "fp",
                                           .credentialGeneration = 1,
                                           .routingGeneration = 1,
                                           .resetGeneration = 1,
                                           .overageVersion = 1,
                                           .overageEnabled = false,
                                           .enabled = true,
                                           .models = {"fixture"},
                                           .proof = std::nullopt,
                                         }},
                                         {PrincipalPolicy{
                                           .principalId = "p",
                                           .version = 1,
                                           .deleted = false,
                                           .windowResetAtMs = 1'000'000,
                                           .requestLimit = 20,
                                           .kernelLimit = 20,
                                           .concurrencyLimit = 20,
                                           .paidLimitMicrocredits = 0,
                                         }});
    const auto store = std::make_shared<OutageStore>(shared);
    const std::string origin = serve(network, [shared](rt::Request request) { return serveFixture(shared, std::move(request)); });
    HttpUpstreamCapability capability{
      .provider = "codex",
      .fetch = globalFetch,
      .baseUrl = rt::URL(origin).origin(),
      .headers = [](std::optional<AccountBinding>) { return resolvedTask(rt::Headers{{"authorization", "Bearer synthetic-upstream"}}); },
      .validate =
        [](GatewayRoute route, json::object body) {
          const std::vector<std::string> fields{"model", "stream", route == "/v1/responses" ? "input" : "messages"};
          const json::value* model = rt::get(body, "model");
          const json::value* stream = rt::get(body, "stream");
          const std::vector<std::string> keys = rt::objectKeys(body);
          if (model == nullptr || !rt::strictEquals(*model, json::value("fixture")) ||
              std::any_of(keys.begin(), keys.end(), [&fields](const std::string& key) { return std::find(fields.begin(), fields.end(), key) == fields.end(); }) ||
              (stream != nullptr && !stream->is_bool()))
            throw AuthorityError("invalid_fixture_schema", 400);
          const bool streaming = stream != nullptr && stream->is_bool() && stream->get_bool();
          return resolvedTask(ValidatedGatewayInput{.route = route, .model = "fixture", .stream = streaming, .body = std::move(body)});
        },
      .observe = [](rt::Response response, ValidatedGatewayInput) { return observe(std::move(response)); },
      .maximumExposureMicrocredits = 0,
    };
    const auto upstream = std::make_shared<PatchableTransport>(std::make_shared<HttpUpstreamTransport>(std::move(capability)));
    WorkerBoundary boundary = createWorkerBoundary(WorkerBoundaryDependencies{
      .scope = scope,
      .authority = std::make_shared<PortableAuthority>(store, scope),
      .upstream = upstream,
      .authenticate =
        [](rt::Request request) {
          const std::optional<BoundaryPrincipal> principal =
            request.headers().get("authorization") == std::optional<std::string>("Bearer synthetic-client") ? std::optional<BoundaryPrincipal>(BoundaryPrincipal{.principalId = "p", .policyVersion = 1})
                                                                                                             : std::nullopt;
          return resolvedTask(principal);
        },
      .now = [] { return 100.0; },
    });
    const BoundaryLifetime lifetime{.waitUntil = [shared](rt::Promise<void> task) { shared->tasks.push_back(std::move(task)); }};
    return Fixture{.shared = shared, .network = network, .origin = origin, .boundary = boundary, .upstream = upstream, .lifetime = lifetime};
  }

  rt::Request request(const json::value& body, const std::string& path = "/v1/responses", const std::string& auth = "Bearer synthetic-client",
                      std::optional<rt::AbortSignal> signal = std::nullopt) {
    rt::RequestInit init;
    init.method = "POST";
    init.headers = rt::Headers{{"authorization", auth}, {"content-type", "application/json"}};
    init.body = rt::BodyInit(rt::jsonStringify(body));
    init.signal = std::move(signal);
    return rt::Request("https://fixture.invalid" + path, std::move(init));
  }

  json::value object(std::initializer_list<std::pair<std::string_view, json::value>> members) {
    json::object out;
    for (const auto& [key, value] : members) out[key] = value;
    return out;
  }

  std::string decodeChunk(const rt::ReadableStreamReadResult<rt::ByteChunk>& chunk) { return chunk.value ? rt::decodeUtf8(*chunk.value) : rt::decodeUtf8(std::string_view()); }

  const AdmissionRecord& firstAdmission(const AuthorityState& state) { return *state.admissions.values().at(0); }
  const PrincipalWindow& firstWindow(const AuthorityState& state) { return *state.windows.values().at(0); }

  rt::Task<UpstreamResult> ownedResponse(std::shared_ptr<int> owner, std::function<rt::Task<void>()> onDispatched, std::optional<rt::AbortController> abort,
                                        std::shared_ptr<std::weak_ptr<const void>> observed = nullptr) {
    auto marking = onDispatched();
    co_await std::move(marking);
    if (abort) abort->abort(rt::makeReason(rt::JsError("fixture dispatch interrupted")));
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [owner](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      controller.enqueue(encode(std::to_string(*owner)));
      controller.close();
    };
    const rt::ByteStream body(std::move(source), rt::QueuingStrategy{.highWaterMark = 0});
    const auto terminal = rt::Promise<TerminalEvidence>::resolve(TerminalEvidence{
      .outcome = "completed", .providerRequestId = std::nullopt, .spendMicrocredits = std::nullopt, .fallback = std::nullopt,
    });
    const rt::Response response{rt::BodyInit(body)};
    if (observed) *observed = response.weakIdentity();
    co_return UpstreamResult{.response = response, .terminal = terminal};
  }
}

ASYNC_TEST("authenticated Responses/chat JSON and progressive SSE use actual loopback HTTP; EOF remains unobserved delivery") {
  // The agent environment carries some provider credentials; the TypeScript runs
  // without them, so the C++ test clears the environment first (never printed).
  const rt::env::ScopedEnvironment clean(std::vector<std::pair<std::string, std::string>>{});
  bool anySecret = false;
  const std::vector<std::string> keys{"CODEX_AUTH_JSON_B64", "SURPLUS_API_KEY", "METERED_API_KEY", "OPENROUTER_API_KEY"};
  for (const std::string& key : keys) anySecret = anySecret || rt::truthy(rt::env::get(key));
  EXPECT_EQ(anySecret, false);
  const Loopback loopback;
  const Fixture f = fixture(loopback.network);
  const std::vector<std::string> paths{"/v1/responses", "/v1/chat/completions"};
  for (const std::string& path : paths) {
    const json::value body = path == "/v1/responses" ? object({{"model", "fixture"}, {"input", "ping"}}) : object({{"model", "fixture"}, {"messages", json::array{}}});
    rt::Task<rt::Response> fetching = f.boundary.fetch(request(body, path), f.lifetime);
    const rt::Response response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    rt::Task<std::string> reading = response.text();
    const std::string text = co_await std::move(reading);
    EXPECT(rt::includes(text, "pong"));
  }
  rt::Task<rt::Response> fetching = f.boundary.fetch(request(object({{"model", "fixture"}, {"input", "ping"}, {"stream", true}})), f.lifetime);
  const rt::Response response = co_await std::move(fetching);
  const auto reader = response.body()->getReader();
  rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> first = reader.read();
  const auto firstChunk = co_await std::move(first);
  EXPECT(rt::includes(decodeChunk(firstChunk), "response.output_text.delta"));
  rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> second = reader.read();
  const auto secondChunk = co_await std::move(second);
  EXPECT(rt::includes(decodeChunk(secondChunk), "response.completed"));
  rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> third = reader.read();
  const auto thirdChunk = co_await std::move(third);
  EXPECT_EQ(thirdChunk.done, true);
  rt::Task<void> draining = f.drain();
  co_await std::move(draining);
  EXPECT_EQ(f.calls(), 3);
  const std::vector<const AdmissionRecord*> admissions = std::as_const(f.state().admissions).values();
  EXPECT(std::all_of(admissions.begin(), admissions.end(), [](const AdmissionRecord* value) {
    return value->semantic == std::optional<std::string>("completed") && value->delivery == std::optional<std::string>("unobserved") && value->settled;
  }));
  EXPECT_EQ(firstWindow(f.state()).kernelCompleted, 3);
  f.stop();
}

ASYNC_TEST("auth/schema/unsupported route, early cancellation and authority outage dispatch zero calls") {
  const Loopback loopback;
  const Fixture f = fixture(loopback.network);
  rt::Task<rt::Response> unauthenticated = f.boundary.fetch(request(object({{"model", "fixture"}}), "/v1/responses", "invalid"), f.lifetime);
  EXPECT_EQ((co_await std::move(unauthenticated)).status(), 401);
  rt::Task<rt::Response> schema = f.boundary.fetch(request(object({{"model", "fixture"}, {"upstreamUrl", "https://forbidden.invalid"}})), f.lifetime);
  EXPECT_EQ((co_await std::move(schema)).status(), 400);
  rt::Task<rt::Response> unsupported = f.boundary.fetch(request(json::object{}, "/admin/native/refresh"), f.lifetime);
  EXPECT_EQ((co_await std::move(unsupported)).status(), 501);
  const rt::AbortController abort;
  abort.abort();
  rt::Task<rt::Response> cancelled = f.boundary.fetch(request(object({{"model", "fixture"}}), "/v1/responses", "Bearer synthetic-client", abort.signal()), f.lifetime);
  EXPECT_EQ((co_await std::move(cancelled)).status(), 499);
  f.outage();
  rt::Task<rt::Response> outage = f.boundary.fetch(request(object({{"model", "fixture"}})), f.lifetime);
  EXPECT_EQ((co_await std::move(outage)).status(), 502);
  EXPECT_EQ(f.calls(), 0);
  f.stop();
}

ASYNC_TEST("midstream body cancellation propagates abort and settles one admission") {
  const Loopback loopback;
  const Fixture f = fixture(loopback.network);
  rt::Task<rt::Response> fetching = f.boundary.fetch(request(object({{"model", "fixture"}, {"stream", true}})), f.lifetime);
  const rt::Response response = co_await std::move(fetching);
  const auto reader = response.body()->getReader();
  rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> reading = reader.read();
  co_await std::move(reading);
  rt::Task<void> cancelling = reader.cancel(rt::thrown(json::value("synthetic disconnect")));
  co_await std::move(cancelling);
  rt::Task<void> draining = f.drain();
  co_await std::move(draining);
  EXPECT_EQ(f.calls(), 1);
  const AdmissionRecord& admission = firstAdmission(f.state());
  EXPECT_EQ(admission.delivery, std::optional<std::string>("interrupted"));
  EXPECT_EQ(admission.settled, true);
  const PrincipalWindow& window = firstWindow(f.state());
  EXPECT_EQ(window.active, 0);
  EXPECT_EQ(window.dispatched, 1);
  EXPECT_EQ(window.kernelReserved, 0);
  f.stop();
}

TEST("an abandoned worker authentication wait releases the request") {
  std::weak_ptr<const void> weakRequest;
  {
    rt::Loop loop;
    const rt::LoopScope scope(loop);
    const rt::Promise<std::optional<BoundaryPrincipal>> authentication;
    const auto started = std::make_shared<bool>(false);
    WorkerBoundaryDependencies dependencies{};
    dependencies.authenticate = [authentication, started](rt::Request) {
      *started = true;
      return authentication.wait();
    };
    const auto boundary = createWorkerBoundary(std::move(dependencies));
    auto input = request(object({{"model", "fixture"}}));
    weakRequest = input.weakIdentity();
    const rt::Promise<rt::Response> pending(boundary.fetch(std::move(input), BoundaryLifetime{}));
    loop.run();
    EXPECT(*started);
    EXPECT(pending.pending());
    EXPECT(!weakRequest.expired());
  }
  EXPECT(weakRequest.expired());
}

TEST("an abandoned worker body read drops its listener even while the caller owns the signal") {
  const rt::AbortController caller;
  std::weak_ptr<int> weakBody;
  {
    rt::Loop loop;
    const rt::LoopScope scope(loop);
    WorkerBoundaryDependencies dependencies{};
    dependencies.authenticate = [](rt::Request) { return resolvedTask(std::optional<BoundaryPrincipal>(BoundaryPrincipal{.principalId = "p", .policyVersion = 1})); };
    const auto boundary = createWorkerBoundary(std::move(dependencies));
    const auto started = std::make_shared<bool>(false);
    rt::RequestInit init;
    init.method = "POST";
    init.duplex = "half";
    init.signal = caller.signal();
    {
      const auto owner = std::make_shared<int>(1);
      weakBody = owner;
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.pull = [owner, started](const rt::ReadableStreamDefaultController<rt::ByteChunk>&) { *started = *owner == 1; };
      init.body = rt::BodyInit(rt::ByteStream(std::move(source), rt::QueuingStrategy{.highWaterMark = 0}));
    }
    rt::Request input("https://fixture.invalid/v1/responses", std::move(init));
    const rt::Promise<rt::Response> pending(boundary.fetch(std::move(input), BoundaryLifetime{}));
    loop.run();
    EXPECT(*started);
    EXPECT(pending.pending());
    EXPECT(!weakBody.expired());
  }
  EXPECT(weakBody.expired());
  EXPECT(!caller.signal().aborted());
}

ASYNC_TEST("dropping a worker response releases an unread body, including after dispatch abort") {
  const Loopback loopback;
  const bool abortCases[] = {false, true};
  for (const bool abortDuringDispatch : abortCases) {
    const Fixture f = fixture(loopback.network);
    const rt::AbortController caller;
    auto owner = std::make_shared<int>(1);
    const std::weak_ptr<int> weakBody = owner;
    const auto observed = std::make_shared<std::weak_ptr<const void>>();
    const std::optional<rt::AbortController> abort = abortDuringDispatch ? std::optional(caller) : std::nullopt;
    f.upstream->dispatchOverride = [owner, abort, observed](ValidatedGatewayInput, UpstreamAttempt, rt::AbortSignal, std::function<rt::Task<void>()> onDispatched) {
      return ownedResponse(owner, std::move(onDispatched), abort, observed);
    };
    {
      auto input = request(object({{"model", "fixture"}, {"stream", true}}), "/v1/responses", "Bearer synthetic-client", caller.signal());
      auto fetching = f.boundary.fetch(std::move(input), f.lifetime);
      const auto response = co_await std::move(fetching);
      EXPECT_EQ(response.status(), 200);
      EXPECT(response.body().has_value());
    }
    f.upstream->dispatchOverride = nullptr;
    owner.reset();
    auto draining = f.drain();
    co_await std::move(draining);
    EXPECT(weakBody.expired());
    EXPECT(observed->expired());
    const AdmissionRecord& admission = firstAdmission(f.state());
    EXPECT_EQ(admission.semantic, std::optional<std::string>("completed"));
    if (abortDuringDispatch) EXPECT_EQ(admission.delivery, std::optional<std::string>("interrupted"));
    else EXPECT(!admission.delivery);
    EXPECT_EQ(admission.settled, abortDuringDispatch);
    f.stop();
  }
}

ASYNC_TEST("a pending worker delivery receipt leaves the dropped body free and settles when it arrives") {
  const Loopback loopback;
  const Fixture f = fixture(loopback.network);
  auto owner = std::make_shared<int>(1);
  const std::weak_ptr<int> weakBody = owner;
  f.upstream->dispatchOverride = [owner](ValidatedGatewayInput, UpstreamAttempt, rt::AbortSignal, std::function<rt::Task<void>()> onDispatched) {
    return ownedResponse(owner, std::move(onDispatched), std::nullopt);
  };
  const auto receipt = rt::withResolvers<DeliveryOutcome>();
  {
    auto input = request(object({{"model", "fixture"}, {"stream", true}}));
    const BoundaryDelivery delivery{.completed = receipt.promise};
    auto fetching = f.boundary.fetch(std::move(input), f.lifetime, delivery);
    const auto response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    EXPECT(response.body().has_value());
  }
  f.upstream->dispatchOverride = nullptr;
  owner.reset();
  // Let the returned stream's start reaction release its temporary ownership.
  co_await rt::microtask();
  EXPECT(receipt.promise.pending());
  EXPECT(weakBody.expired());
  EXPECT(!firstAdmission(f.state()).delivery);
  receipt.resolve(std::string("delivered"));
  auto draining = f.drain();
  co_await std::move(draining);
  const AdmissionRecord& admission = firstAdmission(f.state());
  EXPECT_EQ(admission.semantic, std::optional<std::string>("completed"));
  EXPECT_EQ(admission.delivery, std::optional<std::string>("delivered"));
  EXPECT(admission.settled);
  EXPECT_EQ(firstWindow(f.state()).kernelCompleted, 1);
  EXPECT_EQ(firstWindow(f.state()).active, 0);
  f.stop();
}

ASYNC_TEST("fixed upstream refuses all redirect hops without forwarding synthetic credentials or request content") {
  const Loopback loopback;
  auto escaped = std::make_shared<double>(0);
  auto dispatched = std::make_shared<double>(0);
  auto observed = std::make_shared<double>(0);
  auto status = std::make_shared<double>(307);
  const std::string sink = serve(loopback.network, [escaped](rt::Request) {
    *escaped += 1;
    return resolvedTask(rt::Response::json(json::object{{"status", "completed"}}));
  });
  const std::string sinkHref = rt::URL(sink).href();
  const std::string upstream = serve(loopback.network, [status, sinkHref](rt::Request received) {
    EXPECT_EQ(received.headers().get("authorization"), std::optional<std::string>("Bearer synthetic-only"));
    rt::ResponseInit init;
    init.status = *status;
    init.headers = rt::Headers{{"location", sinkHref}};
    return resolvedTask(rt::Response(nullptr, std::move(init)));
  });
  const auto transport = std::make_shared<HttpUpstreamTransport>(HttpUpstreamCapability{
    .provider = "surplus",
    .fetch = globalFetch,
    .baseUrl = rt::URL(upstream).origin(),
    .headers = [](std::optional<AccountBinding>) { return resolvedTask(rt::Headers{{"authorization", "Bearer synthetic-only"}}); },
    .validate = [](GatewayRoute route, json::object body) { return resolvedTask(ValidatedGatewayInput{.route = route, .model = "fixture", .stream = false, .body = std::move(body)}); },
    .observe =
      [observed](rt::Response response, ValidatedGatewayInput) {
        *observed += 1;
        return observe(std::move(response));
      },
    .maximumExposureMicrocredits = 1,
  });
  rt::Task<ValidatedGatewayInput> validating = transport->validate("/v1/responses", json::object{{"model", "fixture"}, {"input", "synthetic-content"}});
  const ValidatedGatewayInput input = co_await std::move(validating);
  rt::Task<UpstreamAttempt> attempting = transport->attempt(input, std::nullopt);
  const UpstreamAttempt attempt = co_await std::move(attempting);
  const std::vector<double> codes{301, 302, 303, 307, 308};
  for (const double code : codes) {
    *status = code;
    std::function<rt::Task<void>()> onDispatched = [dispatched] {
      *dispatched += 1;
      return rt::Promise<void>::resolve().wait();
    };
    rt::Task<UpstreamResult> dispatching = transport->dispatch(input, attempt, rt::AbortController().signal(), std::move(onDispatched));
    ASSERT_REJECTS(std::move(dispatching), "upstream_redirect_refused");
  }
  EXPECT_EQ(*escaped, 0);
  EXPECT_EQ(*dispatched, 5);
  EXPECT_EQ(*observed, 0);
  loopback.network->servers.erase(upstream);
  loopback.network->servers.erase(sink);
}

ASYNC_TEST("authority cleanup outage returns structured refusal and preserves unresolved pre-dispatch or intent state") {
  const Loopback loopback;
  const std::vector<double> revisions{1, 3, 4};
  for (const double revision : revisions) {
    const Fixture f = fixture(loopback.network);
    f.outageAfterRevision(revision);
    rt::Task<rt::Response> fetching = f.boundary.fetch(request(object({{"model", "fixture"}})), f.lifetime);
    const rt::Response response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 502);
    rt::Task<json::value> reading = response.json();
    const json::value body = co_await std::move(reading);
    EXPECT(rt::matchesObject(body, rt::jsonParse(R"({"error":{"code":"upstream_unavailable","type":"gateway_error"}})")));
    EXPECT_EQ(f.calls(), 0);
    const AdmissionRecord& admission = firstAdmission(f.state());
    EXPECT(!admission.semantic && !admission.delivery && !admission.settled);
    const PrincipalWindow& window = firstWindow(f.state());
    EXPECT_EQ(window.active, 1);
    EXPECT_EQ(window.reserved, 1);
    EXPECT_EQ(window.kernelReserved, 1);
    if (revision == 4) EXPECT_EQ(std::as_const(admission.attempts).values().at(0)->state, "dispatch_intent");
    f.stop();
  }
}

ASYNC_TEST("cleanup outage preserves the original authority refusal and dispatched provider exposure") {
  const Loopback loopback;
  {
    const Fixture refused = fixture(loopback.network);
    refused.upstream->attemptOverride = [refused](ValidatedGatewayInput, std::optional<AccountBinding>) -> rt::Task<UpstreamAttempt> {
      refused.outage();
      throw AuthorityError("fixed_capability_refused", 403);
    };
    rt::Task<rt::Response> fetching = refused.boundary.fetch(request(object({{"model", "fixture"}})), refused.lifetime);
    const rt::Response response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 403);
    rt::Task<json::value> reading = response.json();
    const json::value body = co_await std::move(reading);
    EXPECT(rt::matchesObject(body, rt::jsonParse(R"({"error":{"code":"fixed_capability_refused"}})")));
    EXPECT_EQ(refused.calls(), 0);
    refused.upstream->attemptOverride = nullptr;
    refused.stop();
  }
  const Fixture dispatched = fixture(loopback.network);
  const std::shared_ptr<HttpUpstreamTransport> inner = dispatched.upstream->inner;
  const auto shared = dispatched.shared;
  dispatched.upstream->dispatchOverride = [inner, shared](ValidatedGatewayInput input, UpstreamAttempt attempt, rt::AbortSignal signal, std::function<rt::Task<void>()> onDispatched) {
    return rt::async([inner, shared, input, attempt, signal, onDispatched]() -> rt::Task<UpstreamResult> {
      rt::Task<UpstreamResult> dispatching = inner->dispatch(input, attempt, signal, onDispatched);
      const UpstreamResult result = co_await std::move(dispatching);
      shared->outage = true;
      if (const std::optional<rt::ByteStream> body = result.response.body()) {
        rt::Task<void> cancelling = body->cancel(rt::thrown(json::value("synthetic transport failure")));
        co_await std::move(cancelling);
      }
      throw rt::JsError("synthetic failure after provider exposure");
    });
  };
  rt::Task<rt::Response> fetching = dispatched.boundary.fetch(request(object({{"model", "fixture"}})), dispatched.lifetime);
  const rt::Response response = co_await std::move(fetching);
  EXPECT_EQ(response.status(), 502);
  EXPECT_EQ(dispatched.calls(), 1);
  const AdmissionRecord& admission = firstAdmission(dispatched.state());
  EXPECT(admission.dispatched && !admission.semantic && !admission.delivery && !admission.settled);
  const AttemptRecord& attempt = *std::as_const(admission.attempts).values().at(0);
  EXPECT(attempt.state == "dispatched" && !attempt.terminal && !attempt.billingSettled);
  const PrincipalWindow& window = firstWindow(dispatched.state());
  EXPECT_EQ(window.active, 1);
  EXPECT_EQ(window.dispatched, 1);
  EXPECT_EQ(window.kernelReserved, 1);
  dispatched.upstream->dispatchOverride = nullptr;
  dispatched.stop();
}
