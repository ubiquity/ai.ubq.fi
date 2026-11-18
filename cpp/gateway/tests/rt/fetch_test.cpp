// fetch front end, globalThis.fetch replacement and the transport seam. Expected
// errors and orderings were observed with Bun 1.4.0 (fetch against a local
// Bun.serve; see docs/rt/web.md).
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

using namespace uos::rt;

namespace {
  // Installs a MockTransport for the scope and restores the previous one.
  struct TransportScope {
    std::shared_ptr<FetchTransport> previous = fetchTransport();
    std::shared_ptr<MockTransport> transport;
    explicit TransportScope(MockTransport::Handler handler) : transport(std::make_shared<MockTransport>(std::move(handler))) { setFetchTransport(transport); }
    ~TransportScope() { setFetchTransport(previous); }
  };

  Task<Response> echo(Request request) {
    std::string body = "<null>";
    if (request.body()) {
      Task<std::string> reading = request.text();
      body = co_await std::move(reading);
    }
    Response response(BodyInit(request.method() + " " + request.url() + " " + body));
    response.setFetchResult(request.url(), false);
    co_return response;
  }

  RequestInfo info(const char* url) { return RequestInfo(std::string(url)); }
}

ASYNC_TEST("Fetch values compare by identity like JS functions") {
  const Fetch original = globalFetch();
  EXPECT(original == defaultFetch());
  const Fetch mock([](RequestInfo, RequestInit) { return Response(BodyInit(std::string("mocked"))); });
  const Fetch copy = mock;
  EXPECT(copy == mock);
  EXPECT(!(mock == original));
  EXPECT(!(Fetch([](RequestInfo, RequestInit) { return Response(); }) == mock));
  {
    GlobalFetchOverride scope(mock);
    EXPECT(globalFetch() == mock);
    EXPECT(!(globalFetch() == defaultFetch()));
    Task<Response> responseCall = fetch(info("http://a/"));
    Response response = co_await std::move(responseCall);
    Task<std::string> responseTextReading = response.text();
    const std::string responseText = co_await std::move(responseTextReading);
    EXPECT_EQ(responseText, "mocked");
  }
  EXPECT(globalFetch() == original);
}

ASYNC_TEST("Fetch keeps borrowed inputs after returning its callback's Task") {
  const std::string address = "http://example.test/" + std::string(128, 'x');
  Fetch fetcher([](const RequestInfo& input, const RequestInit& init) -> Task<Response> {
    co_await microtask();
    co_return Response(BodyInit(init.method.value_or("") + " " + std::get<std::string>(input)));
  });
  RequestInit init;
  init.method = "POST";
  auto task = fetcher(RequestInfo(address), std::move(init));
  auto response = co_await std::move(task);
  const auto text = co_await response.text();
  EXPECT_EQ(text, "POST " + address);
}

ASYNC_TEST("a global Fetch can replace itself during its eager invocation") {
  GlobalFetchOverride scope(Fetch{});
  auto payload = std::make_shared<std::string>("original");
  setGlobalFetch(Fetch([payload](RequestInfo, RequestInit) {
    setGlobalFetch(Fetch([](RequestInfo, RequestInit) { return Response(BodyInit(std::string("replacement"))); }));
    return Response(BodyInit(*payload));
  }));
  payload.reset();

  auto original = fetch(info("http://example.test/"));
  auto replacement = fetch(info("http://example.test/"));
  auto fromOriginal = co_await std::move(original);
  auto fromReplacement = co_await std::move(replacement);
  const auto originalText = co_await fromOriginal.text();
  const auto replacementText = co_await fromReplacement.text();
  EXPECT_EQ(originalText, "original");
  EXPECT_EQ(replacementText, "replacement");
}

ASYNC_TEST("a MockTransport Task keeps its handler and borrowed request after the mock is destroyed") {
  auto mock = std::make_unique<MockTransport>([prefix = std::string(128, 'x')](const Request& request) -> Task<Response> {
    co_await microtask();
    co_return Response(BodyInit(prefix + request.url()));
  });
  auto task = mock->send(Request("http://example.test/"));
  EXPECT_EQ(mock->requests().size(), 1u);
  mock.reset();

  auto response = co_await std::move(task);
  const auto text = co_await response.text();
  EXPECT_EQ(text, std::string(128, 'x') + "http://example.test/");
}

ASYNC_TEST("a replaced globalThis.fetch receives input and init unchanged") {
  auto seen = std::make_shared<std::vector<std::string>>();
  const Fetch mock([seen](RequestInfo input, RequestInit init) -> Task<Response> {
    std::string url;
    if (auto* text = std::get_if<std::string>(&input)) url = *text;
    else if (auto* object = std::get_if<URL>(&input)) url = object->href();
    else url = std::get<Request>(input).url();
    const auto* body = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
    const Headers headers(init.headers.value_or(Headers()));
    seen->push_back(url + "|" + init.method.value_or("<none>") + "|" + (body ? *body : "<not a string>") + "|" + headers.get("authorization").value_or("<none>"));
    ResponseInit created;
    created.status = 201;
    co_return Response(BodyInit(std::string("ok")), created);
  });
  GlobalFetchOverride scope(mock);
  RequestInit init;
  init.method = "POST";
  init.headers = Headers{{"Authorization", "Bearer k"}};
  init.body = BodyInit(std::string("{\"a\":1}"));
  Task<Response> responseCall = fetch(info("http://a.test/v1"), std::move(init));
  Response response = co_await std::move(responseCall);
  EXPECT_EQ(response.status(), 201);
  Task<std::string> responseTextReading = response.text();
  const std::string responseText = co_await std::move(responseTextReading);
  EXPECT_EQ(responseText, "ok");
  RequestInit bytesInit;
  bytesInit.body = BodyInit(Bytes{1, 2});
  RequestInfo urlInput(URL("http://b.test/x?y"));
  Task<Response> ignored1 = fetch(std::move(urlInput), std::move(bytesInit));
  (void)co_await std::move(ignored1);
  RequestInfo requestInput(Request("http://c.test/"));
  Task<Response> ignored2 = fetch(std::move(requestInput));
  (void)co_await std::move(ignored2);
  const std::vector<std::string> expected = {"http://a.test/v1|POST|{\"a\":1}|Bearer k", "http://b.test/x?y|<none>|<not a string>|<none>", "http://c.test/|<none>|<not a string>|<none>"};
  EXPECT_EQ(*seen, expected);
}

ASYNC_TEST("default fetch validates its input like Bun") {
  TransportScope scope(echo);
  Task<Response> rejected1 = fetch(info(""));
  EXPECT_REJECTS_MESSAGE(std::move(rejected1), CodedTypeError, "fetch() URL must not be a blank string.");
  Task<Response> rejected2 = fetch(info("  "));
  EXPECT_REJECTS_MESSAGE(std::move(rejected2), CodedTypeError, "fetch() URL is invalid");
  Task<Response> rejected3 = fetch(info("http://"));
  EXPECT_REJECTS_MESSAGE(std::move(rejected3), CodedTypeError, "fetch() URL is invalid");
  Task<Response> rejected4 = fetch(info("/relative"));
  EXPECT_REJECTS_MESSAGE(std::move(rejected4), CodedTypeError, "fetch() URL is invalid");
  Task<Response> rejected5 = fetch(info("ws://127.0.0.1:1/"));
  EXPECT_REJECTS_MESSAGE(std::move(rejected5), CodedTypeError, "protocol must be http:, https: or s3:");
  Task<Response> rejected6 = fetch(info("blob:http://a/x"));
  EXPECT_REJECTS_MESSAGE(std::move(rejected6), CodedTypeError, "protocol must be http:, https: or s3:");
  RequestInfo ftp(URL("ftp://x/"));
  Task<Response> rejected7 = fetch(std::move(ftp));
  EXPECT_REJECTS_MESSAGE(std::move(rejected7), CodedTypeError, "protocol must be http:, https: or s3:");
  RequestInit getWithBody;
  getWithBody.method = "GET";
  getWithBody.body = BodyInit(std::string("x"));
  Task<Response> rejected8 = fetch(info("http://a/"), std::move(getWithBody));
  EXPECT_REJECTS_MESSAGE(std::move(rejected8), CodedTypeError, "fetch() request with GET/HEAD method cannot have body.");
  RequestInit badRedirect;
  badRedirect.redirect = "bogus";
  Task<Response> rejected9 = fetch(info("http://a/"), std::move(badRedirect));
  EXPECT_REJECTS_MESSAGE(std::move(rejected9), CodedTypeError, "redirect must be one of 'follow', 'manual' or 'error'");
  // Validation comes before the abort check.
  RequestInit aborted;
  aborted.signal = AbortSignal::abort();
  Task<Response> rejected10 = fetch(info("http://[x"), std::move(aborted));
  EXPECT_REJECTS_MESSAGE(std::move(rejected10), CodedTypeError, "fetch() URL is invalid");
  std::string codes;
  try {
    Task<Response> ignored3 = fetch(info(""));
    (void)co_await std::move(ignored3);
  } catch (const CodedTypeError& error) {
    codes += error.code;
  }
  try {
    Task<Response> ignored4 = fetch(info("ws://x/"));
    (void)co_await std::move(ignored4);
  } catch (const CodedTypeError& error) {
    codes += " " + error.code;
  }
  EXPECT_EQ(codes, "ERR_INVALID_URL ERR_INVALID_ARG_VALUE");
  EXPECT(scope.transport->requests().empty());
}

ASYNC_TEST("default fetch builds the Request Bun sends") {
  TransportScope scope(echo);
  RequestInit init;
  init.method = "Post";
  Task<Response> firstCall = fetch(info("HTTP://A.test:80/./x/../y?q#f"), std::move(init));
  Response first = co_await std::move(firstCall);
  Task<std::string> firstTextReading = first.text();
  const std::string firstText = co_await std::move(firstTextReading);
  EXPECT_EQ(firstText, "GET http://a.test/y?q#f <null>");
  EXPECT_EQ(first.url(), "http://a.test/y?q#f");
  RequestInit post;
  post.method = "post";
  post.body = BodyInit(URLSearchParams("a=1"));
  Task<Response> secondCall = fetch(info("https://b.test/"), std::move(post));
  Response second = co_await std::move(secondCall);
  Task<std::string> secondTextReading = second.text();
  const std::string secondText = co_await std::move(secondTextReading);
  EXPECT_EQ(secondText, "POST https://b.test/ a=1");
  EXPECT_EQ(scope.transport->requests().at(1).headers().get("content-type"), std::optional<std::string>("application/x-www-form-urlencoded;charset=UTF-8"));
  Task<Response> dataCall = fetch(info("data:text/plain,x"));
  Response data = co_await std::move(dataCall);
  Task<std::string> dataTextReading = data.text();
  const std::string dataText = co_await std::move(dataTextReading);
  EXPECT_EQ(dataText, "GET data:text/plain,x <null>");
  EXPECT_EQ(scope.transport->requests().size(), std::size_t{3});
}

ASYNC_TEST("fetch(request) takes the request's body") {
  TransportScope scope(echo);
  RequestInit init;
  init.method = "POST";
  init.body = BodyInit(std::string("hi"));
  init.headers = Headers{{"x", "1"}};
  Request request("http://a.test/p", std::move(init));
  Task<Response> responseCall = fetch(RequestInfo(request));
  Response response = co_await std::move(responseCall);
  Task<std::string> responseTextReading = response.text();
  const std::string responseText = co_await std::move(responseTextReading);
  EXPECT_EQ(responseText, "POST http://a.test/p hi");
  EXPECT(request.bodyUsed());
  Task<Response> rejected11 = fetch(RequestInfo(request));
  EXPECT_REJECTS_MESSAGE(std::move(rejected11), CodedTypeError, "Request body already used");
  RequestInit replacement;
  replacement.body = BodyInit(std::string("new"));
  replacement.headers = Headers{{"x", "2"}};
  Task<Response> againCall = fetch(RequestInfo(request), std::move(replacement));
  Response again = co_await std::move(againCall);
  Task<std::string> againTextReading = again.text();
  const std::string againText = co_await std::move(againTextReading);
  EXPECT_EQ(againText, "POST http://a.test/p new");
  EXPECT_EQ(scope.transport->requests().back().headers().get("x"), std::optional<std::string>("2"));
}

ASYNC_TEST("an aborted signal rejects fetch with its reason") {
  TransportScope scope(echo);
  RequestInit preAborted;
  preAborted.signal = AbortSignal::abort();
  Task<Response> rejected12 = fetch(info("http://a/"), std::move(preAborted));
  EXPECT_REJECTS_MESSAGE(std::move(rejected12), DOMException, "The operation was aborted.");
  AbortController custom;
  custom.abort(makeReason(JsError("why")));
  RequestInit withReason;
  withReason.signal = custom.signal();
  Task<Response> rejected13 = fetch(info("http://a/"), std::move(withReason));
  EXPECT_REJECTS_REASON(std::move(rejected13), custom.signal().reason());
  AbortController viaRequest;
  RequestInit requestInit;
  requestInit.signal = viaRequest.signal();
  Request request("http://a/", std::move(requestInit));
  viaRequest.abort();
  Task<Response> rejected14 = fetch(RequestInfo(request));
  EXPECT_REJECTS_MESSAGE(std::move(rejected14), DOMException, "The operation was aborted.");
  // init.signal overrides the request's.
  RequestInit fresh;
  fresh.signal = AbortController().signal();
  Task<Response> responseCall = fetch(RequestInfo(request), std::move(fresh));
  Response response = co_await std::move(responseCall);
  Task<std::string> responseTextReading = response.text();
  const std::string responseText = co_await std::move(responseTextReading);
  EXPECT_EQ(responseText, "GET http://a/ <null>");
  EXPECT_EQ(scope.transport->requests().size(), std::size_t{1});
}

ASYNC_TEST("aborting while the transport waits rejects with the reason") {
  TransportScope scope([](Request) -> Task<Response> {
    co_await uos::rt::sleep(200);
    co_return Response(BodyInit(std::string("slow")));
  });
  AbortController controller;
  setTimeout([controller] { controller.abort(); }, 20);
  RequestInit init;
  init.signal = controller.signal();
  Task<Response> rejected15 = fetch(info("http://a/slow"), std::move(init));
  EXPECT_REJECTS_MESSAGE(std::move(rejected15), DOMException, "The operation was aborted.");
  RequestInit timed;
  timed.signal = AbortSignal::timeout(20);
  Task<Response> rejected16 = fetch(info("http://a/slow"), std::move(timed));
  EXPECT_REJECTS_MESSAGE(std::move(rejected16), DOMException, "The operation timed out.");
}

ASYNC_TEST("abort retains a replaced transport until its suspended send returns") {
  struct Transport final : FetchTransport {
    Resolvers<void> started = withResolvers();
    Resolvers<void> resume = withResolvers();
    std::string payload = "transport still alive";
    std::shared_ptr<std::string> observed = std::make_shared<std::string>();

    Task<Response> send(Request) override {
      started.resolve();
      co_await resume.promise.wait();
      *observed = payload;
      co_return Response();
    }
  };
  auto restore = ScopeExit([previous = fetchTransport()] { setFetchTransport(previous); });
  auto transport = std::make_shared<Transport>();
  const std::weak_ptr<Transport> weak = transport;
  auto started = transport->started.promise;
  auto resume = transport->resume.resolve;
  auto observed = transport->observed;
  setFetchTransport(std::move(transport));
  AbortController abort;
  RequestInit init;
  init.signal = abort.signal();
  Promise result(fetch(info("http://example.test/"), std::move(init)));
  co_await started.wait();
  setFetchTransport(nullptr);

  abort.abort();
  EXPECT_REJECTS_REASON(result.wait(), abort.signal().reason());
  EXPECT(!weak.expired());
  resume();
  co_await uos::rt::sleep(1);
  EXPECT_EQ(*observed, "transport still alive");
  EXPECT(weak.expired());
}

ASYNC_TEST("dropping an unread abortable response releases the wrapper and source") {
  std::weak_ptr<detail::StreamState<ByteChunk>> sourceState, wrapperState;
  TransportScope scope([&sourceState](Request) -> Task<Response> {
    ByteStream source({}, QueuingStrategy{0});
    sourceState = source.state();
    co_return Response(BodyInit(std::move(source)));
  });
  AbortController abort;
  {
    RequestInit init;
    init.signal = abort.signal();
    auto sending = fetch(info("http://example.test/"), std::move(init));
    auto response = co_await std::move(sending);
    ASSERT(response.body().has_value());
    wrapperState = response.body()->state();
    EXPECT(!sourceState.expired());
  }
  co_await uos::rt::sleep(1);
  EXPECT(wrapperState.expired());
  EXPECT(sourceState.expired());
}

ASYNC_TEST("aborting after the response errors its body with the reason") {
  TransportScope scope([](Request) -> Task<Response> { co_return Response(BodyInit(std::string("ok"))); });
  AbortController controller;
  RequestInit init;
  init.signal = controller.signal();
  Task<Response> responseCall = fetch(info("http://a/"), std::move(init));
  Response response = co_await std::move(responseCall);
  controller.abort();
  Task<std::string> rejected17 = response.text();
  EXPECT_REJECTS_MESSAGE(std::move(rejected17), DOMException, "The operation was aborted.");
  EXPECT(response.bodyUsed());
  AbortController custom;
  RequestInit customInit;
  customInit.signal = custom.signal();
  Task<Response> secondCall = fetch(info("http://a/"), std::move(customInit));
  Response second = co_await std::move(secondCall);
  custom.abort(makeReason(JsError("mine")));
  Task<std::string> rejected18 = second.text();
  EXPECT_REJECTS_MESSAGE(std::move(rejected18), JsError, "mine");
  AbortController late;
  RequestInit lateInit;
  lateInit.signal = late.signal();
  Task<Response> thirdCall = fetch(info("http://a/"), std::move(lateInit));
  Response third = co_await std::move(thirdCall);
  Task<std::string> thirdTextReading = third.text();
  const std::string thirdText = co_await std::move(thirdTextReading);
  EXPECT_EQ(thirdText, "ok");
  late.abort();
}

ASYNC_TEST("a fetch without a transport is a configuration error") {
  const std::shared_ptr<FetchTransport> previous = fetchTransport();
  setFetchTransport(nullptr);
  bool threw = false;
  try {
    Task<Response> ignored5 = fetch(info("http://a/"));
    (void)co_await std::move(ignored5);
  } catch (const std::logic_error&) {
    threw = true;
  }
  setFetchTransport(previous);
  EXPECT(threw);
}
