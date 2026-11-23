// Port of tests/embeddings-coverage.test.ts (all 23 cases).
// The HOME permission-refusal oracle is pinned separately; rt env has no injectable permission refusal.
// Links: tests/embeddings_coverage_test.cpp
// src/embeddings/voyage.cpp tests/helpers/embeddings_harness.cpp src/embeddings/ledger.cpp src/utils.cpp src/kv.cpp
// src/embeddings/jobs.cpp src/openai_telemetry.cpp src/responses_failover_stream.cpp src/openai.cpp src/codex/responses_error.cpp
// src/request.cpp src/runtime_config.cpp src/embeddings/handlers.cpp src/responses_stream.cpp src/http.cpp
// src/defaults.cpp src/config.cpp src/provider/removed_provider_telemetry.cpp src/request_policy.cpp src/models/codex_models.cpp
// src/inference_deadline.cpp src/openai_schema.cpp src/provider/surplus.cpp src/auth/relay.cpp src/upstream_wire.cpp
// src/storage/kv.cpp src/provider/cerebras.cpp src/deepseek/index.cpp src/provider/surplus_claude_request.cpp src/storage/sqlite_readonly.cpp
// src/provider/cerebras_rate_limits.cpp src/storage/native_kv.cpp src/provider/claude_code_models.cpp src/provider/claude_code_accounts.cpp src/provider/anthropic_chat_responses.cpp
// src/release.cpp src/models/codex_models_fetch.cpp src/provider/anthropic_response_wire.cpp src/types.cpp src/codex/dispatch.cpp
// src/provider/metered.cpp src/provider/claude_code_credentials.cpp src/provider/health.cpp src/provider/selection.cpp src/bounded_response_body.cpp
// src/provider/anthropic_chat_stream.cpp src/authority/native_codex_contracts.cpp src/provider/anthropic_replay.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/lithos.cpp src/codex/auth.cpp src/models/codex_models_availability.cpp src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp
// src/provider/capacity_events.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/auth_refresh.cpp
// src/codex/routing_state.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp
// src/authority/native_codex_evaluation.cpp src/provider/lithos_rate_limits.cpp src/authority/native_codex_429.cpp src/codex/banked_reset.cpp src/provider/capacity.cpp
// src/provider/capacity_history.cpp src/provider/capacity_contract.cpp src/provider/claude_capacity_rollups.cpp src/provider/anthropic_responses.cpp src/provider/claude_capacity_history.cpp
// src/provider/capacity_parse.cpp src/provider/claude_code_usage_schema.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/paid_fallback/ledger_stats.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity_rollups.cpp src/provider/capacity_analytics.cpp src/codex/experiment.cpp src/codex/reset_settings.cpp
// src/codex/banked_reset_provider.cpp src/metered_quota.cpp src/runtime/websocket.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
// src/codex/routing_evaluation.cpp src/authority/native_codex.cpp src/codex/overage_settings.cpp
#include "tests/helpers/embeddings_harness.hpp"
#include "uos/embeddings/voyage.hpp"
#include "uos/kv.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include <bit>
#include <cmath>
#include <limits>
#include <memory>

#define AWAIT(name, expr) auto name##Task = (expr); auto name = co_await std::move(name##Task)
#define WAIT(expr) do { auto waitingTask = (expr); co_await std::move(waitingTask); } while (false)
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::embeddings_harness;
  namespace v = uos::embeddings::voyage;
  namespace l = uos::embeddings::ledger;
  rt::Request jobRequest(const json::value& body) {
    return rt::Request("https://ai.ubq.fi/uos/embedding-jobs", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}},
      .body = body.is_string() ? std::string(body.get_string()) : rt::jsonStringify(body)});
  }
  json::object jobBody(json::value input = "hello") { return {{"model", "voyage-4-large"}, {"input", input}, {"input_type", "document"}}; }
  rt::Task<json::value> errorOf(rt::Response response) { auto value = co_await response.json(); co_return *rt::get(value, "error"); }
  rt::Task<json::value> parseFailure(l::EmbeddingsParseResult parsed) {
    ASSERT(std::holds_alternative<l::EmbeddingsParseResultNotOk>(parsed));
    auto value = co_await errorOf(std::get<l::EmbeddingsParseResultNotOk>(parsed).response); co_return value;
  }
  void resetStore() { h::kvStore.clear(); h::kvStore.set(h::keyToString(l::VOYAGE_API_KEY_KV_KEY), rt::KvValue("voyage_test_key")); }
  void put(const rt::KvKey& key, const json::value& value) { h::kvStore.set(h::keyToString(key), rt::KvValue(value)); }
  const rt::KvValue* row(const rt::KvKey& key) { return h::kvStore.get(h::keyToString(key)); }
  class FaultKv final : public rt::ForwardingKv {
    std::string mode_;
  public:
    explicit FaultKv(std::string mode) : rt::ForwardingKv(*h::kvStub), mode_(std::move(mode)) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      return std::make_unique<rt::GenericAtomicOperation>([this](std::vector<rt::AtomicCheck>, std::vector<rt::KvMutation> ops) -> rt::Task<rt::KvCommitResult> {
        if (mode_ == "throw") throw rt::JsError("KV storage quota exceeded");
        if (mode_ == "none") return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = ""});
        for (const auto& op : ops) {
          if (mode_ == "rate" && h::keyToString(op.key) == h::keyToString(l::VOYAGE_RATE_LIMIT_KEY)) return rt::fulfilledTask(rt::KvCommitResult{});
          if (mode_ == "cache" && op.key.size() > 2 && op.key[0].isString() && op.key[0].string() == "embeddings" && op.key[2].isString() && op.key[2].string() == "cache")
            return rt::fulfilledTask(rt::KvCommitResult{});
        }
        for (const auto& op : ops) {
          const auto raw = h::keyToString(op.key);
          if (op.type == rt::KvMutationType::set) h::kvStore.set(raw, op.value);
          else h::kvStore.delete_(raw);
        }
        return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = ""});
      });
    }
  };
  v::FetchVoyageEmbeddingsParams transportParams() { return {"voyage-test-key", "voyage-4-large", {"hello"}, "document", 1024, "float", true, rt::dateNow() + 10000}; }
  rt::Fetch mock(std::function<rt::Response(std::string, std::optional<std::string>, rt::Headers)> fn) { return h::makeFetchMock(h::adaptFetchMock(std::move(fn))); }
  rt::Task<rt::Reason> rejection(rt::Task<v::VoyageEmbeddingsResult> task) {
    rt::Reason error;
    try { auto ignored = co_await std::move(task); (void)ignored; } catch (...) { error = std::current_exception(); }
    if (error) co_return error;
    throw rt::JsError("expected rejection");
  }
  struct Queued { std::string id; std::string profile; std::string hash; };
  rt::Task<Queued> queuedJob(std::string token, json::value input) {
    rt::GlobalFetchOverride fetch(mock([](auto, auto, auto) { return rt::Response("too many requests", rt::ResponseInit{.status=429, .headers=rt::Headers{{"Retry-After", "30"}}}); }));
    AWAIT(created, h::handleEmbeddingsJobCreate(jobRequest(jobBody(input)), token));
    EXPECT_EQ(created.status(), 202);
    AWAIT(payload, created.json());
    EXPECT_EQ(rt::getString(payload, "status"), "queued");
    const std::string id(*rt::getString(payload, "id"));
    AWAIT(hash, h::sha256Hex(token));
    const auto& lookup = *row(h::embeddingsJobLookupKey(hash,id));
    const auto profile = lookup["cache_profile_key"].asString();
    const auto& job = *row(h::embeddingsJobKey(hash,profile,id));
    co_return Queued{id,profile,job["input_hashes"][std::size_t{0}].asString()};
  }
  rt::Task<rt::Response> poll(const Queued& job, const std::string& token) {
    return h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/"+job.id), token, job.id);
  }
}

ASYNC_TEST("embeddings request parsing rejects every malformed field for both contracts") {
  h::HarnessScope fixture;
  struct Case { json::object body; std::string message; std::optional<std::string> param; };
  const std::vector<Case> cases{
    {{{"model","voyage-4-large"},{"input",7}},"input must be a string or an array of strings","input"},
    {{{"model","voyage-4-large"},{"input",json::array{"ok",7}}},"input must be a string or an array of strings","input"},
    {{{"model","voyage-4-large"},{"input",json::array{}}},"input must be a non-empty string or a non-empty array","input"},
    {{{"model","voyage-4-large"},{"input",std::string(20001,'x')}},"Input too large: 20001 chars (max 20000)","input"},
    {{{"model","voyage-4-large"},{"input",rt::repeat("€",20000)}},"Input too large for embeddings provider: ~15000 tokens (max 10000).","input"},
    {{{"model","voyage-4-large"},{"input",json::value_from(std::vector<std::string>(6,std::string(20000,'x')))}},"Request too large: 120000 chars total (max 100000)","input"},
    {{{"input","hello"}},"model is required and must be a non-empty string","model"},
    {{{"model","text-embedding-3-small"},{"input","hello"}},"Unsupported embedding model: text-embedding-3-small","model"},
    {{{"model","voyage-4-large"},{"input","hello"},{"dimensions",3}},"dimensions must be one of: 256, 512, 1024, 2048","dimensions"},
    {{{"model","voyage-4-large"},{"input","hello"},{"dimensions",1.5}},"dimensions must be an integer","dimensions"},
    {{{"model","voyage-4-large"},{"input","hello"},{"encoding_format","float32"}},"encoding_format must be one of: \"float\", \"base64\"","encoding_format"},
    {{{"model","voyage-4-large"},{"input","hello"},{"encoding_format",5}},"encoding_format must be a string","encoding_format"},
    {{{"model","voyage-4-large"},{"input","hello"},{"input_type","other"}},"input_type must be one of: \"query\", \"document\"","input_type"},
    {{{"model","voyage-4-large"},{"input","hello"},{"truncation","yes"}},"truncation must be a boolean","truncation"},
    {{{"model","voyage-4-large"},{"input","hello"},{"user",7}},"user must be a string","user"},
    {{{"model","voyage-4-large"},{"input","hello"},{"unexpected",true}},"Unrecognized request argument supplied: unexpected",std::nullopt}
  };
  for (const auto& item : cases) {
    AWAIT(error,parseFailure(v::parseUosEmbeddingsRequest(item.body)));
    EXPECT_EQ(rt::getString(error,"message"),item.message);
    if (item.param) EXPECT_EQ(rt::getString(error,"param"),*item.param);
  }
  const json::object missing{{"model","voyage-4-large"},{"input","hello"}};
  AWAIT(inputType,parseFailure(v::parseEmbeddingsJobRequest(missing)));
  EXPECT_EQ(rt::getString(inputType,"message"),"input_type is required and must be one of: \"query\", \"document\"");
  auto base64=jobBody();base64["encoding_format"]="base64";
  AWAIT(encoding,parseFailure(v::parseEmbeddingsJobRequest(base64)));
  EXPECT_EQ(rt::getString(encoding,"message"),"encoding_format must be \"float\" for embeddings jobs");
  const json::object valid{{"model","voyage-4-large"},{"input","hello"},{"user",nullptr}};
  const auto parsed=v::parseUosEmbeddingsRequest(valid);ASSERT(std::holds_alternative<l::EmbeddingsParseResultOk>(parsed));
  const auto& value=std::get<l::EmbeddingsParseResultOk>(parsed).value;
  EXPECT_EQ(value.inputs,std::vector<std::string>{"hello"});EXPECT_EQ(value.total_chars,5);
  EXPECT_EQ(value.profile.cache_profile_key,h::embeddingsProfileKey());EXPECT_EQ(value.profile.input_type,"document");
  EXPECT_EQ(value.profile.dimensions,1024);EXPECT_EQ(value.profile.encoding_format,"float");EXPECT(value.profile.truncation);
  co_return;
}
ASYNC_TEST("embeddings token estimation chunks inputs by item and token budget") {
  h::HarnessScope fixture;
  EXPECT_EQ(v::estimateTokens(""),0);EXPECT_EQ(v::estimateTokens("abcd"),1);EXPECT_EQ(v::estimateTokens("abcde"),2);
  EXPECT_EQ(v::estimateTokenCount({"abcd","abcd"}),2);
  std::vector<v::EmbeddingsChunkItem> items;
  for(int i=0;i<4;++i)items.push_back({"h"+rt::numberToString(i),std::string(400,'x')});
  const auto chunks=v::chunkByTokenBudget(items,128,250);ASSERT_EQ(chunks.size(),2U);EXPECT_EQ(chunks[0][0].hash,"h0");EXPECT_EQ(chunks[0][1].hash,"h1");EXPECT_EQ(chunks[1][0].hash,"h2");
  EXPECT_EQ(v::chunkByTokenBudget(items,1,10000).size(),4U);EXPECT(v::chunkByTokenBudget({},5,5).empty());EXPECT_EQ(v::chunkByTokenBudget(items,0,10000).size(),4U);
  co_return;
}
ASYNC_TEST("voyage rate limiting reserves, refuses and recovers from a lost commit") {
  h::HarnessScope fixture;h::resetVoyageRateLimit();
  AWAIT(reserved,v::tryReserveVoyageBudget(*h::kvStub,10));EXPECT(reserved.ok);
  EXPECT_EQ((*row(l::VOYAGE_RATE_LIMIT_KEY))["requests"].asNumber(),1);EXPECT_EQ((*row(l::VOYAGE_RATE_LIMIT_KEY))["tokens"].asNumber(),10);
  put(l::VOYAGE_RATE_LIMIT_KEY,json::object{{"window_start_ms",-1},{"requests",-2},{"tokens",-3}});
  AWAIT(replaced,v::tryReserveVoyageBudget(*h::kvStub,1));EXPECT(replaced.ok);
  put(l::VOYAGE_RATE_LIMIT_KEY,json::object{{"window_start_ms",rt::makeNumber(rt::dateNow())},{"requests",3},{"tokens",0}});
  AWAIT(refused,v::tryReserveVoyageBudget(*h::kvStub,1));EXPECT(!refused.ok);EXPECT(*refused.wait_ms>0&&*refused.wait_ms<=60000);
  put(l::VOYAGE_RATE_LIMIT_KEY,json::object{{"window_start_ms",rt::makeNumber(rt::dateNow())},{"requests",0},{"tokens",10000}});
  AWAIT(tokens,v::tryReserveVoyageBudget(*h::kvStub,1));EXPECT(!tokens.ok);
  h::resetVoyageRateLimit();h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return true;});
  AWAIT(lost,v::tryReserveVoyageBudget(*h::kvStub,1));EXPECT(!lost.ok);EXPECT_EQ(lost.wait_ms,0);
  AWAIT(recovered,v::applyVoyageRateLimit(*h::kvStub,1,rt::dateNow()+2000));EXPECT(recovered.ok);EXPECT_EQ((*row(l::VOYAGE_RATE_LIMIT_KEY))["requests"].asNumber(),1);
  AWAIT(expired,v::applyVoyageRateLimit(*h::kvStub,1,rt::dateNow()-1));EXPECT(!expired.ok);EXPECT_EQ(expired.wait_ms,0);
  put(l::VOYAGE_RATE_LIMIT_KEY,json::object{{"window_start_ms",rt::makeNumber(rt::dateNow()-59990)},{"requests",3},{"tokens",0}});
  AWAIT(waited,v::applyVoyageRateLimit(*h::kvStub,1,rt::dateNow()+5000));EXPECT(waited.ok);h::resetVoyageRateLimit();
  co_return;
}
ASYNC_TEST("embeddings sleep helpers honor aborts and deadlines") {
  h::HarnessScope fixture;bool resumed=false;auto pending=v::sleep(1).then([&]{resumed=true;});EXPECT(!resumed);co_await pending.wait();EXPECT(resumed);
  rt::AbortController clean;auto normal=v::sleepUnlessAborted(1,clean.signal());EXPECT(co_await normal.wait());
  rt::AbortController pre;pre.abort(rt::makeReason(rt::JsError("already cancelled")));auto stopped=v::sleepUnlessAborted(10000,pre.signal());EXPECT(!(co_await stopped.wait()));
  rt::AbortController during;auto sleeping=v::sleepUnlessAborted(10000,during.signal());during.abort(rt::makeReason(rt::JsError("cancelled while sleeping")));EXPECT(!(co_await sleeping.wait()));
  co_return;
}
TEST("abandoning an embeddings retry timer releases the abort signal") {
  const rt::AbortController caller;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const auto signal = rt::AbortSignal::any({caller.signal()});
    signalState = signal.state();
    const auto waiting = v::sleepUnlessAborted(10000, signal);
    EXPECT(waiting.pending());
    EXPECT_EQ(loop.timerCount(), 1u);
  }
  EXPECT(signalState.expired());
}
ASYNC_TEST("embeddings job token seeds prefer stable principals") {
  h::HarnessScope fixture;EXPECT_EQ(v::resolveEmbeddingsJobTokenSeed("job-1",std::nullopt),"job-1");EXPECT_EQ(v::resolveEmbeddingsJobTokenSeed("job-1","bearer-token"),"bearer-token");
  auto context=h::uosIdempotencyUsageContext("x");context.keyId="key-9";EXPECT_EQ(v::resolveEmbeddingsJobTokenSeed("job-1","bearer-token",&context),"uos_api_key_id:key-9");
  context.keyId.reset();context.kernelRepo=uos::openai_telemetry::UsageContext::KernelRepo{"ubiquity","ai.ubq.fi"};EXPECT_EQ(v::resolveEmbeddingsJobTokenSeed("job-1","bearer-token",&context),"uos_kernel_repo:ubiquity/ai.ubq.fi");
  co_return;
}
ASYNC_TEST("voyage environment and API key lookups fail closed") {
  h::HarnessScope fixture;rt::env::ScopedOverride missing({{"HOME",std::nullopt}});EXPECT(!v::getEnv("HOME"));
  rt::env::set("VOYAGEAI_API_KEY","  env-voyage-key  ");AWAIT(env,v::readVoyageApiKey(nullptr));EXPECT_EQ(env,"env-voyage-key");AWAIT(first,v::readVoyageApiKey(h::kvStub));EXPECT_EQ(first,"env-voyage-key");rt::env::remove("VOYAGEAI_API_KEY");
  AWAIT(absent,v::readVoyageApiKey(nullptr));EXPECT(!absent);put(l::VOYAGE_API_KEY_KV_KEY,"   ");AWAIT(blank,v::readVoyageApiKey(h::kvStub));EXPECT(!blank);
  put(l::VOYAGE_API_KEY_KV_KEY,"  kv-voyage-key  ");AWAIT(stored,v::readVoyageApiKey(h::kvStub));EXPECT_EQ(stored,"kv-voyage-key");
  co_return;
}
ASYNC_TEST("embeddings cache writes recover from quota errors and abandon unusable ones") {
  h::HarnessScope fixture;resetStore();const auto profile=h::embeddingsProfileKey();const auto vector=h::testVector(1024);const double deadline=rt::dateNow()+30000;
  h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return rt::makeReason(rt::JsError("cache write transport failed"));});
  AWAIT(abandoned,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-abandoned",vector,rt::dateNow(),deadline));EXPECT(!abandoned.isNew);EXPECT(!row(l::embeddingsCacheKey(profile,"hash-abandoned")));
  const double old=rt::dateNow()-60000;AWAIT(prior,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-old",vector,old,deadline));EXPECT(prior.isNew);
  h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return rt::makeReason(rt::JsError("KV storage quota exceeded"));});
  AWAIT(fresh,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-new",vector,rt::dateNow(),deadline));EXPECT(fresh.isNew);EXPECT(row(l::embeddingsCacheKey(profile,"hash-new")));EXPECT(!row(l::embeddingsCacheKey(profile,"hash-old")));EXPECT(!row(l::embeddingsCacheGlobalIndexKey(old,profile,"hash-old")));
  resetStore();const std::vector<rt::KvKey> invalid{{"embeddings","v2","cache_index_global","not-a-number",profile,"hash-x"},{"embeddings","v2","cache_index_global",1,"","hash-y"},{"embeddings","v2","cache_index_global",1,profile,7}};
  for(const auto& key:invalid) { put(key,1); }
  h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return rt::makeReason(rt::JsError("insufficient storage for embeddings cache"));});
  AWAIT(skipped,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-skipped",vector,rt::dateNow(),deadline));EXPECT(!skipped.isNew);for(const auto& key:invalid)EXPECT(row(key));EXPECT(!row(l::embeddingsCacheKey(profile,"hash-skipped")));
  AWAIT(late,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-late",vector,rt::dateNow(),rt::dateNow()-1));EXPECT(!late.isNew);
  const double legacy=rt::dateNow()-30000;put(l::embeddingsCacheGlobalIndexKey(legacy,profile,"hash-legacy"),1);put(l::embeddingsCacheKey(profile,"hash-legacy"),json::object{{"embedding",json::value_from(vector)},{"created_at",rt::Date(legacy).toISOString()}});
  h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return rt::makeReason(rt::JsError("embedded cache quota exceeded"));});
  AWAIT(after,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-after-legacy",vector,rt::dateNow(),deadline));EXPECT(after.isNew);EXPECT(!row(l::embeddingsCacheKey(profile,"hash-legacy")));EXPECT(row(l::embeddingsCacheKey(profile,"hash-after-legacy")));
  co_return;
}
ASYNC_TEST("float embeddings encode to base64 as little-endian float32") {
  h::HarnessScope fixture;EXPECT_EQ(v::floatEmbeddingToBase64({1}),"AACAPw==");EXPECT_EQ(v::floatEmbeddingToBase64({0,-2.5}),"AAAAAAAAIMA=");
  std::vector<double> large;for(int i=0;i<20000;++i)large.push_back(i-10000);const auto bytes=rt::atobBytes(v::floatEmbeddingToBase64(large));EXPECT_EQ(bytes.size(),80000U);
  const auto decode=[&](std::size_t n){std::uint32_t bits=0;for(unsigned i=0;i<4;++i)bits|=static_cast<std::uint32_t>(bytes[n+i])<<(i*8);return std::bit_cast<float>(bits);};EXPECT_EQ(decode(0),-10000.0F);EXPECT_EQ(decode(79996),9999.0F);
  co_return;
}
ASYNC_TEST("embeddings job input records validate, encrypt and decrypt") {
  h::HarnessScope fixture;const std::vector<json::value> invalid{nullptr,json::object{},json::object{{"v",2},{"iv_b64","a"},{"data_b64","b"},{"created_at_ms",1}},json::object{{"v",1},{"iv_b64",""},{"data_b64","b"},{"created_at_ms",1}},json::object{{"v",1},{"iv_b64","a"},{"data_b64","b"}},json::object{{"v",1},{"iv_b64","a"},{"data_b64","b"},{"created_at_ms",-1}}};
  for(const auto& item:invalid) { EXPECT(!v::normalizeEmbeddingsJobInputRecord(item)); }
  const json::value good=json::object{{"v",1},{"iv_b64","a"},{"data_b64","b"},{"created_at_ms",12.7}};EXPECT_EQ(v::normalizeEmbeddingsJobInputRecord(good)->created_at_ms,12);
  AWAIT(encrypted,v::encryptEmbeddingsJobInput("seed-one","secret input text"));EXPECT_EQ(encrypted.v,1);AWAIT(plain,v::decryptEmbeddingsJobInput("seed-one",encrypted));EXPECT_EQ(plain,"secret input text");AWAIT(wrong,v::decryptEmbeddingsJobInput("seed-two",encrypted));EXPECT(!wrong);
  auto bad=encrypted;bad.iv_b64="!!!";bad.data_b64="abc";AWAIT(corrupt,v::decryptEmbeddingsJobInput("seed-one",bad));EXPECT(!corrupt);
  co_return;
}
ASYNC_TEST("voyage transport reports upstream status, payload and abort failures") {
  h::HarnessScope fixture;auto params=transportParams();
  {rt::GlobalFetchOverride fetch(mock([](auto url,auto text,auto headers){EXPECT_EQ(url,"https://api.voyageai.com/v1/embeddings");EXPECT_EQ(headers.get("authorization"),"Bearer voyage-test-key");EXPECT_EQ(rt::getString(rt::jsonParse(text.value()),"input"),"hello");return h::voyageOkResponse(1);}));AWAIT(single,v::fetchVoyageEmbeddings(params));EXPECT_EQ(single.vectors.size(),1U);EXPECT_EQ(single.totalTokens,5);}
  params.inputs={"a","b"};{rt::GlobalFetchOverride fetch(mock([](auto,auto text,auto){EXPECT(rt::getArray(rt::jsonParse(text.value()),"input"));return rt::Response::json(json::object{{"data",json::array{json::object{{"embedding",json::value_from(h::testVector(1024))}},json::object{{"embedding",json::value_from(h::testVector(1024,1))}}}}});}));AWAIT(multiple,v::fetchVoyageEmbeddings(params));EXPECT_EQ(multiple.vectors.size(),2U);EXPECT(!multiple.totalTokens);}
  params.inputs={"hello"};{rt::GlobalFetchOverride fetch(mock([](auto,auto,auto){return rt::Response("rate limited",rt::ResponseInit{.status=429,.headers=rt::Headers{{"Retry-After","2"}}});}));AWAIT(error,rejection(v::fetchVoyageEmbeddings(params)));try{std::rethrow_exception(error);}catch(const v::VoyageEmbeddingsError&e){EXPECT_EQ(e.message,"Voyage embeddings failed (429).");EXPECT_EQ(rt::toNumber(e.status),429);EXPECT_EQ(e.retry_after_ms,2000);}}
  {rt::GlobalFetchOverride fetch(mock([](auto,auto,auto){return rt::Response("rate limited",rt::ResponseInit{.status=503,.headers=rt::Headers{{"Retry-After",rt::Date(rt::dateNow()+3000).toUTCString()}}});}));AWAIT(error,rejection(v::fetchVoyageEmbeddings(params)));try{std::rethrow_exception(error);}catch(const v::VoyageEmbeddingsError&e){EXPECT(e.retry_after_ms&&*e.retry_after_ms>0&&*e.retry_after_ms<=3000);}}
  const std::vector<std::pair<std::string,std::string>> payloads{{"not json at all","Voyage embeddings returned invalid JSON."},{"{\"data\":[{\"embedding\":\"nope\"}]}","Voyage embeddings response missing embedding vector."},{"{\"data\":[{\"embedding\":[1,\"two\"]}]}","Voyage embeddings response contained non-numeric values."}};
  for(const auto&[text,message]:payloads){rt::GlobalFetchOverride fetch(mock([text](auto,auto,auto){return rt::Response(text);}));AWAIT(error,rejection(v::fetchVoyageEmbeddings(params)));EXPECT_EQ(rt::errorMessage(error),message);}
  rt::AbortController cancelled;cancelled.abort(rt::makeReason(rt::JsError("client cancelled")));bool cancel=false,started=false;params.downstreamSignal=cancelled.signal();
  params.beforeProviderDispatch=[&](std::string){uos::api_key_policy::ApiKeyProviderDispatch dispatch{[&]{started=true;},[&]{cancel=true;return rt::fulfilledTask();}};return rt::fulfilledTask(std::optional<uos::api_key_policy::ApiKeyProviderDispatch>(dispatch));};
  AWAIT(error,rejection(v::fetchVoyageEmbeddings(params)));EXPECT_EQ(rt::errorMessage(error),"client cancelled");EXPECT(cancel);EXPECT(!started);
  params=transportParams();params.deadlineMs=rt::dateNow()-1;bool observed=false;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit init){return rt::newPromise<rt::Response>([&,signal=*init.signal](const auto&,const rt::Reject& reject){rt::EventListener listener([&,signal,reject]{observed=true;reject(signal.reason());});if(signal.aborted())listener();else signal.addEventListener("abort",listener);}).wait();}));
  AWAIT(timeout,rejection(v::fetchVoyageEmbeddings(params)));EXPECT_EQ(rt::errorName(timeout),"AbortError");EXPECT(observed);
  co_return;
}
ASYNC_TEST("embedding job create rejects malformed bodies, missing KV and missing provider keys") {
  h::HarnessScope fixture;resetStore();AWAIT(invalid,h::handleEmbeddingsJobCreate(jobRequest("{"),"coverage-token"));EXPECT_EQ(invalid.status(),400);AWAIT(error,errorOf(invalid));EXPECT_EQ(rt::getString(error,"message"),"Invalid JSON body");
  const json::object malformed{{"model","voyage-4-large"},{"input","hello"}};AWAIT(fields,h::handleEmbeddingsJobCreate(jobRequest(malformed),"coverage-token"));EXPECT_EQ(fields.status(),400);
  uos::kv::setKvForTest(nullptr);AWAIT(noKv,h::handleEmbeddingsJobCreate(jobRequest(jobBody()),"coverage-token"));EXPECT_EQ(noKv.status(),503);uos::kv::setKvForTest(h::kvStub);
  h::kvStore.delete_(h::keyToString(l::VOYAGE_API_KEY_KV_KEY));AWAIT(noProvider,h::handleEmbeddingsJobCreate(jobRequest(jobBody()),"coverage-token"));EXPECT_EQ(noProvider.status(),503);
  resetStore();h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return true;});AWAIT(persist,h::handleEmbeddingsJobCreate(jobRequest(jobBody()),"coverage-token"));EXPECT_EQ(persist.status(),502);AWAIT(persistError,errorOf(persist));EXPECT_EQ(rt::getString(persistError,"message"),"Embeddings job could not be persisted.");
  auto none=std::make_shared<FaultKv>("none");uos::kv::setKvForTest(none);AWAIT(readback,h::handleEmbeddingsJobCreate(jobRequest(jobBody()),"coverage-token"));EXPECT_EQ(readback.status(),502);uos::kv::setKvForTest(h::kvStub);
  co_return;
}
ASYNC_TEST("embedding job get reports missing jobs, mismatched profiles and unavailable KV") {
  h::HarnessScope fixture;resetStore();const std::string token="coverage-token",id="embjob_mismatched";
  AWAIT(unknown,h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/embjob_missing"),token,"embjob_missing"));EXPECT_EQ(unknown.status(),404);
  AWAIT(hash,h::sha256Hex(token));put(h::embeddingsJobLookupKey(hash,id),json::object{{"cache_profile_key","profile-lookup"}});put(h::embeddingsJobKey(hash,"profile-lookup",id),json::object{{"id",id},{"cache_profile_key","profile-other"}});
  AWAIT(mismatch,h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/"+id),token,id));EXPECT_EQ(mismatch.status(),404);
  uos::kv::setKvForTest(nullptr);AWAIT(noKv,h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/"+id),token,id));EXPECT_EQ(noKv.status(),503);uos::kv::setKvForTest(h::kvStub);
  co_return;
}
ASYNC_TEST("embedding job polling fails closed when inputs or results disappear") {
  h::HarnessScope fixture;resetStore();h::resetVoyageRateLimit();const std::string token="coverage-inputs";AWAIT(job,queuedJob(token,"missing-input"));AWAIT(hash,h::sha256Hex(token));h::kvStore.delete_(h::keyToString(l::embeddingsJobInputKey(hash,job.profile,job.id,job.hash)));
  AWAIT(missing,poll(job,token));AWAIT(body,missing.json());EXPECT_EQ(rt::getString(body,"status"),"failed");EXPECT_EQ(rt::getString(*rt::get(body,"error"),"code"),"embeddings_job_input_missing");
  resetStore();h::resetVoyageRateLimit();AWAIT(undecryptable,queuedJob(token,"undecryptable"));put(l::embeddingsJobInputKey(hash,undecryptable.profile,undecryptable.id,undecryptable.hash),json::object{{"v",1},{"iv_b64",rt::btoa("0123456789ab")},{"data_b64",rt::btoa("0123456789abcdef0123456789abcdef")},{"created_at_ms",rt::makeNumber(rt::dateNow())}});
  AWAIT(failed,poll(undecryptable,token));AWAIT(failedBody,failed.json());EXPECT_EQ(rt::getString(*rt::get(failedBody,"error"),"code"),"embeddings_job_input_decrypt_failed");
  resetStore();h::resetVoyageRateLimit();rt::GlobalFetchOverride fetch(mock([](auto,auto,auto){return h::voyageOkResponse(1);}));AWAIT(success,h::handleEmbeddingsJobCreate(jobRequest(jobBody("evicted")),token));AWAIT(ok,success.json());EXPECT_EQ(rt::getString(ok,"status"),"succeeded");const std::string id(*rt::getString(ok,"id"));const auto profile=(*row(h::embeddingsJobLookupKey(hash,id)))["cache_profile_key"].asString();const auto stored=*row(h::embeddingsJobKey(hash,profile,id));for(const auto& inputHash:stored["input_hashes"].as<rt::KvArray>())h::kvStore.delete_(h::keyToString(l::embeddingsCacheKey(profile,inputHash.asString())));
  AWAIT(evicted,h::handleEmbeddingsJobGet(rt::Request("https://ai.ubq.fi/uos/embedding-jobs/"+id),token,id));AWAIT(evictedBody,evicted.json());EXPECT_EQ(rt::getString(*rt::get(evictedBody,"error"),"code"),"embeddings_job_result_missing");
  co_return;
}
ASYNC_TEST("embedding job polling reports upstream failures and count mismatches") {
  h::HarnessScope fixture;resetStore();h::resetVoyageRateLimit();const std::string token="coverage-upstream";AWAIT(job,queuedJob(token,json::array{"count-a","count-b"}));
  {rt::GlobalFetchOverride fetch(mock([](auto,auto,auto){return h::voyageOkResponse(1);}));AWAIT(mismatch,poll(job,token));AWAIT(body,mismatch.json());EXPECT_EQ(rt::getString(*rt::get(body,"error"),"code"),"embeddings_job_upstream_mismatch");}
  resetStore();h::resetVoyageRateLimit();AWAIT(rejected,queuedJob(token,"upstream400"));rt::GlobalFetchOverride fetch(mock([](auto,auto,auto){return rt::Response("bad request",rt::ResponseInit{.status=400});}));AWAIT(bad,poll(rejected,token));AWAIT(body,bad.json());EXPECT_EQ(rt::getString(*rt::get(body,"error"),"code"),"embeddings_job_upstream_error");
  co_return;
}
ASYNC_TEST("embedding job polling serves a cached completion and refuses queued jobs without a provider key") {
  h::HarnessScope fixture;resetStore();h::resetVoyageRateLimit();const std::string token="coverage-cached";AWAIT(job,queuedJob(token,"cached"));const auto vector=h::testVector(1024);
  AWAIT(written,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,job.profile,job.hash,vector,rt::dateNow(),rt::dateNow()+30000));EXPECT(written.isNew);AWAIT(served,poll(job,token));AWAIT(body,served.json());EXPECT_EQ(rt::getString(body,"status"),"succeeded");EXPECT(rt::deepEqual(*rt::get(*rt::get(*rt::getArray(*rt::get(body,"result"),"data"),std::size_t{0}),"embedding"),json::value_from(vector)));
  resetStore();h::resetVoyageRateLimit();AWAIT(unconfigured,queuedJob(token,"no-provider"));h::kvStore.delete_(h::keyToString(l::VOYAGE_API_KEY_KV_KEY));AWAIT(noProvider,poll(unconfigured,token));EXPECT_EQ(noProvider.status(),503);
  co_return;
}
ASYNC_TEST("embeddings cache entries are addressed by the resolved profile") {
  h::HarnessScope fixture;const auto profile=h::embeddingsProfileKey("query",512,"base64",false);EXPECT(profile.find("voyage-profile-v2")!=std::string::npos);EXPECT(profile!=h::embeddingsProfileKey("document",512,"base64",false));const json::value vector=json::value_from(h::testVector(512));EXPECT(v::isValidEmbeddingVector(vector,512));EXPECT(!v::isValidEmbeddingVector(vector,1024));auto invalid=vector;invalid.as_array()[0]=rt::makeNumber(std::numeric_limits<double>::quiet_NaN());EXPECT(!v::isValidEmbeddingVector(invalid,512));EXPECT(!v::isValidEmbeddingVector(json::value("nope"),512));
  co_return;
}
ASYNC_TEST("embeddings KV access is memoized for the harness stub") {h::HarnessScope fixture;auto pending=uos::kv::getKv();auto store=co_await pending.wait();EXPECT(store==h::kvStub);  co_return;
}
ASYNC_TEST("embeddings cache writes treat a negative by-hash pointer as a miss") {
  h::HarnessScope fixture;resetStore();const auto profile=h::embeddingsProfileKey();put(l::embeddingsCacheIndexByHashKey(profile,"hash-negative"),-7);AWAIT(result,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-negative",h::testVector(1024),rt::dateNow(),rt::dateNow()+30000));EXPECT(result.isNew);EXPECT(row(l::embeddingsCacheKey(profile,"hash-negative")));
  co_return;
}
ASYNC_TEST("embeddings cache writes abandon a ledger that always rejects commits") {
  h::HarnessScope fixture;resetStore();const auto profile=h::embeddingsProfileKey();AWAIT(initial,v::writeEmbeddingsCacheEntryBestEffort(h::kvStub,profile,"hash-evictable",h::testVector(1024),rt::dateNow()-1000,rt::dateNow()+30000));EXPECT(initial.isNew);const auto stuck=std::make_shared<FaultKv>("throw");AWAIT(result,v::writeEmbeddingsCacheEntryBestEffort(stuck,profile,"hash-stuck",h::testVector(1024),rt::dateNow(),rt::dateNow()+30000));EXPECT(!result.isNew);EXPECT(!row(l::embeddingsCacheKey(profile,"hash-stuck")));EXPECT(row(l::embeddingsCacheKey(profile,"hash-evictable")));
  co_return;
}
ASYNC_TEST("voyage rate limiting tolerates unusable state and a permanently stuck ledger") {
  h::HarnessScope fixture;h::resetVoyageRateLimit();put(l::VOYAGE_RATE_LIMIT_KEY,json::object{{"window_start_ms",1},{"requests","many"},{"tokens",0}});AWAIT(fresh,v::tryReserveVoyageBudget(*h::kvStub,4));EXPECT(fresh.ok);EXPECT_EQ((*row(l::VOYAGE_RATE_LIMIT_KEY))["requests"].asNumber(),1);EXPECT_EQ((*row(l::VOYAGE_RATE_LIMIT_KEY))["tokens"].asNumber(),4);
  h::resetVoyageRateLimit();h::setFailNextAtomicCommit([](auto&,auto&)->h::CommitFailure{return true;});AWAIT(recovered,v::applyVoyageRateLimit(*h::kvStub,2,rt::dateNow()+2000));EXPECT(recovered.ok);h::resetVoyageRateLimit();FaultKv stuck("rate");AWAIT(exhausted,v::applyVoyageRateLimit(stuck,1,rt::dateNow()+200));EXPECT(!exhausted.ok);EXPECT_EQ(exhausted.wait_ms,0);h::resetVoyageRateLimit();
  co_return;
}
ASYNC_TEST("embedding job creation requeues when the rate-limit ledger is stuck") {
  h::HarnessScope fixture;resetStore();h::resetVoyageRateLimit();auto store=std::make_shared<FaultKv>("rate");uos::kv::setKvForTest(store);AWAIT(response,h::handleEmbeddingsJobCreate(jobRequest(jobBody("stuck")),"coverage-stuck"));EXPECT_EQ(response.status(),202);AWAIT(body,response.json());EXPECT_EQ(rt::getString(body,"status"),"queued");EXPECT_EQ(rt::getNumber(body,"retry_after_seconds"),1);uos::kv::setKvForTest(h::kvStub);
  co_return;
}
ASYNC_TEST("voyage transport ignores unusable Retry-After values") {
  h::HarnessScope fixture;const std::vector<std::string> values{"later","   ","-5"};for(const auto& value:values){rt::GlobalFetchOverride fetch(mock([value](auto,auto,auto){return rt::Response("unavailable",rt::ResponseInit{.status=502,.headers=rt::Headers{{"Retry-After",value}}});}));auto params=transportParams();AWAIT(error,rejection(v::fetchVoyageEmbeddings(params)));try{std::rethrow_exception(error);}catch(const v::VoyageEmbeddingsError&e){EXPECT_EQ(rt::toNumber(e.status),502);EXPECT(!e.retry_after_ms);}}
  co_return;
}
ASYNC_TEST("embeddings cache writes give up after repeated create-index CAS losses") {
  h::HarnessScope fixture;resetStore();const auto refusing=std::make_shared<FaultKv>("cache");const auto profile=h::embeddingsProfileKey();AWAIT(result,v::writeEmbeddingsCacheEntryBestEffort(refusing,profile,"hash-refused",h::testVector(1024),rt::dateNow(),rt::dateNow()+30000));EXPECT(!result.isNew);EXPECT(!row(l::embeddingsCacheKey(profile,"hash-refused")));
  co_return;
}
