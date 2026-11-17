// Differential cases for src/sentinel/upstream-capture.ts beyond its bun tests. Every
// expected value was recorded by running the TypeScript module under Bun 1.4.0
// (parse results and messages, the sealed trace's JSON.stringify and canonical
// forms with a deterministic Date.now, content-type and safe-header normalization).
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::sentinel::upstream_capture;

  struct ParseVector {
    const char* name;
    const char* input;     // JSON.stringify(value)
    bool ok;
    const char* expected;  // String(error), or canonicalSentinelUpstreamJson(parsed)
    const char* json;      // JSON.stringify(parsed)
  };

  const ParseVector PARSE_VECTORS[] = {
    {"null", R"json(null)json", false, "Error: Sentinel upstream trace is not an object", nullptr},
    {"array", R"json([])json", false, "Error: Sentinel upstream trace is not an object", nullptr},
    {"string", R"json("x")json", false, "Error: Sentinel upstream trace is not an object", nullptr},
    {"missing key", R"json({"version":1,"attempts":[],"attempts_truncated":false,"bytes_truncated":false})json", false, "Error: Sentinel upstream trace keys are invalid", nullptr},
    {"extra key", R"json({"version":1,"attempts":[],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"extra":1})json", false, "Error: Sentinel upstream trace keys are invalid", nullptr},
    {"version 3", R"json({"version":3,"attempts":[],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace version is invalid", nullptr},
    {"version string", R"json({"version":"1","attempts":[],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace version is invalid", nullptr},
    {"version 1.5", R"json({"version":1.5,"attempts":[],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace version is invalid", nullptr},
    {"flags", R"json({"version":1,"attempts":[],"attempts_truncated":false,"bytes_truncated":0,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace truncation flags are invalid", nullptr},
    {"attempts object", R"json({"version":1,"attempts":{},"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace attempts are invalid", nullptr},
    {"too many attempts", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream trace has too many attempts", nullptr},
    {"attempt null", R"json({"version":1,"attempts":[null],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt keys are invalid (attempt 0)", nullptr},
    {"attempt array", R"json({"version":1,"attempts":[[]],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt keys are invalid (attempt 0)", nullptr},
    {"attempt extra", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof","extra":1}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt keys are invalid (attempt 0)", nullptr},
    {"attempt index 1 bad provider", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"},{"provider":"claude_code","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt provider is invalid (attempt 1)", nullptr},
    {"provider number", R"json({"version":1,"attempts":[{"provider":1,"status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt provider is invalid (attempt 0)", nullptr},
    {"status 1.5", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":1.5,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt status is invalid (attempt 0)", nullptr},
    {"status string", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":"200","content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt status is invalid (attempt 0)", nullptr},
    {"status 100", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":100,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":[],"content_type":"application/json","provider":"chatgpt_codex","status":100,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":100,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"status 599", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":599,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":[],"content_type":"application/json","provider":"chatgpt_codex","status":599,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":599,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"status 9007199254740992", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":9007199254740992,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt status is invalid (attempt 0)", nullptr},
    {"content type other", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"other","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":[],"content_type":"other","provider":"chatgpt_codex","status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"other","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"content type bad", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"text/html","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt content type is invalid (attempt 0)", nullptr},
    {"terminal bad", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"done"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt terminal is invalid (attempt 0)", nullptr},
    {"chunks not array", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":"YQ==","terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunks are invalid (attempt 0)", nullptr},
    {"chunk number", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[1],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream chunk base64 is invalid (attempt 0)", nullptr},
    {"chunk empty", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[""],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream chunk base64 is invalid (attempt 0)", nullptr},
    {"chunk noncanonical", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YR=="],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream chunk base64 is invalid (attempt 0)", nullptr},
    {"chunk whitespace", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ== "],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream chunk base64 is invalid (attempt 0)", nullptr},
    {"chunk url alphabet", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["-_8="],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream chunk base64 is invalid (attempt 0)", nullptr},
    {"chunk ok", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["+/8=","AAAA","YWI="],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":["+/8=","AAAA","YWI="],"content_type":"application/json","provider":"chatgpt_codex","status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["+/8=","AAAA","YWI="],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"pending pre-header ok", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":[],"content_type":null,"provider":"chatgpt_codex","status":null,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"pre-header ct", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":"application/json","chunks_base64":[],"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream pre-header attempt cannot carry a content type (attempt 0)", nullptr},
    {"header-bearing no ct", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":null,"chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream header-bearing attempt requires a content type (attempt 0)", nullptr},
    {"pre-header chunks", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":["YQ=="],"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream pre-header attempt cannot have chunks (attempt 0)", nullptr},
    {"fetch_error ok", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"fetch_error"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunks_base64":[],"content_type":null,"provider":"chatgpt_codex","status":null,"terminal":"fetch_error"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":1})json", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"fetch_error"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"fetch_error status", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"fetch_error"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream fetch_error attempt is inconsistent (attempt 0)", nullptr},
    {"eof null status", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream header-bearing terminal requires a status (attempt 0)", nullptr},
    {"v2 ok", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunk_times_ms":[0],"chunks_base64":["YQ=="],"content_type":"application/json","ended_at_ms":1700000000002,"headers":{"content-type":"application/json"},"headers_at_ms":1700000000001,"headers_truncated":false,"provider":"chatgpt_codex","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"v2 headers null", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":null,"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt headers are invalid (attempt 0)", nullptr},
    {"v2 headers array", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":[],"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt headers are invalid (attempt 0)", nullptr},
    {"v2 header not allowlisted", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"authorization":"x"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header name is not allowlisted (attempt 0)", nullptr},
    {"v2 header index key first", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"1":"x","content-type":5},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header name is not allowlisted (attempt 0)", nullptr},
    {"v2 header empty", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":""},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header value is invalid (attempt 0)", nullptr},
    {"v2 header 512", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunk_times_ms":[0],"chunks_base64":["YQ=="],"content_type":"application/json","ended_at_ms":1700000000002,"headers":{"content-type":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"headers_at_ms":1700000000001,"headers_truncated":false,"provider":"chatgpt_codex","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"v2 header 513", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header value is invalid (attempt 0)", nullptr},
    {"v2 header utf16 512", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"éééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééé"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunk_times_ms":[0],"chunks_base64":["YQ=="],"content_type":"application/json","ended_at_ms":1700000000002,"headers":{"content-type":"éééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééé"},"headers_at_ms":1700000000001,"headers_truncated":false,"provider":"chatgpt_codex","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"éééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééééé"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"v2 header astral 257", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀😀"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header value is invalid (attempt 0)", nullptr},
    {"v2 header not trimmed", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"retry-after":" 5"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header value is not canonical (attempt 0)", nullptr},
    {"v2 header nbsp", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"retry-after":"5 "},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header value is not canonical (attempt 0)", nullptr},
    {"v2 headers_truncated string", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":"false","chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header truncation flag is invalid (attempt 0)", nullptr},
    {"v2 chunk times length", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing does not match its chunks (attempt 0)", nullptr},
    {"v2 chunk times not array", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":0,"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing does not match its chunks (attempt 0)", nullptr},
    {"v2 chunk times negative", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[-1],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing is invalid (attempt 0)", nullptr},
    {"v2 chunk times fraction", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0.5],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing is invalid (attempt 0)", nullptr},
    {"v2 chunk times decreasing", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ==","YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[1,0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing is invalid (attempt 0)", nullptr},
    {"v2 chunk times over max", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[604800001],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt chunk timing is invalid (attempt 0)", nullptr},
    {"v2 started 0", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":0,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt start time is invalid (attempt 0)", nullptr},
    {"v2 started null", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":null,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt start time is invalid (attempt 0)", nullptr},
    {"v2 headers_at string", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":"1","ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header time is invalid (attempt 0)", nullptr},
    {"v2 ended fraction", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1.5}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt end time is invalid (attempt 0)", nullptr},
    {"v2 no header time", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":null,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream header-bearing attempt requires a header time (attempt 0)", nullptr},
    {"v2 pre-header header time", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream pre-header attempt cannot carry a header time (attempt 0)", nullptr},
    {"v2 header predates start", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1699999999999,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt header time predates its start (attempt 0)", nullptr},
    {"v2 end predates headers", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000000}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt end time predates its headers (attempt 0)", nullptr},
    {"v2 pending with end", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"pending","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt end time disagrees with its terminal (attempt 0)", nullptr},
    {"v2 eof without end", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt end time disagrees with its terminal (attempt 0)", nullptr},
    {"v2 timing exceeds", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[5],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt timing exceeds its bounds (attempt 0)", nullptr},
    {"v2 elapsed exceeds", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700604800001}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt timing exceeds its bounds (attempt 0)", nullptr},
    {"v2 pending pre-header ok", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":null,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", true, R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{"content-type":"application/json"},"headers_at_ms":null,"headers_truncated":false,"provider":"chatgpt_codex","started_at_ms":1700000000000,"status":null,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":null,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json"},
    {"v1 attempt in v2", R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt keys are invalid (attempt 0)", nullptr},
    {"v2 attempt in v1", R"json({"version":1,"attempts":[{"provider":"chatgpt_codex","status":200,"content_type":"application/json","chunks_base64":["YQ=="],"terminal":"eof","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[0],"started_at_ms":1700000000000,"headers_at_ms":1700000000001,"ended_at_ms":1700000000002}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", false, "Error: Sentinel upstream attempt keys are invalid (attempt 0)", nullptr},
  };

  // A: 5 Date.now() calls
  const char* const A_JSON = R"json({"version":2,"attempts":[{"provider":"surplus","status":200,"content_type":"text/event-stream","chunks_base64":["ZGF0YTogb25lCgo=","ZGF0YTogdHdvCgo="],"terminal":"eof","headers":{"content-type":"text/event-stream; charset=utf-8","retry-after":"7"},"headers_truncated":false,"chunk_times_ms":[10,15],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000020}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const A_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[10,15],"chunks_base64":["ZGF0YTogb25lCgo=","ZGF0YTogdHdvCgo="],"content_type":"text/event-stream","ended_at_ms":1700000000020,"headers":{"content-type":"text/event-stream; charset=utf-8","retry-after":"7"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // B: 4 Date.now() calls
  const char* const B_JSON = R"json({"version":2,"attempts":[{"provider":"metered","status":502,"content_type":"application/json","chunks_base64":["cGFydGlhbA=="],"terminal":"read_error","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[10],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000015}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const B_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[10],"chunks_base64":["cGFydGlhbA=="],"content_type":"application/json","ended_at_ms":1700000000015,"headers":{"content-type":"application/json"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"metered","started_at_ms":1700000000000,"status":502,"terminal":"read_error"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // C: 4 Date.now() calls
  const char* const C_JSON = R"json({"version":2,"attempts":[{"provider":"cerebras","status":200,"content_type":"text/event-stream","chunks_base64":["eA=="],"terminal":"cancelled","headers":{"content-type":"text/event-stream"},"headers_truncated":false,"chunk_times_ms":[10],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000015}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const C_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[10],"chunks_base64":["eA=="],"content_type":"text/event-stream","ended_at_ms":1700000000015,"headers":{"content-type":"text/event-stream"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"cerebras","started_at_ms":1700000000000,"status":200,"terminal":"cancelled"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // D: 3 Date.now() calls
  const char* const D_JSON = R"json({"version":2,"attempts":[{"provider":"deepseek","status":null,"content_type":null,"chunks_base64":[],"terminal":"fetch_error","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":null,"ended_at_ms":1700000000005},{"provider":"lithos","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000010,"headers_at_ms":null,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const D_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":1700000000005,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"deepseek","started_at_ms":1700000000000,"status":null,"terminal":"fetch_error"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"lithos","started_at_ms":1700000000010,"status":null,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // E: 6 Date.now() calls
  const char* const E_JSON = R"json({"version":2,"attempts":[{"provider":"chatgpt_codex","status":204,"content_type":"other","chunks_base64":[],"terminal":"eof","headers":{"content-length":"0"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000010},{"provider":"openrouter","status":101,"content_type":"other","chunks_base64":[],"terminal":"eof","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000015,"headers_at_ms":1700000000020,"ended_at_ms":1700000000025}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const E_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":"other","ended_at_ms":1700000000010,"headers":{"content-length":"0"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"chatgpt_codex","started_at_ms":1700000000000,"status":204,"terminal":"eof"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":"other","ended_at_ms":1700000000025,"headers":{},"headers_at_ms":1700000000020,"headers_truncated":false,"provider":"openrouter","started_at_ms":1700000000015,"status":101,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // F: 4 Date.now() calls
  const char* const F_JSON = R"json({"version":2,"attempts":[{"provider":"claude_code","status":429,"content_type":"application/json","chunks_base64":["YWJj"],"terminal":"pending","headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_truncated":true,"chunk_times_ms":[15],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const F_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[15],"chunks_base64":["YWJj"],"content_type":"application/json","ended_at_ms":null,"headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_at_ms":1700000000005,"headers_truncated":true,"provider":"claude_code","started_at_ms":1700000000000,"status":429,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // F2: 5 Date.now() calls
  const char* const F2_JSON = R"json({"version":2,"attempts":[{"provider":"claude_code","status":429,"content_type":"application/json","chunks_base64":["YWJj"],"terminal":"pending","headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_truncated":true,"chunk_times_ms":[15],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const F2_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[15],"chunks_base64":["YWJj"],"content_type":"application/json","ended_at_ms":null,"headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_at_ms":1700000000005,"headers_truncated":true,"provider":"claude_code","started_at_ms":1700000000000,"status":429,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // F3: 5 Date.now() calls
  const char* const F3_JSON = R"json({"version":2,"attempts":[{"provider":"claude_code","status":429,"content_type":"application/json","chunks_base64":["YWJj"],"terminal":"pending","headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_truncated":true,"chunk_times_ms":[15],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const F3_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[15],"chunks_base64":["YWJj"],"content_type":"application/json","ended_at_ms":null,"headers":{"content-type":"Application/JSON ; q=1","openai-processing-ms":"12"},"headers_at_ms":1700000000005,"headers_truncated":true,"provider":"claude_code","started_at_ms":1700000000000,"status":429,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // G: 11 Date.now() calls
  const char* const G_JSON = R"json({"version":2,"attempts":[{"provider":"metered","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"eof","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000040,"ended_at_ms":1700000000050},{"provider":"surplus","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000005,"headers_at_ms":null,"ended_at_ms":null},{"provider":"metered","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000010,"headers_at_ms":null,"ended_at_ms":null},{"provider":"surplus","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000015,"headers_at_ms":null,"ended_at_ms":null},{"provider":"metered","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000020,"headers_at_ms":null,"ended_at_ms":null},{"provider":"surplus","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000025,"headers_at_ms":null,"ended_at_ms":null},{"provider":"metered","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000030,"headers_at_ms":null,"ended_at_ms":null},{"provider":"surplus","status":null,"content_type":null,"chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000035,"headers_at_ms":null,"ended_at_ms":null}],"attempts_truncated":true,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const G_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":"application/json","ended_at_ms":1700000000050,"headers":{},"headers_at_ms":1700000000040,"headers_truncated":false,"provider":"metered","started_at_ms":1700000000000,"status":200,"terminal":"eof"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000005,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"metered","started_at_ms":1700000000010,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000015,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"metered","started_at_ms":1700000000020,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000025,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"metered","started_at_ms":1700000000030,"status":null,"terminal":"pending"},{"chunk_times_ms":[],"chunks_base64":[],"content_type":null,"ended_at_ms":null,"headers":{},"headers_at_ms":null,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000035,"status":null,"terminal":"pending"}],"attempts_truncated":true,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // H: 4 Date.now() calls
  const char* const H_JSON = R"json({"version":2,"attempts":[{"provider":"surplus","status":200,"content_type":"other","chunks_base64":["aGVsbG8gYnVmZmVyZWQ="],"terminal":"eof","headers":{"content-type":"text/plain"},"headers_truncated":false,"chunk_times_ms":[10],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000015}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const H_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[10],"chunks_base64":["aGVsbG8gYnVmZmVyZWQ="],"content_type":"other","ended_at_ms":1700000000015,"headers":{"content-type":"text/plain"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";
  // I: 3 Date.now() calls
  const char* const I_JSON = R"json({"version":2,"attempts":[{"provider":"surplus","status":200,"content_type":"other","chunks_base64":[],"terminal":"eof","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":1700000000010}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json";
  const char* const I_CANONICAL = R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":"other","ended_at_ms":1700000000010,"headers":{},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000000,"status":200,"terminal":"eof"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json";

  rt::QueuingStrategy zeroHighWaterMark() {
    rt::QueuingStrategy strategy;
    strategy.highWaterMark = 0;
    return strategy;
  }

  // Date.now = () => 1_700_000_000_000 + 5 * calls++
  std::shared_ptr<double> useClock() {
    auto calls = std::make_shared<double>(0);
    rt::globalDateNow() = [calls] { return 1'700'000'000'000.0 + 5.0 * (*calls)++; };
    return calls;
  }

  enum class End { close, error, never };

  // The TS scenario's streamOf(parts, end): one part per pull, then close, error, or "tail" forever.
  rt::ByteStream streamOf(std::vector<rt::ByteChunk> parts, End end = End::close, rt::Reason error = nullptr) {
    auto queue = std::make_shared<std::deque<rt::ByteChunk>>(parts.begin(), parts.end());
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [queue, end, error](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      if (queue->empty()) {
        if (end == End::close) controller.close();
        else if (end == End::error) controller.error(error);
        else controller.enqueue(rt::encodeUtf8("tail"));
        return;
      }
      rt::ByteChunk next = std::move(queue->front());
      queue->pop_front();
      controller.enqueue(std::move(next));
    };
    return rt::ByteStream(std::move(source), zeroHighWaterMark());
  }

  rt::ResponseInit responseInit(double status, rt::Headers headers = rt::Headers()) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = std::move(headers);
    return init;
  }

  void expectTrace(const SentinelUpstreamTrace& trace, const char* expectedJson, const char* expectedCanonical, const char* label) {
    const std::string actualJson = rt::jsonStringify(json::value_from(trace));
    if (actualJson != expectedJson) rt::test::report(__FILE__, __LINE__, std::string(label) + " JSON.stringify\n      actual:   " + actualJson + "\n      expected: " + expectedJson, false);
    const std::string actualCanonical = canonicalSentinelUpstreamJson(trace);
    if (actualCanonical != expectedCanonical) {
      rt::test::report(__FILE__, __LINE__, std::string(label) + " canonical\n      actual:   " + actualCanonical + "\n      expected: " + expectedCanonical, false);
    }
  }

  json::value v1Trace(json::array attempts) {
    return json::object{{"version", 1}, {"attempts", std::move(attempts)}, {"attempts_truncated", false}, {"bytes_truncated", false}, {"chunks_truncated", false}};
  }

  json::object attemptWithChunks(json::array chunks) {
    return json::object{{"provider", "chatgpt_codex"}, {"status", 200}, {"content_type", "application/json"}, {"chunks_base64", std::move(chunks)}, {"terminal", "eof"}};
  }

  std::string parseOutcome(const json::value& value) {
    try {
      parseSentinelUpstreamTrace(value);
      return "ok";
    } catch (const uos::rt::JsError& error) {
      return error.toString();
    }
  }
}

TEST("differential: parseSentinelUpstreamTrace results and messages match Bun") {
  EXPECT_EQ(std::size(PARSE_VECTORS), std::size_t{72});
  for (const ParseVector& vector : PARSE_VECTORS) {
    const json::value input = rt::jsonParse(vector.input);
    EXPECT_EQ(isSentinelUpstreamTrace(input), vector.ok);
    try {
      const SentinelUpstreamTrace parsed = parseSentinelUpstreamTrace(input);
      if (!vector.ok) {
        rt::test::report(__FILE__, __LINE__, std::string(vector.name) + ": parsed, expected " + vector.expected, false);
        continue;
      }
      const std::string canonical = canonicalSentinelUpstreamJson(parsed);
      if (canonical != vector.expected) rt::test::report(__FILE__, __LINE__, std::string(vector.name) + ": canonical " + canonical, false);
      const std::string stringified = rt::jsonStringify(json::value_from(parsed));
      if (stringified != vector.json) rt::test::report(__FILE__, __LINE__, std::string(vector.name) + ": JSON " + stringified, false);
      EXPECT(parseSentinelUpstreamTrace(parsed) == parsed);
    } catch (const rt::JsError& error) {
      if (vector.ok || error.toString() != vector.expected) {
        rt::test::report(__FILE__, __LINE__, std::string(vector.name) + ": threw " + error.toString() + (vector.ok ? std::string() : ", expected " + std::string(vector.expected)), false);
      }
    }
  }
}

TEST("differential: aggregate chunk and byte bounds match Bun") {
  json::array tooMany;
  for (int index = 0; index < 4097; ++index) tooMany.emplace_back("YQ==");
  json::array attempts;
  attempts.emplace_back(attemptWithChunks(std::move(tooMany)));
  EXPECT_EQ(parseOutcome(v1Trace(std::move(attempts))), "Error: Sentinel upstream trace has too many chunks");

  json::array overBytes;
  overBytes.emplace_back(json::string(rt::btoa(std::string(3 * 1024 * 1024, 'a'))));
  overBytes.emplace_back(json::string(rt::btoa(std::string(1024 * 1024 + 1, 'b'))));
  json::array overAttempts;
  overAttempts.emplace_back(attemptWithChunks(std::move(overBytes)));
  EXPECT_EQ(parseOutcome(v1Trace(std::move(overAttempts))), "Error: Sentinel upstream trace exceeds its byte bound");

  // One chunk at exactly the byte bound: 5,592,408 base64 characters in a single
  // canonical string (Bun's regex and decoder accept it).
  json::array atBound;
  atBound.emplace_back(json::string(rt::toBase64(std::vector<std::uint8_t>(static_cast<std::size_t>(SENTINEL_UPSTREAM_MAX_BYTES), std::uint8_t{0x5a}))));
  json::array atBoundAttempts;
  atBoundAttempts.emplace_back(attemptWithChunks(std::move(atBound)));
  EXPECT_EQ(parseOutcome(v1Trace(std::move(atBoundAttempts))), "ok");
}

TEST("differential: content type and safe header normalization match Bun") {
  EXPECT_EQ(normalizeSentinelUpstreamContentType(std::nullopt), std::nullopt);
  EXPECT_EQ(normalizeSentinelUpstreamContentType(""), std::optional<std::string>("other"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType(";"), std::optional<std::string>("other"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType(" text/event-stream ;x"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("TEXT/EVENT-STREAM"), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("application/json;charset=utf-8"), std::optional<std::string>("application/json"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("application/json+x"), std::optional<std::string>("other"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType(" application/json "), std::optional<std::string>("application/json"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("text/event-stream "), std::optional<std::string>("text/event-stream"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("Application/Json; charset"), std::optional<std::string>("application/json"));
  EXPECT_EQ(normalizeSentinelUpstreamContentType("text/plain"), std::optional<std::string>("other"));

  const NormalizedSentinelUpstreamSafeHeaders none = normalizeSentinelUpstreamSafeHeaders(rt::Headers());
  EXPECT_EQ(rt::jsonStringify(json::value(none.headers)), "{}");
  EXPECT_EQ(none.truncated, false);
  const NormalizedSentinelUpstreamSafeHeaders some =
    normalizeSentinelUpstreamSafeHeaders(rt::Headers{{"content-type", "a"}, {"cache-control", "no-cache"}, {"retry-after", "1"}, {"x-other", "y"}});
  EXPECT_EQ(rt::jsonStringify(json::value(some.headers)), R"({"cache-control":"no-cache","content-type":"a","retry-after":"1"})");
  EXPECT_EQ(some.truncated, false);
  const NormalizedSentinelUpstreamSafeHeaders bounded =
    normalizeSentinelUpstreamSafeHeaders(rt::Headers{{"x-ratelimit-reset-requests", std::string(512, 'x')}, {"x-ratelimit-reset-tokens", std::string(513, 'x')}});
  EXPECT_EQ(rt::jsonStringify(json::value(bounded.headers)), "{\"x-ratelimit-reset-requests\":\"" + std::string(512, 'x') + "\"}");
  EXPECT_EQ(bounded.truncated, true);
  const NormalizedSentinelUpstreamSafeHeaders latin = normalizeSentinelUpstreamSafeHeaders(rt::Headers{{"x-ratelimit-remaining-tokens", rt::repeat("é", 513)}});
  EXPECT_EQ(rt::jsonStringify(json::value(latin.headers)), "{}");
  EXPECT_EQ(latin.truncated, true);
  const NormalizedSentinelUpstreamSafeHeaders blank = normalizeSentinelUpstreamSafeHeaders(rt::Headers{{"content-length", " "}});
  EXPECT_EQ(rt::jsonStringify(json::value(blank.headers)), "{}");
  EXPECT_EQ(blank.truncated, false);
}

ASYNC_TEST("differential: two chunks then EOF (scenario A)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  std::vector<rt::ByteChunk> parts{rt::encodeUtf8("data: one\n\n"), rt::encodeUtf8("data: two\n\n")};
  const rt::Response wrapped = recorder.startAttempt("surplus").wrap(rt::Response(
    rt::BodyInit(streamOf(std::move(parts))), responseInit(200, rt::Headers{{"Content-Type", "text/event-stream; charset=utf-8"}, {"Retry-After", " 7 "}, {"X-Request-Id", "secret"}})));
  const auto reader = wrapped.body().value().getReader();
  for (;;) {
    const auto step = co_await reader.read();
    if (step.done) break;
  }
  expectTrace(recorder.snapshotAndSeal(), A_JSON, A_CANONICAL, "A");
  EXPECT_EQ(*calls, 5.0);
}

ASYNC_TEST("differential: read error after one chunk (scenario B)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  std::vector<rt::ByteChunk> parts{rt::encodeUtf8("partial")};
  const rt::Reason boom = rt::makeReason(rt::JsError("boom"));
  const rt::Response wrapped =
    recorder.startAttempt("metered").wrap(rt::Response(rt::BodyInit(streamOf(std::move(parts), End::error, boom)), responseInit(502, rt::Headers{{"content-type", "application/json"}})));
  const auto reader = wrapped.body().value().getReader();
  co_await reader.read();
  EXPECT_REJECTS_REASON(reader.read(), boom);
  expectTrace(recorder.snapshotAndSeal(), B_JSON, B_CANONICAL, "B");
  EXPECT_EQ(*calls, 4.0);
}

ASYNC_TEST("differential: cancel after one chunk (scenario C)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  std::vector<rt::ByteChunk> parts{rt::encodeUtf8("x"), rt::encodeUtf8("y")};
  const rt::Response wrapped =
    recorder.startAttempt("cerebras").wrap(rt::Response(rt::BodyInit(streamOf(std::move(parts))), responseInit(200, rt::Headers{{"content-type", "text/event-stream"}})));
  const auto reader = wrapped.body().value().getReader();
  co_await reader.read();
  const rt::Reason stop = rt::makeReason(rt::JsError("stop"));
  co_await reader.cancel(stop);
  expectTrace(recorder.snapshotAndSeal(), C_JSON, C_CANONICAL, "C");
  EXPECT_EQ(*calls, 4.0);
}

ASYNC_TEST("differential: fetch error before headers and an unwrapped attempt (scenario D)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const SentinelUpstreamAttemptHandle first = recorder.startAttempt("deepseek");
  first.recordFetchError();
  first.recordFetchError();
  recorder.startAttempt("lithos");
  expectTrace(recorder.snapshotAndSeal(), D_JSON, D_CANONICAL, "D");
  EXPECT_EQ(*calls, 3.0);
  co_return;
}

ASYNC_TEST("differential: bodyless 204 and 101 attempts (scenario E)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const SentinelUpstreamAttemptHandle handle = recorder.startAttempt("chatgpt_codex");
  handle.wrap(rt::Response(nullptr, responseInit(204, rt::Headers{{"content-length", "0"}, {"cache-control", "  "}})));
  handle.recordFetchError();
  recorder.startAttempt("openrouter").wrap(rt::Response(nullptr, responseInit(101)));
  expectTrace(recorder.snapshotAndSeal(), E_JSON, E_CANONICAL, "E");
  EXPECT_EQ(*calls, 6.0);
  co_return;
}

ASYNC_TEST("differential: header bounds, an empty chunk and sealing while pending (scenario F)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  rt::Headers headers{
    {"content-type", "Application/JSON ; q=1"}, {"x-ratelimit-limit-tokens", std::string(513, '9')}, {"x-ratelimit-reset-tokens", " "}, {"openai-processing-ms", " 12 "}};
  std::vector<rt::ByteChunk> parts{rt::ByteChunk(), rt::encodeUtf8("abc")};
  const rt::Response wrapped = recorder.startAttempt("claude_code").wrap(rt::Response(rt::BodyInit(streamOf(std::move(parts), End::never)), responseInit(429, std::move(headers))));
  const auto reader = wrapped.body().value().getReader();
  co_await reader.read();
  co_await reader.read();
  expectTrace(recorder.snapshotAndSeal(), F_JSON, F_CANONICAL, "F");
  EXPECT_EQ(*calls, 4.0);
  co_await reader.read();
  expectTrace(recorder.snapshotAndSeal(), F2_JSON, F2_CANONICAL, "F2");
  EXPECT_EQ(*calls, 5.0);
  recorder.dispose();
  expectTrace(recorder.snapshotAndSeal(), F3_JSON, F3_CANONICAL, "F3");
}

ASYNC_TEST("differential: nine attempts and dispose before sealing (scenario G)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  std::vector<SentinelUpstreamAttemptHandle> handles;
  for (int index = 0; index < 9; ++index) handles.push_back(recorder.startAttempt(index % 2 ? "surplus" : "metered"));
  std::vector<rt::ByteChunk> parts{rt::encodeUtf8("kept")};
  const rt::Response wrapped = handles[0].wrap(rt::Response(rt::BodyInit(streamOf(std::move(parts))), responseInit(200, rt::Headers{{"content-type", "application/json"}})));
  const auto reader = wrapped.body().value().getReader();
  co_await reader.read();
  recorder.dispose();
  co_await reader.read();
  expectTrace(recorder.snapshotAndSeal(), G_JSON, G_CANONICAL, "G");
  EXPECT_EQ(*calls, 11.0);
}

ASYNC_TEST("differential: a buffered body read through arrayBuffer (scenario H)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const rt::Response wrapped = recorder.startAttempt("surplus").wrap(rt::Response(std::string("hello buffered"), responseInit(200, rt::Headers{{"content-type", "text/plain"}})));
  co_await wrapped.arrayBuffer();
  expectTrace(recorder.snapshotAndSeal(), H_JSON, H_CANONICAL, "H");
  EXPECT_EQ(*calls, 4.0);
}

ASYNC_TEST("differential: an empty string body (scenario I)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const rt::Response wrapped = recorder.startAttempt("surplus").wrap(rt::Response(std::string(), responseInit(200)));
  const std::string text = co_await wrapped.text();
  EXPECT_EQ(text, "");
  expectTrace(recorder.snapshotAndSeal(), I_JSON, I_CANONICAL, "I");
  EXPECT_EQ(*calls, 3.0);
}

ASYNC_TEST("differential: a locked body is returned unwrapped (scenario J)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const rt::Response original(std::string("locked"), responseInit(200, rt::Headers{{"content-type", "application/json"}}));
  original.body().value().getReader();
  const rt::Response wrapped = recorder.startAttempt("surplus").wrap(original);
  EXPECT(wrapped == original);
  expectTrace(recorder.snapshotAndSeal(), R"json({"version":2,"attempts":[{"provider":"surplus","status":200,"content_type":"application/json","chunks_base64":[],"terminal":"pending","headers":{"content-type":"application/json"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json", R"json({"attempts":[{"chunk_times_ms":[],"chunks_base64":[],"content_type":"application/json","ended_at_ms":null,"headers":{"content-type":"application/json"},"headers_at_ms":1700000000005,"headers_truncated":false,"provider":"surplus","started_at_ms":1700000000000,"status":200,"terminal":"pending"}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false,"version":2})json", "J");
  EXPECT_EQ(*calls, 2.0);
  co_return;
}

ASYNC_TEST("differential: Response.error() cannot be rebuilt and keeps its locked body (scenario K)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const rt::Response original = rt::Response::error();
  const rt::Response wrapped = recorder.startAttempt("metered").wrap(original);
  EXPECT(wrapped == original);
  EXPECT_EQ(original.body().value().locked(), true);
  const std::string stringified = rt::jsonStringify(json::value_from(recorder.snapshotAndSeal()));
  EXPECT_EQ(stringified, R"json({"version":2,"attempts":[{"provider":"metered","status":null,"content_type":"other","chunks_base64":[],"terminal":"pending","headers":{},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000005,"ended_at_ms":null}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json");
  EXPECT_EQ(*calls, 2.0);
  co_return;
}

ASYNC_TEST("differential: a second wrap on one handle (scenario L)") {
  const auto calls = useClock();
  SentinelUpstreamRecorder recorder = createSentinelUpstreamRecorder();
  const SentinelUpstreamAttemptHandle handle = recorder.startAttempt("lithos");
  handle.wrap(rt::Response(nullptr, responseInit(503, rt::Headers{{"retry-after", "9"}})));
  const rt::Response second = handle.wrap(rt::Response(std::string("again"), responseInit(200, rt::Headers{{"content-type", "text/event-stream"}})));
  co_await second.text();
  const std::string stringified = rt::jsonStringify(json::value_from(recorder.snapshotAndSeal()));
  EXPECT_EQ(stringified, R"json({"version":2,"attempts":[{"provider":"lithos","status":200,"content_type":"text/event-stream","chunks_base64":[],"terminal":"eof","headers":{"content-type":"text/event-stream"},"headers_truncated":false,"chunk_times_ms":[],"started_at_ms":1700000000000,"headers_at_ms":1700000000015,"ended_at_ms":1700000000010}],"attempts_truncated":false,"bytes_truncated":false,"chunks_truncated":false})json");
  EXPECT_EQ(*calls, 6.0);
}
