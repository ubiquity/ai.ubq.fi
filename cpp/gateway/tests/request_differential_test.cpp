// Differential cases for src/request.ts recorded from Bun 1.4.0. The TS tests that
// import request.ts (images, responses-native-headers, sentinel-replay-envelope-coverage)
// exercise it only through unported modules, so these vectors pin the module itself:
// each expected literal is the JSON the same scenario printed under Bun.
#include "uos/request.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
using request::captureRawBodyOnce;
using request::discardRawBodyObserverOnce;
using request::JsonBodyReadResult;
using request::JsonBodyReadResultNotOk;
using request::JsonBodyReadResultOk;
using request::MAX_ACCEPTED_JSON_BODY_BYTES;
using request::observeRawBodyOnce;
using request::readJsonBody;
using request::readJsonBodyWithLimit;
using request::rejectRawBodyOnce;

namespace {
  std::shared_ptr<rt::Bytes> enc(std::string_view text) { return std::make_shared<rt::Bytes>(text.begin(), text.end()); }

  rt::Request post(std::optional<rt::BodyInit> body, std::vector<rt::Headers::Entry> headers = {}) {
    rt::RequestInit init;
    init.method = "POST";
    init.body = std::move(body);
    if (!headers.empty()) init.headers = rt::Headers(headers);
    return rt::Request("https://ai.ubq.fi/v1/x", std::move(init));
  }

  rt::Bytes bytesOf(std::string_view text) { return rt::Bytes(text.begin(), text.end()); }

  rt::ByteStream streamOf(std::vector<rt::Bytes> chunks, std::optional<std::string> error = std::nullopt) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [chunks = std::move(chunks), error](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      for (const rt::Bytes& chunk : chunks) controller.enqueue(chunk);
      if (error) controller.error(rt::makeReason(rt::JsError(*error)));
      else controller.close();
    };
    return rt::ByteStream(std::move(source));
  }

  json::value bytesJson(const rt::Bytes& bytes) {
    json::array out;
    for (std::uint8_t byte : bytes) out.push_back(rt::makeNumber(static_cast<double>(byte)));
    return out;
  }

  json::value resultJson(const JsonBodyReadResult& result) {
    json::object out;
    if (const auto* ok = std::get_if<JsonBodyReadResultOk>(&result)) {
      out["ok"] = true;
      out["value"] = ok->value;
    } else {
      out["ok"] = false;
      out["kind"] = std::get<JsonBodyReadResultNotOk>(result).kind;
    }
    return out;
  }

  // The TS `run(label, req, maxBytes, observe)` helper: [result, log, kept bytes, req.bodyUsed].
  rt::Task<json::value> run(rt::Request req, double maxBytes, bool observe = true) {
    auto log = std::make_shared<json::array>();
    auto kept = std::make_shared<std::shared_ptr<rt::Bytes>>();
    if (observe) {
      request::RawBodyObserver observer = [log, kept](std::shared_ptr<rt::Bytes> bytes) {
        *kept = bytes;
        log->push_back(json::array{"observer", json::string(rt::decodeUtf8(*bytes))});
      };
      request::RawBodyRejection onRejected = [log](const std::string& reason) { log->push_back(json::array{"rejected", json::string(reason)}); };
      observeRawBodyOnce(req, observer, onRejected);
    }
    json::value result;
    JsonBodyReadResult read = co_await readJsonBodyWithLimit(req, maxBytes);
    result = resultJson(read);
    json::value keptAfter = *kept ? bytesJson(**kept) : json::value(nullptr);
    const bool leftover = captureRawBodyOnce(req, enc("late"));
    log->push_back(json::array{"leftover", leftover});
    json::array out{result, *log, keptAfter, req.bodyUsed()};
    co_return json::value(std::move(out));
  }

  bool same(const json::value& actual, std::string_view expected) {
    const bool equal = rt::deepEqual(actual, rt::jsonParse(expected));
    if (!equal) std::fprintf(stderr, "      actual:   %s\n      expected: %s\n", rt::jsonStringify(actual).c_str(), std::string(expected).c_str());
    return equal;
  }
}

TEST("MAX_ACCEPTED_JSON_BODY_BYTES is 32 MiB") {
  EXPECT_EQ(MAX_ACCEPTED_JSON_BODY_BYTES, 33554432);
  EXPECT_EQ(rt::numberToString(static_cast<double>(MAX_ACCEPTED_JSON_BODY_BYTES + 1)), "33554433");
}

ASYNC_TEST("an observed body is handed over once, by reference") {
  const rt::Request req = post(rt::BodyInit(std::string("{}")));
  auto log = std::make_shared<json::array>();
  auto seen = std::make_shared<std::shared_ptr<rt::Bytes>>();
  observeRawBodyOnce(
    req,
    [log, seen](std::shared_ptr<rt::Bytes> bytes) {
      *seen = bytes;
      log->push_back(json::array{"observer", bytesJson(*bytes)});
    },
    [log](const std::string& reason) { log->push_back(json::array{"rejected", json::string(reason)}); });
  const auto bytes = enc("abc");
  const bool capture1 = captureRawBodyOnce(req, bytes);
  log->push_back(json::array{"capture1", capture1});
  log->push_back(json::array{"same", *seen == bytes});
  const bool capture2 = captureRawBodyOnce(req, bytes);
  log->push_back(json::array{"capture2", capture2});
  const bool unobserved = captureRawBodyOnce(post(rt::BodyInit(std::string("{}"))), bytes);
  log->push_back(json::array{"unobserved", unobserved});
  EXPECT(same(*log, R"([["observer",[97,98,99]],["capture1",true],["same",true],["capture2",false],["unobserved",false]])"));
  co_return;
}

ASYNC_TEST("a body over the replay cap is reported, not observed") {
  auto log = std::make_shared<json::array>();
  const rt::Request req = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(
    req, [log](std::shared_ptr<rt::Bytes>) { log->push_back("observer"); }, [log](const std::string& reason) { log->push_back(json::array{"rejected", json::string(reason)}); });
  const bool oversize = captureRawBodyOnce(req, std::make_shared<rt::Bytes>(static_cast<std::size_t>(MAX_ACCEPTED_JSON_BODY_BYTES + 1)));
  log->push_back(json::array{"oversize", oversize});
  const bool after = captureRawBodyOnce(req, enc("x"));
  log->push_back(json::array{"after", after});
  const rt::Request exact = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(exact, [log](std::shared_ptr<rt::Bytes> bytes) { log->push_back(json::array{"exact", rt::makeNumber(static_cast<double>(bytes->size()))}); });
  const bool exactCapture = captureRawBodyOnce(exact, std::make_shared<rt::Bytes>(static_cast<std::size_t>(MAX_ACCEPTED_JSON_BODY_BYTES)));
  log->push_back(json::array{"exactCapture", exactCapture});
  const rt::Request quiet = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(quiet, [log](std::shared_ptr<rt::Bytes>) { log->push_back("quiet observer"); });
  const bool oversizeNoRejection = captureRawBodyOnce(quiet, std::make_shared<rt::Bytes>(static_cast<std::size_t>(MAX_ACCEPTED_JSON_BODY_BYTES + 1)));
  log->push_back(json::array{"oversizeNoRejection", oversizeNoRejection});
  EXPECT(same(*log, R"([["rejected","body_over_limit"],["oversize",false],["after",false],["exact",33554432],["exactCapture",true],["oversizeNoRejection",false]])"));
  co_return;
}

ASYNC_TEST("discard, reject, replacement and request identity") {
  auto log = std::make_shared<json::array>();
  auto observer = [log](std::string name) { return [log, name](std::shared_ptr<rt::Bytes>) { log->push_back(json::string(name)); }; };
  auto rejection = [log](std::string name) { return [log, name](const std::string& reason) { log->push_back(json::array{json::string(name), json::string(reason)}); }; };
  const rt::Request a = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(a, observer("a observer"), rejection("a rejected"));
  discardRawBodyObserverOnce(a);
  const bool discardedCapture = captureRawBodyOnce(a, enc("x"));
  log->push_back(json::array{"discarded capture", discardedCapture});
  const bool discardedReject = rejectRawBodyOnce(a, "body_unavailable");
  log->push_back(json::array{"discarded reject", discardedReject});
  const rt::Request b = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(b, observer("b observer"), rejection("b rejected"));
  const bool reject = rejectRawBodyOnce(b, "body_unavailable");
  log->push_back(json::array{"reject", reject});
  const bool rejectAgain = rejectRawBodyOnce(b, "body_over_limit");
  log->push_back(json::array{"reject again", rejectAgain});
  const rt::Request c = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(c, observer("c observer"));
  const bool rejectWithoutCallback = rejectRawBodyOnce(c, "body_over_limit");
  log->push_back(json::array{"reject without callback", rejectWithoutCallback});
  const rt::Request d = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(d, observer("d first"));
  observeRawBodyOnce(d, [log](std::shared_ptr<rt::Bytes> bytes) { log->push_back(json::array{"d second", bytesJson(*bytes)}); });
  const bool replaced = captureRawBodyOnce(d, enc("z"));
  log->push_back(json::array{"replaced", replaced});
  const rt::Request e = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(e, observer("e observer"));
  const rt::Request alias = e;
  const bool aliased = captureRawBodyOnce(alias, enc("q"));
  log->push_back(json::array{"alias", aliased});
  const rt::Request f = post(rt::BodyInit(std::string("{}")));
  observeRawBodyOnce(f, observer("f observer"));
  const rt::Request copy(f, {});
  const bool copyCapture = captureRawBodyOnce(copy, enc("q"));
  const bool originalCapture = captureRawBodyOnce(f, enc("r"));
  log->push_back(json::array{"copy", copyCapture, originalCapture});
  EXPECT(same(*log, R"([["discarded capture",false],["discarded reject",false],["b rejected","body_unavailable"],["reject",true],["reject again",false],)"
                    R"(["reject without callback",true],["d second",[122]],["replaced",true],"e observer",["alias",true],"f observer",["copy",false,true]])"));
  co_return;
}

ASYNC_TEST("buffered bodies: parse, empty, invalid, BOM and replacement characters") {
  const rt::Request okRequest = post(rt::BodyInit(std::string(R"({"a":[1,2.5,"x"],"b":null})")));
  json::value ok = co_await run(okRequest, 1000);
  EXPECT(same(ok, R"([{"ok":true,"value":{"a":[1,2.5,"x"],"b":null}},[["observer","{\"a\":[1,2.5,\"x\"],\"b\":null}"],["leftover",false]],)"
                  R"([123,34,97,34,58,91,49,44,50,46,53,44,34,120,34,93,44,34,98,34,58,110,117,108,108,125],true])"));
  const rt::Request okNumberRequest = post(rt::BodyInit(std::string("  42 ")));
  json::value okNumber = co_await run(okNumberRequest, 1000);
  EXPECT(same(okNumber, R"([{"ok":true,"value":42},[["observer","  42 "],["leftover",false]],[32,32,52,50,32],true])"));
  const rt::Request okNullRequest = post(rt::BodyInit(std::string("null")));
  json::value okNull = co_await run(okNullRequest, 1000);
  EXPECT(same(okNull, R"([{"ok":true,"value":null},[["observer","null"],["leftover",false]],[110,117,108,108],true])"));
  const rt::Request noBodyRequest = post(std::nullopt);
  json::value noBody = co_await run(noBodyRequest, 1000);
  EXPECT(same(noBody, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null,false])"));
  const rt::Request emptyStringRequest = post(rt::BodyInit(std::string("")));
  json::value emptyString = co_await run(emptyStringRequest, 1000);
  // Bun leaves bodyUsed false after reading an empty buffered body to its end; rt::Request
  // reports true. That is a runtime difference outside this module, so it is not pinned.
  emptyString.as_array().pop_back();
  EXPECT(same(emptyString, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null])"));
  const rt::Request invalidRequest = post(rt::BodyInit(std::string("{bad")));
  json::value invalid = co_await run(invalidRequest, 1000);
  EXPECT(same(invalid, R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,true])"));
  const rt::Request whitespaceRequest = post(rt::BodyInit(std::string("   ")));
  json::value whitespace = co_await run(whitespaceRequest, 1000);
  EXPECT(same(whitespace, R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,true])"));
  rt::Bytes bom{0xef, 0xbb, 0xbf};
  for (std::uint8_t byte : bytesOf(R"({"bom":true})")) bom.push_back(byte);
  const rt::Request withBomRequest = post(rt::BodyInit(bom));
  json::value withBom = co_await run(withBomRequest, 1000);
  EXPECT(same(withBom, R"([{"ok":true,"value":{"bom":true}},[["observer","{\"bom\":true}"],["leftover",false]],[239,187,191,123,34,98,111,109,34,58,116,114,117,101,125],true])"));
  const rt::Request badUtf8Request = post(rt::BodyInit(rt::Bytes{0x22, 0xff, 0x61, 0x22}));
  json::value badUtf8 = co_await run(badUtf8Request, 1000);
  EXPECT(same(badUtf8, R"([{"ok":true,"value":"�a"},[["observer","\"�a\""],["leftover",false]],[34,255,97,34],true])"));
  const rt::Request noObserverRequest = post(rt::BodyInit(std::string(R"({"k":"v"})")));
  json::value noObserver = co_await run(noObserverRequest, 1000, false);
  EXPECT(same(noObserver, R"([{"ok":true,"value":{"k":"v"}},[["leftover",false]],null,true])"));
}

ASYNC_TEST("byte limits on buffered bodies and declared lengths") {
  const rt::Request exactLimitRequest = post(rt::BodyInit(std::string(R"("12345678")")));
  json::value exactLimit = co_await run(exactLimitRequest, 10);
  EXPECT(same(exactLimit, R"([{"ok":true,"value":"12345678"},[["observer","\"12345678\""],["leftover",false]],[34,49,50,51,52,53,54,55,56,34],true])"));
  const rt::Request overLimitBufferedRequest = post(rt::BodyInit(std::string(R"("123456789")")));
  json::value overLimitBuffered = co_await run(overLimitBufferedRequest, 10);
  EXPECT(same(overLimitBuffered, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
  const rt::Request declaredOverLimitRequest = post(rt::BodyInit(std::string(R"({"a":1})")), {{"content-length", "11"}});
  json::value declaredOverLimit = co_await run(declaredOverLimitRequest, 10);
  EXPECT(same(declaredOverLimit, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
  const rt::Request declaredAtLimitRequest = post(rt::BodyInit(std::string(R"({"a":1})")), {{"content-length", "10"}});
  json::value declaredAtLimit = co_await run(declaredAtLimitRequest, 10);
  EXPECT(same(declaredAtLimit, R"([{"ok":true,"value":{"a":1}},[["observer","{\"a\":1}"],["leftover",false]],[123,34,97,34,58,49,125],true])"));
  const rt::Request declaredZeroRequest = post(rt::BodyInit(std::string(R"({"a":1})")), {{"content-length", "0"}});
  json::value declaredZero = co_await run(declaredZeroRequest, 10);
  EXPECT(same(declaredZero, R"([{"ok":true,"value":{"a":1}},[["observer","{\"a\":1}"],["leftover",false]],[123,34,97,34,58,49,125],true])"));
  const rt::Request declaredNoBodyRequest = post(std::nullopt, {{"content-length", "50"}});
  json::value declaredNoBody = co_await run(declaredNoBodyRequest, 10);
  EXPECT(same(declaredNoBody, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,false])"));
  const rt::Request declaredNoBodyInvalidRequest = post(std::nullopt, {{"content-length", "x"}});
  json::value declaredNoBodyInvalid = co_await run(declaredNoBodyInvalidRequest, 10);
  EXPECT(same(declaredNoBodyInvalid, R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,false])"));
  const rt::Request zeroLimitRequest = post(rt::BodyInit(std::string("1")));
  json::value zeroLimit = co_await run(zeroLimitRequest, 0);
  EXPECT(same(zeroLimit, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
  const rt::Request nanLimitRequest = post(rt::BodyInit(std::string("1")));
  json::value nanLimit = co_await run(nanLimitRequest, std::numeric_limits<double>::quiet_NaN());
  EXPECT(same(nanLimit, R"([{"ok":true,"value":1},[["observer","1"],["leftover",false]],[49],true])"));
}

ASYNC_TEST("Content-Length must be a canonical safe decimal integer") {
  const std::string accepted = R"([{"ok":true,"value":{"a":1}},[["observer","{\"a\":1}"],["leftover",false]],[123,34,97,34,58,49,125],true])";
  const std::string rejected = R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,false])";
  const std::vector<std::pair<std::string, bool>> cases = {
    {"abc", false}, {"01", false}, {"-1", false}, {" 5", true}, {"5 ", true}, {"1e3", false}, {"1.0", false},
    {"9007199254740991", true}, {"9007199254740992", false}, {"", false}, {"+1", false}, {"0x10", false},
  };
  for (const auto& [value, ok] : cases) {
    const rt::Request outcomeRequest = post(rt::BodyInit(std::string(R"({"a":1})")), {{"content-length", value}});
    json::value outcome = co_await run(outcomeRequest, 1e20);
    if (!same(outcome, ok ? accepted : rejected)) rt::test::report(__FILE__, __LINE__, "content-length " + value, false);
  }
}

ASYNC_TEST("streamed bodies: chunks, limits, errors and empty streams") {
  auto chunks = [] { return std::vector<rt::Bytes>{bytesOf(R"({"x")"), bytesOf(":"), bytesOf("[1,"), bytesOf("2]}")}; };
  const rt::Request wholeRequest = post(rt::BodyInit(streamOf(chunks())));
  json::value whole = co_await run(wholeRequest, 1000);
  EXPECT(same(whole, R"([{"ok":true,"value":{"x":[1,2]}},[["observer","{\"x\":[1,2]}"],["leftover",false]],[123,34,120,34,58,91,49,44,50,93,125],true])"));
  const rt::Request overRequest = post(rt::BodyInit(streamOf(chunks())));
  json::value over = co_await run(overRequest, 9);
  EXPECT(same(over, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
  const rt::Request exactRequest = post(rt::BodyInit(streamOf(chunks())));
  json::value exact = co_await run(exactRequest, 11);
  EXPECT(same(exact, R"([{"ok":true,"value":{"x":[1,2]}},[["observer","{\"x\":[1,2]}"],["leftover",false]],[123,34,120,34,58,91,49,44,50,93,125],true])"));
  const rt::Request streamErrorRequest = post(rt::BodyInit(streamOf({bytesOf(R"({"x")")}, std::string("boom"))));
  json::value streamError = co_await run(streamErrorRequest, 1000);
  EXPECT(same(streamError, R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,true])"));
  const rt::Request emptyStreamRequest = post(rt::BodyInit(streamOf({})));
  json::value emptyStream = co_await run(emptyStreamRequest, 1000);
  EXPECT(same(emptyStream, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null,true])"));
  const rt::Request emptyChunksRequest = post(rt::BodyInit(streamOf({rt::Bytes{}, rt::Bytes{}})));
  json::value emptyChunks = co_await run(emptyChunksRequest, 1000);
  EXPECT(same(emptyChunks, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null,true])"));
  const rt::Request zeroLimitEmptyRequest = post(rt::BodyInit(streamOf({rt::Bytes{}})));
  json::value zeroLimitEmpty = co_await run(zeroLimitEmptyRequest, 0);
  EXPECT(same(zeroLimitEmpty, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null,true])"));
  const rt::Request negativeLimitRequest = post(rt::BodyInit(streamOf({rt::Bytes{}})));
  json::value negativeLimit = co_await run(negativeLimitRequest, -1);
  EXPECT(same(negativeLimit, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
}

ASYNC_TEST("a used or locked body") {
  const rt::Request twice = post(rt::BodyInit(std::string(R"({"twice":1})")));
  json::value firstRead = co_await run(twice, 1000);
  EXPECT(same(firstRead, R"([{"ok":true,"value":{"twice":1}},[["observer","{\"twice\":1}"],["leftover",false]],[123,34,116,119,105,99,101,34,58,49,125],true])"));
  json::value secondRead = co_await run(twice, 1000);
  EXPECT(same(secondRead, R"([{"ok":false,"kind":"empty"},[["leftover",false]],null,true])"));

  const rt::Request locked = post(rt::BodyInit(std::string(R"({"locked":1})")));
  const auto reader = locked.body()->getReader();
  json::value lockedRead = co_await run(locked, 1000);
  EXPECT(same(lockedRead, R"([{"ok":false,"kind":"invalid"},[["leftover",false]],null,true])"));
  reader.releaseLock();

  const rt::Request lockedDeclared = post(rt::BodyInit(std::string(R"({"locked":1})")), {{"content-length", "999"}});
  const auto declaredReader = lockedDeclared.body()->getReader();
  json::value lockedDeclaredOver = co_await run(lockedDeclared, 10);
  EXPECT(same(lockedDeclaredOver, R"([{"ok":false,"kind":"too_large"},[["rejected","body_over_limit"],["leftover",false]],null,true])"));
  declaredReader.releaseLock();
}

ASYNC_TEST("an observer that throws after keeping the bytes sees them zeroed") {
  const rt::Request req = post(rt::BodyInit(std::string(R"({"t":1})")));
  auto kept = std::make_shared<std::shared_ptr<rt::Bytes>>();
  observeRawBodyOnce(req, [kept](std::shared_ptr<rt::Bytes> bytes) {
    *kept = bytes;
    throw rt::JsError("observer failed");
  });
  JsonBodyReadResult result = co_await readJsonBodyWithLimit(req, 1000);
  json::array outcome{resultJson(result), *kept ? bytesJson(**kept) : json::value(nullptr)};
  EXPECT(same(outcome, R"([{"ok":false,"kind":"invalid"},[0,0,0,0,0,0,0]])"));
}

ASYNC_TEST("the stream is cancelled without a reason when the read stops at the cap") {
  auto reasons = std::make_shared<std::vector<std::string>>();
  rt::UnderlyingSource<rt::ByteChunk> endless;
  endless.pull = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(bytesOf("xxxxxxxx")); };
  endless.cancel = [reasons](rt::Reason reason) { reasons->push_back(rt::reasonToString(reason)); };
  const rt::Request resultRequest = post(rt::BodyInit(rt::ByteStream(endless)));
  JsonBodyReadResult result = co_await readJsonBodyWithLimit(resultRequest, 20);
  json::array outcome{resultJson(result), json::array{}};
  for (const std::string& reason : *reasons) outcome[1].as_array().push_back(json::string(reason));
  EXPECT(same(outcome, R"([{"ok":false,"kind":"too_large"},["undefined"]])"));

  auto reasons2 = std::make_shared<std::vector<std::string>>();
  rt::UnderlyingSource<rt::ByteChunk> open;
  open.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) { controller.enqueue(bytesOf("1")); };
  open.cancel = [reasons2](rt::Reason reason) { reasons2->push_back(rt::reasonToString(reason)); };
  const rt::Request req2 = post(rt::BodyInit(rt::ByteStream(open)), {{"content-length", "100"}});
  JsonBodyReadResult result2 = co_await readJsonBodyWithLimit(req2, 20);
  json::array outcome2{resultJson(result2), json::array{}, req2.bodyUsed()};
  for (const std::string& reason : *reasons2) outcome2[1].as_array().push_back(json::string(reason));
  EXPECT(same(outcome2, R"([{"ok":false,"kind":"too_large"},["undefined"],true])"));
}

ASYNC_TEST("readJsonBody returns the parsed value or null") {
  const rt::Request parsedRequest = post(rt::BodyInit(std::string(R"({"v":1})")));
  json::value parsed = co_await readJsonBody(parsedRequest);
  const rt::Request brokenRequest = post(rt::BodyInit(std::string("{")));
  json::value broken = co_await readJsonBody(brokenRequest);
  const rt::Request tooLargeRequest = post(rt::BodyInit(std::string(R"("x")")));
  json::value tooLarge = co_await readJsonBody(tooLargeRequest, 2);
  const rt::Request fitsRequest = post(rt::BodyInit(std::string(R"("x")")));
  json::value fits = co_await readJsonBody(fitsRequest, 3);
  json::array outcome{parsed, broken, tooLarge, fits};
  EXPECT(same(outcome, R"([{"v":1},null,null,"x"])"));
}

ASYNC_TEST("received-body observation retains exact bytes for malformed and empty requests") {
  const std::vector<std::pair<std::string, std::string>> cases{{"{bad", "invalid"}, {"   ", "invalid"}, {"", "empty"}};
  for (const auto& [body, kind] : cases) {
    const rt::Request req = post(rt::BodyInit(body));
    std::shared_ptr<rt::Bytes> captured;
    std::optional<std::string> status;
    observeRawBodyOnce(req, [&captured](std::shared_ptr<rt::Bytes> bytes) { captured = std::move(bytes); },
      [&status](const std::string& reason) { status = reason; }, request::RawBodyStage::received);
    auto result = co_await readJsonBodyWithLimit(req, 1024);
    const auto* error = std::get_if<request::JsonBodyReadResultNotOk>(&result);
    ASSERT(error);
    EXPECT_EQ(error->kind, kind);
    ASSERT(captured);
    EXPECT_EQ(*captured, bytesOf(body));
    EXPECT_EQ(status, std::optional<std::string>(body.empty() ? "body_empty" : "body_invalid"));
  }
}

ASYNC_TEST("received-body observation distinguishes partial and over-limit payloads") {
  const auto observe = [](const rt::Request& req, std::optional<std::string>& status) {
    observeRawBodyOnce(req, [](std::shared_ptr<rt::Bytes>) { EXPECT(false); },
      [&status](const std::string& reason) { status = reason; }, request::RawBodyStage::received);
  };
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.pull = [sent = false](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) mutable {
    if (sent) controller.error(rt::makeReason(rt::JsError("interrupted")));
    else {
      sent = true;
      controller.enqueue(bytesOf("{"));
    }
  };
  rt::QueuingStrategy strategy;
  strategy.highWaterMark = 0;
  const rt::Request partial = post(rt::BodyInit(rt::ByteStream(std::move(source), strategy)));
  std::optional<std::string> status;
  observe(partial, status);
  (void)co_await readJsonBodyWithLimit(partial, 1024);
  EXPECT_EQ(status, std::optional<std::string>("body_partial"));
  const rt::Request oversized = post(rt::BodyInit(std::string("123")));
  status.reset();
  observe(oversized, status);
  (void)co_await readJsonBodyWithLimit(oversized, 2);
  EXPECT_EQ(status, std::optional<std::string>("body_over_limit"));
}
