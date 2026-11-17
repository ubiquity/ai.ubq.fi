// Differential tests for src/provider/anthropic-chat-stream.ts (no bun test targets
// it directly; tests/anthropic_chat_responses_test.cpp covers it through its only
// caller). Expected frames and errors were recorded by running the TS module under
// Bun 1.4.0 on the same inputs.
// Links: src/provider/anthropic_chat_stream.cpp src/responses_stream.cpp src/utils.cpp src/inference_deadline.cpp
#include "uos/provider/anthropic_chat_stream.hpp"

#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::provider::anthropic_chat_stream;
using uos::responses_stream::responsesStreamErrorOf;

namespace {
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(
#include "anthropic_chat_stream_differential_vectors.inc"
    );
    return parsed;
  }

  json::value frameJson(const ChatFrame& frame) {
    json::object out;
    if (const auto* chunk = std::get_if<ChatFrameChunk>(&frame)) {
      out["kind"] = "chunk";
      out["value"] = chunk->value;
    } else if (const auto* comment = std::get_if<ChatFrameComment>(&frame)) {
      out["kind"] = "comment";
      out["text"] = comment->text;
    } else {
      out["kind"] = "done";
    }
    return out;
  }

  // describeError() of the Bun script.
  json::value errorJson(const rt::Reason& error) {
    json::object out;
    if (const auto* streamError = responsesStreamErrorOf(error)) {
      out["name"] = streamError->name;
      out["message"] = streamError->message;
      out["kind"] = streamError->kind;
      const rt::Reason& cause = streamError->causeValue;
      if (!cause) out["cause"] = nullptr;
      else if (rt::isError(cause)) out["cause"] = *rt::errorName(cause) + ": " + *rt::errorMessage(cause);
      else out["cause"] = rt::jsonStringify(rt::thrownValue(cause).value_or(json::value(nullptr)));
    } else if (rt::isError(error)) {
      out["name"] = *rt::errorName(error);
      out["message"] = *rt::errorMessage(error);
    } else {
      out["thrown"] = rt::reasonToString(error);
    }
    return out;
  }

  rt::Response bodyResponse(std::optional<std::string> body) { return body ? rt::Response(rt::BodyInit(*body)) : rt::Response(nullptr); }

  rt::Response chunkedResponse(std::vector<rt::ByteChunk> chunks) {
    auto remaining = std::make_shared<std::vector<rt::ByteChunk>>(std::move(chunks));
    auto index = std::make_shared<std::size_t>(0);
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [remaining, index](Controller controller) {
      if (*index < remaining->size()) controller.enqueue((*remaining)[(*index)++]);
      else controller.close();
    };
    return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))));
  }

  // `new ReadableStream({ start(controller) { enqueue every chunk; close } })`.
  rt::Response startedResponse(std::vector<std::string> parts) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [parts](Controller controller) {
      for (const std::string& part : parts) controller.enqueue(rt::encodeUtf8(part));
      controller.close();
    };
    return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))));
  }

  // run(body, chunks, abortReason, startChunks) of the Bun script.
  rt::Task<json::value> run(rt::Response response, rt::Reason abortReason) {
    const rt::AbortController controller;
    if (abortReason) controller.abort(abortReason);
    json::array frames;
    rt::Reason failure;
    const rt::AsyncGenerator<ChatFrame> iterator = readAnthropicChatStream(response, controller.signal());
    try {
      for (;;) {
        rt::IteratorResult<ChatFrame> step = co_await iterator.next();
        if (step.done) break;
        frames.push_back(frameJson(*step.value));
      }
    } catch (...) {
      failure = std::current_exception();
    }
    json::object out;
    out["frames"] = std::move(frames);
    if (failure) out["error"] = errorJson(failure);
    co_return json::value(std::move(out));
  }

  rt::Task<void> expectRun(rt::Response response, const char* key, rt::Reason abortReason = nullptr) {
    const json::value actual = co_await run(response, abortReason);
    const json::value& expected = *rt::get(vectors(), key);
    const bool equal = rt::deepEqual(actual, expected);
    if (!equal) std::fprintf(stderr, "    %s\n    actual:   %s\n    expected: %s\n", key, rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    EXPECT(equal);
  }

  std::vector<rt::ByteChunk> encoded(std::vector<std::string> parts) {
    std::vector<rt::ByteChunk> chunks;
    for (const std::string& part : parts) chunks.push_back(rt::encodeUtf8(part));
    return chunks;
  }
}

ASYNC_TEST("readAnthropicChatStream frames comments, chunks and [DONE] like Bun") {
  const rt::Response basic =
    bodyResponse(std::string(": hello\n:world\n\ndata: {\"choices\":[{\"delta\":{\"content\":\"hi\"}}]}\n\ndata: [DONE]\n\ndata: {\"after\":true}\n\n"));
  co_await expectRun(basic, "basic");
  const rt::Response crlf = bodyResponse(std::string("data: {\"choices\":[]}\r\n\r\ndata:  [DONE] \r\r"));
  co_await expectRun(crlf, "crlf");
  const rt::Response split = chunkedResponse(encoded({"da", "ta: {\"choi", "ces\":[{\"index\":0}]}\n", "\n: c\n\n", "data: [DO", "NE]\n\n"}));
  co_await expectRun(split, "split");
  const rt::Response trailingDone = bodyResponse(std::string("data: {\"choices\":[]}\n\ndata: [DONE]"));
  co_await expectRun(trailingDone, "trailingDone");
  const rt::Response multiData = bodyResponse(std::string("data: {\"choices\":\ndata: []}\nevent: x\nid: 1\n\ndata: [DONE]\n\n"));
  co_await expectRun(multiData, "multiData");
}

ASYNC_TEST("readAnthropicChatStream decodes UTF-8 across reads like Bun") {
  // The Bun script encoded strings holding lone surrogates (each became U+FFFD).
  const rt::Response loneSurrogates = chunkedResponse(
    encoded({"data: {\"choices\":[{\"delta\":{\"content\":\"\xC3\xA9\xED\xA0\xBC", "\xED\xBC\xA4\"}}]}\n\n", "data: [DONE]\n\n"}));
  co_await expectRun(loneSurrogates, "utf8");
  const rt::ByteChunk full = rt::encodeUtf8("data: {\"choices\":[{\"delta\":{\"content\":\"\xC3\xA9\xF0\x9F\x8C\xA4\"}}]}\n\ndata: [DONE]\n\n");
  std::size_t cut = 0;
  while (full[cut] != 0xF0) ++cut;
  cut += 2;
  const rt::Response utf8Split = chunkedResponse({rt::ByteChunk(full.begin(), full.begin() + static_cast<std::ptrdiff_t>(cut)),
                                                  rt::ByteChunk(full.begin() + static_cast<std::ptrdiff_t>(cut), full.end())});
  co_await expectRun(utf8Split, "utf8Split");
}

ASYNC_TEST("readAnthropicChatStream fails like Bun on malformed, erroring and truncated streams") {
  const rt::Response malformed = bodyResponse(std::string("data: {nope\n\n"));
  co_await expectRun(malformed, "malformed");
  const rt::Response nonObject = bodyResponse(std::string("data: [1,2]\n\n"));
  co_await expectRun(nonObject, "nonObject");
  const rt::Response errorField = bodyResponse(std::string("data: {\"error\":{\"message\":\"boom\"},\"choices\":[]}\n\n"));
  co_await expectRun(errorField, "errorField");
  const rt::Response errorNull = bodyResponse(std::string("data: {\"error\":null,\"choices\":[]}\n\n"));
  co_await expectRun(errorNull, "errorNull");
  const rt::Response noChoices = bodyResponse(std::string("data: {\"id\":\"x\"}\n\n"));
  co_await expectRun(noChoices, "noChoices");
  const rt::Response premature = bodyResponse(std::string("data: {\"choices\":[]}\n\n"));
  co_await expectRun(premature, "premature");
  const rt::Response prematureTrailing = bodyResponse(std::string("data: {\"choices\":[]}"));
  co_await expectRun(prematureTrailing, "prematureTrailing");
  const rt::Response emptyBody = bodyResponse(std::string());
  co_await expectRun(emptyBody, "emptyBody");
  const rt::Response noBody = bodyResponse(std::nullopt);
  co_await expectRun(noBody, "noBody");
}

// An abort that happened before the first read races a chunk that is already
// queued (the read wins, as JS subscribes to it first) or one a pull still has to
// produce (the abort wins). A buffered `new Response(text)` body is not used here:
// Bun hands its first chunk to a read at once, rt one turn later (see the report).
ASYNC_TEST("readAnthropicChatStream lets a ready read win over an earlier abort like Bun") {
  const rt::Reason callerGone = rt::makeReason(rt::JsError("caller gone"));
  const rt::Reason plainReason = rt::thrown(json::value("plain reason"));
  const rt::Response readyDone = startedResponse({"data: [DONE]\n\n"});
  co_await expectRun(readyDone, "abortedError", callerGone);
  const rt::Response readyChunks = startedResponse({"data: {\"choices\":[]}\n\n", "data: [DONE]\n\n"});
  co_await expectRun(readyChunks, "abortedString", plainReason);
  const rt::Response pulled = chunkedResponse(encoded({"data: [DONE]\n\n"}));
  co_await expectRun(pulled, "abortedPull", plainReason);
  const rt::Response pulledError = chunkedResponse(encoded({"data: [DONE]\n\n"}));
  co_await expectRun(pulledError, "abortedPullError", callerGone);
}
