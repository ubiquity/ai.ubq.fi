#include "tests/helpers/kv_budget_harness.hpp"
#include "uos/kv.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <array>

namespace uos::tests::helpers::kv_budget_harness {
namespace {
  void require(bool condition, const std::string& message) { if (!condition) throw rt::JsError(message, "AssertionError"); }
  json::value fingerprintValue(const rt::KvValue& value, bool bigintSuffix) {
    if (value.isBigInt()) return json::value(value.asBigInt().toString() + (bigintSuffix ? "n" : ""));
    if (value.isU64()) return json::object{{"value", value.asU64().toBigInt().toString() + (bigintSuffix ? "n" : "")}};
    if (value.isArray()) { json::array result; for (const auto& item : value.asArray()) result.push_back(fingerprintValue(item, bigintSuffix)); return result; }
    if (value.isObject()) { json::object result; for (const auto& [key, item] : value.asObject()) if (!item.isUndefined()) rt::objectDefine(result, key, fingerprintValue(item, bigintSuffix)); return result; }
    return rt::jsonParse(rt::kvJsonStringify(value).value_or("null"));
  }
  bool prefix(const rt::KvKey& key, const std::vector<std::string>& expected) { if (key.size() < expected.size()) return false; for (std::size_t index = 0; index < expected.size(); ++index) if (!key[index].isString() || key[index].string() != expected[index]) return false; return true; }
  bool apiKeyV3(const rt::KvKey& key) { return prefix(key, {"uos_ai", "api_key_usage", "v3"}); }
  bool kernelQuota(const rt::KvKey& key) { return prefix(key, {"uos_ai", "kernel_quota", "v2"}); }
  double bytesOf(const rt::KvValue* value) { return value ? static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(fingerprintValue(*value, false)))) : 0; }
  rt::KvKey decodeKey(const std::string& encoded) { const auto value = rt::jsonParse(encoded); return rt::kvKeyFromValue(rt::KvValue(value)); }
  class List final : public rt::KvListIterator {
  public:
    explicit List(std::vector<rt::KvEntry<>> rows_) : rows(std::move(rows_)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override { std::optional<rt::KvEntry<>> result; if (index < rows.size()) result = rows[index++]; co_await rt::microtask(); co_return result; }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { index = rows.size(); co_return; }
  private: std::vector<rt::KvEntry<>> rows; std::size_t index = 0;
  };
  json::object completion(const json::object& usage) { return {{"type", "response.completed"}, {"response", json::object{{"model", std::string(MODEL)}, {"output", json::array{json::object{{"type", "message"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "fixture output"}}}}}}}, {"usage", usage}}}}; }
}
std::string encodeKey(const rt::KvKey& key) { return rt::kvJsonStringify(rt::kvValueFromKey(key)).value_or("null"); }
std::string kvFingerprint(const rt::KvValue& value) { return rt::jsonStringify(fingerprintValue(value, true)); }
std::string encodeBase64Url(const rt::Bytes& bytes) { return rt::toBase64Url(bytes); }
std::string encodeJsonBase64Url(const json::value& value) { return encodeBase64Url(textEncoder.encode(rt::jsonStringify(value))); }
std::string toPublicKeyPem(const rt::Bytes& spki) { const auto encoded = rt::toBase64(spki); std::vector<std::string> lines; for (std::size_t index = 0; index < encoded.size(); index += 64) lines.push_back(encoded.substr(index, 64)); return "-----BEGIN PUBLIC KEY-----\n" + rt::join(lines, "\n") + "\n-----END PUBLIC KEY-----"; }
bool isSumMutation(const CountingKvMutation& mutation) { return mutation.kind == "sum"; }
bool isKernelReservationMutation(const CountingKvMutation& mutation) { return mutation.kind == "set" && kernelQuota(mutation.key) && mutation.key.size() > 3 && rt::endsWith(mutation.key[3].toString(), "reservation") && rt::kvIsObject(mutation.value) && mutation.value["state"].isString() && mutation.value["state"].asString() == "reserved"; }
bool isKernelSettlementMutation(const CountingKvMutation& mutation) { return mutation.kind == "set" && kernelQuota(mutation.key) && mutation.key.size() > 3 && rt::endsWith(mutation.key[3].toString(), "reservation") && rt::kvIsObject(mutation.value) && mutation.value["state"].isString() && (mutation.value["state"].asString() == "committed" || mutation.value["state"].asString() == "released"); }
bool isApiKeyV3DispatchMutation(const CountingKvMutation& mutation) { return mutation.kind == "set" && apiKeyV3(mutation.key) && rt::kvIsObject(mutation.value) && mutation.value["state"].isString() && mutation.value["state"].asString() == "dispatched"; }
std::optional<std::string> CountingKv::versionstamp(const rt::KvKey& key) {
  const auto encoded = encodeKey(key); const auto* value = values.get(encoded); if (!value) return std::nullopt;
  const auto fingerprint = kvFingerprint(*value); auto* existing = versions.get(encoded);
  if (!existing || existing->fingerprint != fingerprint) { const double revision = nextRevision++; versions.set(encoded, Version{fingerprint, revision}); return rt::padStart(rt::numberToString(revision), 20, "0"); }
  return rt::padStart(rt::numberToString(existing->revision), 20, "0");
}
void CountingKv::write(const rt::KvKey& key, const rt::KvValue& value) { const auto encoded = encodeKey(key); values.set(encoded, value); versions.set(encoded, Version{kvFingerprint(value), nextRevision++}); }
void CountingKv::remove(const rt::KvKey& key) { const auto encoded = encodeKey(key); values.delete_(encoded); versions.delete_(encoded); ++nextRevision; }
void CountingKv::resetCounts() { reads = readUnits = writes = sums = sumCommitAttempts = failNextSumCommits = failNextCommits = 0; failKernelQuotaReads = false; kernelQuotaReadGate.reset(); failNextKernelSettlementCommits = 0; onKernelReservationCommit = {}; sumCommitDelayMs = 0; apiKeyV3DispatchCommitGate.reset(); onApiKeyV3DispatchCommit = {}; retries = listCalls = 0; readKeys.clear(); writeKeys.clear(); }
rt::Task<rt::KvEntryMaybe<>> CountingKv::get(rt::KvKey key, rt::KvGetOptions) {
  if (kernelQuotaReadGate && kernelQuota(key)) co_await kernelQuotaReadGate->wait();
  if (failApiKeyV3Reads && apiKeyV3(key)) throw rt::JsError("injected API-key V3 ledger read failure");
  if (failKernelQuotaReads && kernelQuota(key)) throw rt::JsError("injected Kernel quota read failure");
  ++reads; readKeys.push_back(key); const auto* value = values.get(encodeKey(key)); readUnits += rt::Math::max(1, rt::Math::ceil(bytesOf(value) / 4096));
  rt::KvEntryMaybe<> row{.key = key, .value = value && !value->isNullish() ? *value : rt::KvValue(nullptr), .versionstamp = versionstamp(key)};
  co_await rt::microtask(); co_return row;
}
rt::Task<std::vector<rt::KvEntryMaybe<>>> CountingKv::getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions) {
  if (failApiKeyV3Reads && std::any_of(keys.begin(), keys.end(), apiKeyV3)) throw rt::JsError("injected API-key V3 ledger read failure");
  ++reads; readKeys.insert(readKeys.end(), keys.begin(), keys.end()); std::vector<rt::KvEntryMaybe<>> rows;
  for (const auto& key : keys) { const auto* value = values.get(encodeKey(key)); readUnits += rt::Math::max(1, rt::Math::ceil(bytesOf(value) / 4096)); rows.push_back({.key = key, .value = value && !value->isNullish() ? *value : rt::KvValue(nullptr), .versionstamp = versionstamp(key)}); }
  co_await rt::microtask(); co_return rows;
}
rt::Task<rt::KvCommitResult> CountingKv::set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) { write(key, value); ++writes; writeKeys.push_back(key); const rt::KvCommitResult result{.ok = true, .versionstamp = "00000000000000000001"}; co_await rt::microtask(); co_return result; }
rt::Task<void> CountingKv::delete_(rt::KvKey key) { remove(key); ++writes; writeKeys.push_back(key); co_await rt::microtask(); }
std::unique_ptr<rt::KvListIterator> CountingKv::list(rt::KvListSelector selector, rt::KvListOptions) {
  ++listCalls; std::vector<rt::KvEntry<>> rows; for (const auto& [encoded, value] : values) { const auto key = decodeKey(encoded); bool matches = !selector.prefix || selector.prefix->size() <= key.size(); if (selector.prefix) for (std::size_t index = 0; matches && index < selector.prefix->size(); ++index) if (!rt::kvKeyPartStrictEquals((*selector.prefix)[index], key[index])) matches = false; if (matches) rows.push_back({.key = key, .value = value, .versionstamp = "00000000000000000001"}); } return std::make_unique<List>(std::move(rows));
}
class CountingAtomic final : public rt::AtomicOperation {
public:
  explicit CountingAtomic(CountingKv& owner_) : owner(owner_) {}
  rt::AtomicOperation& check(std::vector<rt::AtomicCheck> input) override { if (!input.empty()) checks.push_back(input.front()); return *this; }
  rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) override { mutations.push_back({"set", std::move(key), std::move(value)}); return *this; }
  rt::AtomicOperation& delete_(rt::KvKey key) override { mutations.push_back({"delete", std::move(key)}); return *this; }
  rt::AtomicOperation& sum(rt::KvKey key, const rt::BigInt& value) override { mutations.push_back({"sum", std::move(key), rt::KvValue(value)}); return *this; }
  rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.min is not a function"); }
  rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("operation.max is not a function"); }
  rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("operation.mutate is not a function"); }
  rt::Task<rt::KvCommitResult> commit() override {
    if (owner.failNextCommits > 0) { --owner.failNextCommits; co_await rt::microtask(); co_return rt::KvCommitResult(); }
    if (std::any_of(mutations.begin(), mutations.end(), isKernelReservationMutation) && owner.onKernelReservationCommit) { auto callback = std::move(owner.onKernelReservationCommit); owner.onKernelReservationCommit = {}; callback(); }
    for (const auto& entry : checks) if (owner.versionstamp(entry.key) != entry.versionstamp) { co_await rt::microtask(); co_return rt::KvCommitResult(); }
    if (std::any_of(mutations.begin(), mutations.end(), isSumMutation)) co_await owner.commitSumMutation();
    if (std::any_of(mutations.begin(), mutations.end(), isKernelSettlementMutation) && owner.failNextKernelSettlementCommits > 0) { --owner.failNextKernelSettlementCommits; throw rt::JsError("injected Kernel quota settlement failure"); }
    if (std::any_of(mutations.begin(), mutations.end(), isApiKeyV3DispatchMutation)) { if (owner.onApiKeyV3DispatchCommit) owner.onApiKeyV3DispatchCommit(); if (owner.apiKeyV3DispatchCommitGate) co_await owner.apiKeyV3DispatchCommitGate->wait(); }
    for (const auto& mutation : mutations) owner.applyMutation(mutation);
    const rt::KvCommitResult result{.ok = true, .versionstamp = "00000000000000000001"}; co_await rt::microtask(); co_return result;
  }
private: CountingKv& owner; std::vector<rt::AtomicCheck> checks; std::vector<CountingKvMutation> mutations;
};
std::unique_ptr<rt::AtomicOperation> CountingKv::atomic() { return std::make_unique<CountingAtomic>(*this); }
void CountingKv::close() {}
rt::Task<void> CountingKv::commitSumMutation() { ++sumCommitAttempts; if (failNextSumCommits > 0) { --failNextSumCommits; throw rt::JsError("injected API-key usage sum failure"); } if (sumCommitDelayMs > 0) co_await rt::sleep(sumCommitDelayMs); }
void CountingKv::applyMutation(const CountingKvMutation& mutation) { if (mutation.kind == "delete") remove(mutation.key); else if (mutation.kind == "set") write(mutation.key, mutation.value); else { const auto* current = values.get(encodeKey(mutation.key)); const auto before = current ? current->asU64().toBigInt() : rt::BigInt(0); write(mutation.key, rt::KvValue(rt::KvU64(before + mutation.value.asBigInt()))); ++sums; } ++writes; writeKeys.push_back(mutation.key); }
std::shared_ptr<CountingKv> kv = std::make_shared<CountingKv>();
double now() { static const double value = rt::dateNow(); return value; }
const json::object& runtime() { static const json::object value{{"version", 2}, {"default_model", std::string(MODEL)}, {"default_reasoning_effort", "medium"}, {"codex_models", json::object{{"source", "chatgpt_codex"}, {"client_version", "0.150.0"}, {"updated_at_ms", now()}, {"models", json::array{json::object{{"slug", std::string(MODEL)}, {"default_reasoning_level", "medium"}, {"supported_reasoning_levels", json::array{"none", "low", "medium", "high"}}}}}}}, {"updated_at_ms", now()}}; return value; }
json::object codexAuthPool(double accountCount) { json::array accounts; for (double index = 0; index < rt::Math::max(0, rt::Math::floor(accountCount)); ++index) { const auto suffix = rt::numberToString(index + 1); accounts.push_back(json::object{{"access_token", "access-" + suffix}, {"refresh_token", "refresh-" + suffix}, {"account_id", "acct-" + suffix}, {"updated_at_ms", rt::dateNow()}}); } return {{"accounts", accounts}, {"updated_at_ms", rt::dateNow()}}; }
rt::Task<SeedKeyResult> seedKey(std::string token, std::string id, double limit) {
  const auto hash = co_await utils::sha256Base64Url(token); json::object record{{"id", id}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", limit}, {"usage_requests", 0}, {"usage_reset_at_ms", now() + 60000}, {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}}; kv->values.set(encodeKey(rt::KvKey{"ubq_ai", "api_keys", "hash", hash}), record); co_return SeedKeyResult{hash, record};
}
rt::Task<SeedKeyResult> seedPaidFallbackKey(std::string token, std::string id) {
  const auto hash = co_await utils::sha256Base64Url(token); json::object policy{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", -1}, {"usage_requests", 0}, {"usage_reset_at_ms", rt::dateNow() + 60000}, {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_microcredits", 1000000}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}}; json::object record{{"id", id}}; rt::objectAssign(record, policy); kv->values.set(encodeKey(rt::KvKey{"ubq_ai", "api_keys", "hash", hash}), record); json::object idRecord{{"id", id}, {"name", "First fallback quota"}, {"prefix", rt::slice(token, 0, 12)}, {"hash", hash}, {"created_at_ms", rt::dateNow()}}; rt::objectAssign(idRecord, policy); idRecord["paid_fallback_model_ids"] = json::array{std::string(MODEL)}; idRecord["paid_fallback_quota_per_credit"] = 500000; idRecord["paid_fallback_max_exposure_microcredits"] = json::object{{std::string(MODEL), 250000}}; idRecord["paid_fallback_pricing_checked_at_ms"] = rt::dateNow(); kv->values.set(encodeKey(rt::KvKey{"ubq_ai", "api_keys", "id", id}), idRecord); co_return SeedKeyResult{hash, record};
}
rt::Request request(const std::string& token) { return rt::Request("https://ai.ubq.fi/v1/responses", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(json::object{{"input", "ping"}}))}); }
rt::Request streamingRequest(const std::string& token, const std::string& route) { const auto body = route == "responses" ? json::object{{"input", "ping"}, {"stream", true}} : json::object{{"messages", json::array{json::object{{"role", "user"}, {"content", "ping"}}}}, {"stream", true}}; return rt::Request(route == "responses" ? "https://ai.ubq.fi/v1/responses" : "https://ai.ubq.fi/v1/chat/completions", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(body))}); }
std::string fetchInputUrl(const rt::RequestInfo& input) { return std::visit([](const auto& value) -> std::string { using T = std::decay_t<decltype(value)>; if constexpr (std::is_same_v<T, std::string>) return value; else if constexpr (std::is_same_v<T, rt::URL>) return value.href(); else return value.url(); }, input); }
std::string completedSseEvent(double inputTokens, double outputTokens) { return "data: " + rt::jsonStringify(completion(json::object{{"input_tokens", inputTokens}, {"output_tokens", outputTokens}, {"total_tokens", inputTokens + outputTokens}})) + "\n\n"; }
std::string semanticSseEvent(const std::string& text) { return "data: " + rt::jsonStringify(json::object{{"type", "response.output_text.delta"}, {"delta", text}}) + "\n\n"; }
rt::Response sse() { return sse(json::object{{"input_tokens", 1}, {"output_tokens", 1}, {"total_tokens", 2}}); }
rt::Response sse(const json::object& usage) { return rt::Response("data: " + rt::jsonStringify(completion(usage)) + "\n\n", rt::ResponseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}}}); }
rt::Response authoritativeCodexQuotaResponse() { return rt::Response(rt::jsonStringify(json::object{{"error", json::object{{"message", "Primary limited"}, {"type", "usage_limit_reached"}}}}), rt::ResponseInit{.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}, {"Retry-After", rt::Date((rt::Math::floor(rt::dateNow() / 1000) + 60) * 1000).toUTCString()}}}); }
Deferred deferred() { auto [promise, resolve, reject] = rt::withResolvers<void>(); return {promise, reject, resolve}; }
rt::Task<void> waitFor(std::function<bool()> predicate, std::string label, double timeoutMs) { const auto deadline = rt::performanceNow() + timeoutMs; while (!predicate()) { if (rt::performanceNow() >= deadline) throw rt::JsError("Timed out waiting for " + label); co_await rt::sleep(5); } }
json::object usageWindow(const authority::native_contracts::ApiKeyPolicy& policy) { const auto* value = kv->values.get(encodeKey(api_key_policy::apiKeyUsageV3WindowKey(policy))); require(value != nullptr && value->truthy(), "V3 aggregate must exist"); return {{"committed_requests", value->operator[]("committed_requests").toJson()}, {"reserved_requests", value->operator[]("reserved_requests").toJson()}, {"window_reset_at_ms", value->operator[]("window_reset_at_ms").toJson()}}; }
rt::Task<PreparedApiKeyInference> prepareApiKeyInference(std::string tokenDigit, std::string keyId, double limit) { kv->values.clear(); api_key_policy::resetApiKeyPolicyCacheForTest(); runtime_config::resetRuntimeConfigCacheForTest(); codex::auth::resetCodexAuthCacheForTest(); kv->values.set(encodeKey(runtime_config::RUNTIME_CONFIG_V2_KEY), runtime()); kv->values.set(encodeKey(rt::KvKey{"ubq_ai", "codex_auth"}), codexAuthPool()); const auto token = "u_" + rt::repeat(tokenDigit, 64); auto task = seedKey(token, keyId, limit); auto seeded = co_await std::move(task); const auto policy = api_key_policy::apiKeyPolicyFromHashRecord(seeded.hash, json::value(seeded.record), rt::dateNow()); require(policy.has_value(), "API key policy must exist"); PreparedApiKeyInference result; result.hash = seeded.hash; result.record = seeded.record; result.token = token; result.policy = *policy; co_return result; }
const rt::crypto::CryptoKeyPair& kernelTestKeyPair() { static const auto key = rt::crypto::subtle::generateKey(rt::crypto::Algorithm{.name = "RSASSA-PKCS1-v1_5", .hash = "SHA-256", .modulusLength = 2048, .publicExponent = rt::Bytes{1, 0, 1}}, true, {"sign", "verify"}); return key; }
rt::Task<void> seedKernelTestPublicKey() { const auto publicKey = rt::crypto::subtle::exportKey("spki", kernelTestKeyPair().publicKey); co_await rt::microtask(); kv->values.set(encodeKey(rt::KvKey{"uos_ai", "kernel_pubkeys"}), json::array{json::object{{"pem", toPublicKeyPem(publicKey)}}}); }
rt::Task<std::string> makeKernelTestToken(std::string apiToken, std::string owner, std::string repo) { const auto seconds = rt::Math::floor(rt::dateNow() / 1000); const auto header = encodeJsonBase64Url(json::object{{"alg", "RS256"}, {"typ", "JWT"}}); const auto jti = "jti_" + rt::crypto::randomUUID(); const auto hash = co_await utils::sha256Base64Url(apiToken); const auto payload = encodeJsonBase64Url(json::object{{"iss", "ubiquity-os-kernel"}, {"aud", "ai.ubq.fi"}, {"iat", seconds}, {"exp", seconds + 600}, {"jti", jti}, {"owner", owner}, {"repo", repo}, {"installation_id", nullptr}, {"auth_token_sha256", hash}, {"state_id", "state_" + rt::crypto::randomUUID()}}); const auto signingInput = header + "." + payload; const auto signature = rt::crypto::subtle::sign("RSASSA-PKCS1-v1_5", kernelTestKeyPair().privateKey, textEncoder.encode(signingInput)); co_await rt::microtask(); co_return signingInput + "." + encodeBase64Url(signature); }
rt::Task<rt::Request> withKernelTestToken(rt::Request req, std::string apiToken, std::string owner, std::string repo) { auto headers = req.headers(); const auto token = co_await makeKernelTestToken(apiToken, owner, repo); headers.set("X-Ubiquity-Kernel-Token", token); const rt::RequestInit init{.headers = headers}; co_return rt::Request(req, init); }
void seedKernelDefaultLimit(double limit, double windowMs) { kv->values.set(encodeKey(defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY), rt::KvValue(limit)); kv->values.set(encodeKey(defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY), rt::KvValue(windowMs)); }
rt::Response validStreamingSse() { return rt::Response("data: " + rt::jsonStringify(json::object{{"type", "response.created"}, {"response", json::object{{"id", rt::crypto::randomUUID()}}}}) + "\n\n" + semanticSseEvent() + completedSseEvent(), rt::ResponseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}}}); }
double requiredTerminalTiming(const json::object& terminal, const std::string& field) { const auto value = rt::getNumber(terminal, field); require(value.has_value(), field + " must be a number"); require(rt::Number::isFinite(*value), field + " must be finite"); require(*value >= 0, field + " must be nonnegative"); return *value; }
void assertOrderedTerminalTimings(const json::object& terminal, bool expectsDownstreamDrain) { std::vector<double> ordered; for (const auto* field : {"first_codex_dispatch_ms", "first_codex_headers_ms", "first_upstream_sse_event_ms", "first_semantic_commitment_ms", "stream_terminal_ms", "latency_ms"}) ordered.push_back(requiredTerminalTiming(terminal, field)); for (std::size_t index = 1; index < ordered.size(); ++index) require(ordered[index - 1] <= ordered[index], "terminal timing fields must be ordered"); if (!expectsDownstreamDrain) { const auto* value = rt::get(terminal, "downstream_drain_ms"); require(value && value->is_null(), "downstream_drain_ms must be null"); return; } const auto drain = requiredTerminalTiming(terminal, "downstream_drain_ms"); require(ordered[4] + drain <= ordered[5], "terminal drain must finish within latency"); }
void installHarness() { provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {}); paid_fallback::index::setPaidFallbackTerminalSweepForTest([] {}); uos::kv::setKvForTest(kv); }
void resetHarness() { kv->values.clear(); installHarness(); }
void cleanupHarness() { provider::capacity_events::setProviderCapacitySampleTriggerForTest(nullptr); paid_fallback::index::setPaidFallbackTerminalSweepForTest(nullptr); uos::kv::setKvForTest(nullptr); }
}
