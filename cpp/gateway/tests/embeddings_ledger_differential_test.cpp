// Differential cases for src/embeddings/ledger.ts (its TS tests reach it only through the unported
// voyage/jobs/handlers modules): the idempotency ledger driven step by step over an in-memory Kv with
// injectable faults (get/set/commit throwing, commits rejected), checking each step's result, the stored
// rows and their expiry, console.error calls and get counts; plus fingerprints, control characters, the
// fixed responses, keys and constants. Expected values were recorded from Bun 1.4.0 with the same fake
// Kv written in TS (tests/embeddings_ledger_differential_vectors.inc). The last case runs over
// rt::MemoryKv (the native store's limits) and is not a recording.
// Links: tests/embeddings_ledger_differential_test.cpp src/embeddings/ledger.cpp src/http.cpp src/utils.cpp src/auth/relay.cpp
//        src/config.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp
#include "uos/embeddings/ledger.hpp"

#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ledger = uos::embeddings::ledger;
  using ledger::EmbeddingsIdempotencyLease;

#include "embeddings_ledger_differential_vectors.inc"

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(kEmbeddingsLedgerVectors).as_object();
    return parsed;
  }

  // Special numbers are tagged {"$num": text}.
  json::value untag(const json::value& value) {
    if (const json::array* items = value.if_array()) {
      json::array out;
      for (const json::value& item : *items) out.push_back(untag(item));
      return out;
    }
    if (const json::object* object = value.if_object()) {
      if (object->size() == 1) {
        if (const json::value* number = rt::get(*object, "$num"); number && number->is_string()) return rt::makeNumber(rt::stringToNumber(std::string(number->get_string())));
      }
      json::object out;
      for (const auto& member : *object) out[member.key()] = untag(member.value());
      return out;
    }
    return value;
  }

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.push_back(json::value(part.string()));
      else out.push_back(rt::makeNumber(part.number()));
    }
    return out;
  }

  // Strings longer than 200 code units as their length and UTF-8 SHA-256 (createHash().update(text)).
  json::value summarize(const json::value& value) {
    if (value.is_string()) {
      const std::string text(value.get_string());
      if (rt::length(text) > 200) {
        json::object out;
        out["$len"] = rt::makeNumber(static_cast<double>(rt::length(text)));
        out["$sha256"] = rt::crypto::createHash("sha256").update(rt::encodeUtf8(text)).digest("hex");
        return out;
      }
    }
    return value;
  }

  // The oracle's FakeKv: rows keyed by JSON.stringify(key), structured clones in and out, no
  // native size limits (rt::MemoryKv enforces those; see the last test), injectable faults and a
  // get counter. Expiry is `Date.now() + expireIn` at the write.
  class FakeKv final : public rt::Kv {
  public:
    explicit FakeKv(const double& clock) : clock_(clock) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
      gets += 1;
      if (failGet) throw rt::JsError("kv get failed");
      rt::KvEntryMaybe<> entry;
      entry.key = key;
      entry.value = rt::KvValue(nullptr);
      if (const Row* row = rows_.get(rt::jsonStringify(keyJson(key)))) {
        entry.value = rt::kvStructuredClone(row->value);
        entry.versionstamp = row->versionstamp;
      }
      return rt::fulfilledTask(std::move(entry));
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override { throw std::logic_error("FakeKv.getMany is not used"); }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options) override {
      if (failSet) throw rt::JsError("kv set failed");
      write(key, value, options.expireIn, stamp());
      return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = ""});
    }
    rt::Task<void> delete_(rt::KvKey key) override {
      rows_.delete_(rt::jsonStringify(keyJson(key)));
      return rt::fulfilledTask();
    }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions) override { throw std::logic_error("FakeKv.list is not used"); }
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      return std::make_unique<rt::GenericAtomicOperation>([this](std::vector<rt::AtomicCheck> checks, std::vector<rt::KvMutation> mutations) -> rt::Task<rt::KvCommitResult> {
        if (failCommit) throw rt::JsError("kv commit failed");
        if (rejectCommits) return rt::fulfilledTask(rt::KvCommitResult{});
        for (const rt::AtomicCheck& check : checks) {
          const Row* row = rows_.get(rt::jsonStringify(keyJson(check.key)));
          const std::optional<std::string> current = row ? std::optional<std::string>(row->versionstamp) : std::nullopt;
          if (current != check.versionstamp) return rt::fulfilledTask(rt::KvCommitResult{});
        }
        const std::string versionstamp = stamp();
        for (const rt::KvMutation& mutation : mutations) {
          if (mutation.type == rt::KvMutationType::set) write(mutation.key, mutation.value, mutation.expireIn, versionstamp);
          else rows_.delete_(rt::jsonStringify(keyJson(mutation.key)));
        }
        return rt::fulfilledTask(rt::KvCommitResult{.ok = true, .versionstamp = versionstamp});
      });
    }
    void close() override {}

    // The rows as [key, summarized value, expiry or null], sorted by JSON.stringify(key).
    json::value rows() const {
      std::vector<std::pair<std::string, json::value>> sorted;
      for (const auto& [text, row] : rows_) {
        json::array item;
        item.push_back(keyJson(row.key));
        item.push_back(summarize(row.value.toJson()));
        item.push_back(row.expiresAt ? rt::makeNumber(*row.expiresAt) : json::value(nullptr));
        sorted.emplace_back(text, json::value(std::move(item)));
      }
      std::stable_sort(sorted.begin(), sorted.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
      json::array out;
      for (auto& entry : sorted) out.push_back(std::move(entry.second));
      return out;
    }

    double gets = 0;
    bool failGet = false;
    bool failSet = false;
    bool failCommit = false;
    bool rejectCommits = false;

  private:
    struct Row {
      rt::KvKey key;
      rt::KvValue value;
      std::string versionstamp;
      std::optional<double> expiresAt;
    };
    std::string stamp() {
      version_ += 1;
      return rt::formatVersionstamp(version_);
    }
    void write(const rt::KvKey& key, const rt::KvValue& value, std::optional<double> expireIn, std::string versionstamp) {
      std::optional<double> expiresAt;
      if (expireIn) expiresAt = clock_ + *expireIn;
      rows_.set(rt::jsonStringify(keyJson(key)), Row{key, rt::kvStructuredClone(value), std::move(versionstamp), expiresAt});
    }

    const double& clock_;
    std::uint64_t version_ = 0;
    rt::OrderedMap<std::string, Row> rows_;
  };

  rt::Task<json::value> responseJson(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    const std::string body = co_await response.text();
    json::object out;
    out["status"] = response.status();
    out["headers"] = std::move(headers);
    out["body"] = summarize(json::value(body));
    co_return json::value(std::move(out));
  }

  json::value leaseJson(const EmbeddingsIdempotencyLease& lease) {
    json::object out;
    out["key"] = keyJson(lease.key);
    out["responseKeyPrefix"] = keyJson(lease.responseKeyPrefix);
    out["fingerprint"] = lease.fingerprint;
    out["ownerRequestId"] = lease.ownerRequestId;
    return out;
  }

  // Each step's record, compared with the oracle's as it is taken.
  struct Steps {
    std::shared_ptr<FakeKv> kv;
    std::shared_ptr<rt::console::Capture> capture;
    double* clock = nullptr;
    std::size_t next = 0;
    json::value lastRows = json::array();

    void record(const std::string& label, const json::value& output) {
      const json::array& expectedSteps = vectors().at("steps").as_array();
      ASSERT(next < expectedSteps.size());
      const json::value expected = untag(expectedSteps[next]);
      next += 1;
      EXPECT_EQ(label, std::string(expected.at("label").as_string()));
      if (!rt::deepEqual(output, expected.at("output"))) EXPECT_EQ(label + " " + rt::jsonStringify(output), label + " " + rt::jsonStringify(expected.at("output")));
      const json::value& expectedRows = expected.at("rows");
      if (!expectedRows.is_string()) lastRows = expectedRows;
      const json::value actualRows = kv->rows();
      if (!rt::deepEqual(actualRows, lastRows)) EXPECT_EQ(label + " rows " + rt::jsonStringify(actualRows), label + " rows " + rt::jsonStringify(lastRows));
      json::array errors;
      for (const rt::console::Call& call : capture->calls()) {
        const rt::console::ErrorInfo& error = call.args.at(1).errorInfo();
        errors.push_back(json::array{json::value(call.args.at(0).string()), json::value(error.name + ": " + error.message)});
      }
      capture->clear();
      if (!rt::deepEqual(json::value(errors), expected.at("errors"))) EXPECT_EQ(label + " errors " + rt::jsonStringify(json::value(errors)), label + " errors " + rt::jsonStringify(expected.at("errors")));
      EXPECT_EQ(label + " gets " + rt::numberToString(kv->gets), label + " gets " + rt::numberToString(rt::getNumber(expected, "gets").value()));
      kv->gets = 0;
      EXPECT_EQ(label + " now " + rt::numberToString(*clock), label + " now " + rt::numberToString(rt::getNumber(expected, "now").value()));
    }
  };

  std::string repeat(std::string_view text, std::size_t count) {
    std::string out;
    out.reserve(text.size() * count);
    for (std::size_t index = 0; index < count; ++index) out += text;
    return out;
  }

  const std::string FP1 = repeat("f", 64);
  const std::string FP2 = repeat("e", 64);

  rt::Response makeResponse(std::string body, std::optional<double> status = std::nullopt, std::optional<std::string> contentType = std::nullopt) {
    rt::ResponseInit init;
    init.status = status;
    if (contentType) init.headers = rt::Headers{{"Content-Type", *contentType}};
    return rt::Response(rt::BodyInit(std::move(body)), std::move(init));
  }

  // `{ v: 1, fingerprint: FP1, state: "reserved", owner_request_id: "old", ..., ...o }`
  json::object storedRecord(const json::object& overrides) {
    json::object out;
    out["v"] = 1;
    out["fingerprint"] = FP1;
    out["state"] = "reserved";
    out["owner_request_id"] = "old";
    out["created_at_ms"] = 1;
    out["updated_at_ms"] = 2;
    out["lease_until_ms"] = nullptr;
    out["response_status"] = nullptr;
    out["response_content_type"] = nullptr;
    out["response_generation"] = nullptr;
    out["response_chunk_count"] = nullptr;
    out["response_sha256"] = nullptr;
    for (const auto& member : overrides) out[member.key()] = member.value();
    return out;
  }

  rt::Task<rt::KvKey> ledgerKey(std::string principal, std::string idempotencyKey) {
    const std::string principalHash = co_await uos::utils::sha256Hex("uos-embeddings-principal-v1:" + principal);
    const std::string keyHash = co_await uos::utils::sha256Hex("uos-embeddings-key-v1:" + idempotencyKey);
    co_return rt::KvKey{"embeddings", "idempotency", "v1", principalHash, keyHash};
  }

  rt::Task<rt::KvKey> responsePrefix(std::string principal, std::string idempotencyKey) {
    const std::string principalHash = co_await uos::utils::sha256Hex("uos-embeddings-principal-v1:" + principal);
    const std::string keyHash = co_await uos::utils::sha256Hex("uos-embeddings-key-v1:" + idempotencyKey);
    co_return rt::KvKey{"embeddings", "idempotency", "v1", "response", principalHash, keyHash};
  }

  std::string sha(const std::string& text) { return rt::crypto::createHash("sha256").update(rt::encodeUtf8(text)).digest("hex"); }
}

ASYNC_TEST("differential: the idempotency ledger over a Kv with faults matches Bun step by step") {
  double clock = 1'800'000'000'000;
  rt::globalDateNow() = [&clock] { return clock; };
  const auto kv = std::make_shared<FakeKv>(clock);
  Steps steps{.kv = kv, .capture = std::make_shared<rt::console::Capture>(std::vector<rt::console::Method>{rt::console::Method::error}), .clock = &clock};
  std::map<std::string, EmbeddingsIdempotencyLease> leases;

  // The step helpers take lease names (a lease is an aggregate: never an argument inside co_await).
  const auto acquire = [&](std::string name, std::string principal, std::string idempotencyKey, std::string fingerprint, std::string requestId) -> rt::Task<void> {
    const ledger::AcquireEmbeddingsIdempotencyLeaseParams params{.kv = kv, .principal = principal, .idempotencyKey = idempotencyKey, .fingerprint = fingerprint, .requestId = requestId};
    auto acquiring = ledger::acquireEmbeddingsIdempotencyLease(params);
    ledger::EmbeddingsIdempotencyAcquireResult result = co_await std::move(acquiring);
    json::object output;
    if (const auto* acquired = std::get_if<ledger::EmbeddingsIdempotencyAcquireResultAcquired>(&result)) {
      leases[name] = acquired->lease;
      output["kind"] = std::string(acquired->kind);
      output["lease"] = leaseJson(acquired->lease);
    } else if (const auto* replay = std::get_if<ledger::EmbeddingsIdempotencyAcquireResultReplay>(&result)) {
      output["kind"] = std::string(replay->kind);
      auto describing = responseJson(replay->response);
      output["response"] = co_await std::move(describing);
    } else {
      const auto& error = std::get<ledger::EmbeddingsIdempotencyAcquireResultError>(result);
      output["kind"] = std::string(error.kind);
      auto describing = responseJson(error.response);
      output["response"] = co_await std::move(describing);
    }
    steps.record("acquire " + name, json::value(std::move(output)));
  };
  const auto dispatch = [&](std::string label, std::string lease) -> rt::Task<void> {
    auto marking = ledger::markEmbeddingsIdempotencyDispatched(leases.at(lease));
    const bool marked = co_await std::move(marking);
    steps.record(label, json::value(marked));
  };
  const auto release = [&](std::string label, std::string lease, bool allowDispatched) -> rt::Task<void> {
    auto releasing = ledger::releaseEmbeddingsIdempotencyReservation(leases.at(lease), allowDispatched);
    const bool released = co_await std::move(releasing);
    steps.record(label, json::value(released));
  };
  const auto indeterminate = [&](std::string label, std::string lease) -> rt::Task<void> {
    auto marking = ledger::markEmbeddingsIdempotencyIndeterminate(leases.at(lease));
    co_await std::move(marking);
    steps.record(label, json::value(nullptr));
  };
  const auto storeSuccess = [&](std::string label, std::string lease, rt::Response response) -> rt::Task<void> {
    auto storing = ledger::storeEmbeddingsIdempotencySuccess(leases.at(lease), response);
    const bool stored = co_await std::move(storing);
    steps.record(label, json::value(stored));
  };
  const auto seed = [&](rt::KvKey key, json::value value) -> rt::Task<void> {
    const rt::KvValue stored(value);
    const rt::KvSetOptions noExpiry;
    co_await kv->set(key, stored, noExpiry);
  };

  const std::string bigBody = repeat("a", 47'999) + "\xF0\x9F\x98\x80" + repeat("b", 50'000) + "\xC3\xA9" + repeat("c", 5);

  co_await acquire(std::string("A"), std::string("principal-a"), std::string("key-1"), FP1, std::string("req-1"));
  clock += 1'000;
  co_await acquire(std::string("A busy"), std::string("principal-a"), std::string("key-1"), FP1, std::string("req-2"));
  co_await acquire(std::string("A conflict"), std::string("principal-a"), std::string("key-1"), FP2, std::string("req-3"));
  co_await dispatch(std::string("dispatch A"), std::string("A"));
  co_await dispatch(std::string("dispatch A again"), std::string("A"));
  clock += 61'000;
  co_await acquire(std::string("A retire"), std::string("principal-a"), std::string("key-1"), FP1, std::string("req-4"));
  co_await acquire(std::string("A indeterminate"), std::string("principal-a"), std::string("key-1"), FP1, std::string("req-5"));
  co_await indeterminate(std::string("indeterminate A no-op"), std::string("A"));
  co_await release(std::string("release A"), std::string("A"), true);

  co_await acquire(std::string("B"), std::string("principal-b"), std::string("key-2"), FP1, std::string("req-b1"));
  co_await dispatch(std::string("dispatch B"), std::string("B"));
  co_await storeSuccess(std::string("store B"), std::string("B"), makeResponse(bigBody, 200, std::string("application/json; charset=utf-8")));
  clock += 1'000;
  co_await acquire(std::string("B replay"), std::string("principal-b"), std::string("key-2"), FP1, std::string("req-b2"));
  {
    auto prefixing = responsePrefix(std::string("principal-b"), std::string("key-2"));
    const rt::KvKey prefix = co_await std::move(prefixing);
    const rt::KvKey chunk{prefix, std::string("req-b1"), 1};
    co_await kv->delete_(chunk);
    steps.record("drop B chunk", json::value(nullptr));
  }
  co_await acquire(std::string("B missing chunk"), std::string("principal-b"), std::string("key-2"), FP1, std::string("req-b3"));

  co_await acquire(std::string("C"), std::string("principal-c"), std::string("key-3"), FP1, std::string("req-c1"));
  co_await storeSuccess(std::string("store C empty"), std::string("C"), makeResponse(std::string("")));
  co_await acquire(std::string("C replay"), std::string("principal-c"), std::string("key-3"), FP1, std::string("req-c2"));

  co_await acquire(std::string("D"), std::string("principal-d"), std::string("key-4"), FP1, std::string("req-d1"));
  co_await release(std::string("release D"), std::string("D"), false);
  co_await acquire(std::string("D2"), std::string("principal-d"), std::string("key-4"), FP1, std::string("req-d2"));
  co_await dispatch(std::string("dispatch D2"), std::string("D2"));
  co_await release(std::string("release D2 reserved only"), std::string("D2"), false);
  co_await release(std::string("release D2 dispatched"), std::string("D2"), true);

  co_await acquire(std::string("E"), std::string("principal-e"), std::string("key-5"), FP1, std::string("req-e1"));
  co_await indeterminate(std::string("indeterminate E"), std::string("E"));
  leases["E wrong"] = leases.at("E");
  leases.at("E wrong").fingerprint = FP2;
  co_await indeterminate(std::string("indeterminate wrong fingerprint"), std::string("E wrong"));
  co_await storeSuccess(std::string("store E indeterminate"), std::string("E"), makeResponse(std::string("{}"), std::nullopt, std::string("application/json")));

  auto keyingF = ledgerKey(std::string("principal-f"), std::string("key-6"));
  const rt::KvKey keyF = co_await std::move(keyingF);
  auto prefixingF = responsePrefix(std::string("principal-f"), std::string("key-6"));
  const rt::KvKey prefixF = co_await std::move(prefixingF);
  leases["F"] = EmbeddingsIdempotencyLease{.kv = kv, .key = keyF, .responseKeyPrefix = prefixF, .fingerprint = FP1, .ownerRequestId = "req-f"};
  {
    json::object malformed;
    malformed["v"] = 1;
    malformed["fingerprint"] = FP1;
    malformed["state"] = "bogus";
    const json::value value(std::move(malformed));
    co_await seed(keyF, value);
    steps.record("seed F malformed", json::value(nullptr));
  }
  co_await acquire(std::string("F"), std::string("principal-f"), std::string("key-6"), FP1, std::string("req-f1"));
  co_await dispatch(std::string("dispatch F"), std::string("F"));
  co_await release(std::string("release F"), std::string("F"), true);
  co_await indeterminate(std::string("indeterminate F"), std::string("F"));

  {
    json::object overrides;
    overrides["created_at_ms"] = 1.7;
    overrides["updated_at_ms"] = -0.5;
    overrides["lease_until_ms"] = 5.5;
    overrides["response_status"] = nullptr;
    overrides["response_content_type"] = "text/plain";
    overrides["extra"] = "dropped";
    const json::value value = rt::canonicalNumbers(json::value(storedRecord(overrides)));
    auto keying = ledgerKey(std::string("principal-g"), std::string("key-7"));
    const rt::KvKey key = co_await std::move(keying);
    co_await seed(key, value);
    steps.record("seed G truncation", json::value(nullptr));
  }
  co_await acquire(std::string("G"), std::string("principal-g"), std::string("key-7"), FP1, std::string("req-g1"));
  {
    json::object overrides;
    overrides["response_chunk_count"] = 0;
    const json::value value(storedRecord(overrides));
    auto keying = ledgerKey(std::string("principal-g"), std::string("key-8"));
    const rt::KvKey key = co_await std::move(keying);
    co_await seed(key, value);
    steps.record("seed G2 chunk count zero", json::value(nullptr));
  }
  co_await acquire(std::string("G2"), std::string("principal-g"), std::string("key-8"), FP1, std::string("req-g2"));
  {
    json::object overrides;
    overrides["state"] = "dispatched";
    overrides["lease_until_ms"] = rt::makeNumber(clock + 5);
    overrides["response_status"] = 2.5;
    const json::value value(storedRecord(overrides));
    auto keying = ledgerKey(std::string("principal-g"), std::string("key-9"));
    const rt::KvKey key = co_await std::move(keying);
    co_await seed(key, value);
    steps.record("seed G3 numbers", json::value(nullptr));
  }
  co_await acquire(std::string("G3"), std::string("principal-g"), std::string("key-9"), FP1, std::string("req-g3"));
  {
    json::object overrides;
    overrides["state"] = "dispatched";
    overrides["lease_until_ms"] = rt::makeNumber(clock + 5);
    overrides["response_status"] = 201;
    const json::value value(storedRecord(overrides));
    auto keying = ledgerKey(std::string("principal-g"), std::string("key-10"));
    const rt::KvKey key = co_await std::move(keying);
    co_await seed(key, value);
    steps.record("seed G4 busy", json::value(nullptr));
  }
  co_await acquire(std::string("G4"), std::string("principal-g"), std::string("key-10"), FP1, std::string("req-g4"));

  {
    const auto succeeded = [](double status, std::string contentType, double chunks, std::string digest) {
      json::object overrides;
      overrides["state"] = "succeeded";
      overrides["owner_request_id"] = nullptr;
      overrides["response_status"] = rt::makeNumber(status);
      overrides["response_content_type"] = contentType;
      overrides["response_generation"] = "gen";
      overrides["response_chunk_count"] = rt::makeNumber(chunks);
      overrides["response_sha256"] = digest;
      return json::value(storedRecord(overrides));
    };
    const json::value record1 = succeeded(500, "application/json", 1, "abc");
    const json::value record2 = succeeded(200, "application/json", 1, "abc");
    const json::value record3 = succeeded(200, "application/json", 2, sha("body"));
    const json::value record4 = succeeded(200, "", 1, sha(""));
    const json::value record5 = succeeded(200, "text/csv", 2, sha("x,y"));
    auto keying1 = ledgerKey(std::string("principal-h"), std::string("key-1"));
    const rt::KvKey key1 = co_await std::move(keying1);
    co_await seed(key1, record1);
    auto keying2 = ledgerKey(std::string("principal-h"), std::string("key-2"));
    const rt::KvKey key2 = co_await std::move(keying2);
    co_await seed(key2, record2);
    auto prefixing2 = responsePrefix(std::string("principal-h"), std::string("key-2"));
    const rt::KvKey responses2 = co_await std::move(prefixing2);
    const rt::KvKey chunk2{responses2, std::string("gen"), 0};
    const json::value bodyChunk("body");
    co_await seed(chunk2, bodyChunk);
    auto keying3 = ledgerKey(std::string("principal-h"), std::string("key-3"));
    const rt::KvKey key3 = co_await std::move(keying3);
    co_await seed(key3, record3);
    auto prefixing3 = responsePrefix(std::string("principal-h"), std::string("key-3"));
    const rt::KvKey responses3 = co_await std::move(prefixing3);
    const rt::KvKey chunk30{responses3, std::string("gen"), 0};
    const rt::KvKey chunk31{responses3, std::string("gen"), 1};
    const json::value numberChunk(42);
    co_await seed(chunk30, bodyChunk);
    co_await seed(chunk31, numberChunk);
    auto keying4 = ledgerKey(std::string("principal-h"), std::string("key-4"));
    const rt::KvKey key4 = co_await std::move(keying4);
    co_await seed(key4, record4);
    auto keying5 = ledgerKey(std::string("principal-h"), std::string("key-5"));
    const rt::KvKey key5 = co_await std::move(keying5);
    co_await seed(key5, record5);
    auto prefixing5 = responsePrefix(std::string("principal-h"), std::string("key-5"));
    const rt::KvKey responses5 = co_await std::move(prefixing5);
    const rt::KvKey chunk50{responses5, std::string("gen"), 0};
    const rt::KvKey chunk51{responses5, std::string("gen"), 1};
    const json::value csvHead("x,");
    const json::value csvTail("y");
    co_await seed(chunk50, csvHead);
    co_await seed(chunk51, csvTail);
    steps.record("seed H non-replayable", json::value(nullptr));
  }
  co_await acquire(std::string("H status"), std::string("principal-h"), std::string("key-1"), FP1, std::string("req-h1"));
  co_await acquire(std::string("H hash"), std::string("principal-h"), std::string("key-2"), FP1, std::string("req-h2"));
  co_await acquire(std::string("H chunk type"), std::string("principal-h"), std::string("key-3"), FP1, std::string("req-h3"));
  co_await acquire(std::string("H empty type"), std::string("principal-h"), std::string("key-4"), FP1, std::string("req-h4"));
  co_await acquire(std::string("H csv replay"), std::string("principal-h"), std::string("key-5"), FP1, std::string("req-h5"));

  co_await acquire(std::string("I"), std::string("principal-i"), std::string("key-1"), FP1, std::string("req-i1"));
  co_await storeSuccess(std::string("store I too many chunks"), std::string("I"), makeResponse(repeat("x", 256 * 48'000 + 1)));
  co_await storeSuccess(std::string("store I exactly one chunk boundary"), std::string("I"), makeResponse(repeat("y", 48'000), 202));
  co_await acquire(std::string("I not replayed"), std::string("principal-i"), std::string("key-1"), FP1, std::string("req-i2"));

  kv->failGet = true;
  co_await acquire(std::string("J get fails"), std::string("principal-j"), std::string("key-1"), FP1, std::string("req-j1"));
  co_await dispatch(std::string("dispatch get fails"), std::string("A"));
  co_await release(std::string("release get fails"), std::string("A"), true);
  co_await indeterminate(std::string("indeterminate get fails"), std::string("A"));
  co_await storeSuccess(std::string("store get fails"), std::string("A"), makeResponse(std::string("x")));
  kv->failGet = false;

  co_await acquire(std::string("L"), std::string("principal-l"), std::string("key-1"), FP1, std::string("req-l1"));
  kv->rejectCommits = true;
  co_await acquire(std::string("K commits rejected"), std::string("principal-k"), std::string("key-1"), FP1, std::string("req-k1"));
  co_await dispatch(std::string("dispatch commits rejected"), std::string("L"));
  co_await release(std::string("release commits rejected"), std::string("L"), false);
  co_await indeterminate(std::string("indeterminate commits rejected"), std::string("L"));
  co_await storeSuccess(std::string("store commits rejected"), std::string("L"), makeResponse(std::string("y")));
  kv->rejectCommits = false;
  kv->failCommit = true;
  co_await acquire(std::string("M commit throws"), std::string("principal-m"), std::string("key-1"), FP1, std::string("req-m1"));
  kv->failCommit = false;
  kv->failSet = true;
  co_await storeSuccess(std::string("store set fails"), std::string("L"), makeResponse(std::string("y")));
  kv->failSet = false;
  {
    rt::Response used = makeResponse(std::string("z"));
    co_await used.text();
    co_await storeSuccess(std::string("store used body"), std::string("L"), used);
  }

  EXPECT_EQ(steps.next, vectors().at("steps").as_array().size());
}

ASYNC_TEST("differential: buildEmbeddingsIdempotencyFingerprint") {
  for (const json::value& row : vectors().at("fingerprints").as_array()) {
    const json::value input = untag(row);
    const ledger::ResolvedEmbeddingsProfile profile = json::value_to<ledger::ResolvedEmbeddingsProfile>(input.at(0));
    const std::vector<std::string> hashes = json::value_to<std::vector<std::string>>(input.at(1));
    auto building = ledger::buildEmbeddingsIdempotencyFingerprint(profile, hashes);
    const std::string fingerprint = co_await std::move(building);
    EXPECT_EQ(fingerprint, std::string(input.at(2).as_string()));
  }
  EXPECT_EQ(vectors().at("fingerprints").as_array().size(), std::size_t{5});
}

TEST("differential: hasAsciiControlCharacter") {
  for (const json::value& row : vectors().at("controls").as_array()) {
    const std::string text(row.at(0).as_string());
    EXPECT_EQ(rt::jsonQuote(text) + (ledger::hasAsciiControlCharacter(text) ? " true" : " false"), rt::jsonQuote(text) + (row.at(1).as_bool() ? " true" : " false"));
  }
  EXPECT_EQ(vectors().at("controls").as_array().size(), std::size_t{13});
}

ASYNC_TEST("differential: the fixed idempotency responses") {
  const json::object& expected = vectors().at("responses").as_object();
  auto indeterminateJson = responseJson(ledger::embeddingsIdempotencyIndeterminateResponse());
  const json::value indeterminate = co_await std::move(indeterminateJson);
  EXPECT(rt::deepEqual(indeterminate, expected.at("indeterminate")));
  auto unavailableJson = responseJson(ledger::embeddingsIdempotencyUnavailableResponse());
  const json::value unavailable = co_await std::move(unavailableJson);
  EXPECT(rt::deepEqual(unavailable, expected.at("unavailable")));
}

TEST("differential: keys and constants") {
  const json::object& keys = vectors().at("keys").as_object();
  const auto expectKey = [&](const char* name, const rt::KvKey& key) {
    if (!rt::deepEqual(keyJson(key), untag(keys.at(name)))) EXPECT_EQ(std::string(name) + " " + rt::jsonStringify(keyJson(key)), std::string(name) + " " + rt::jsonStringify(keys.at(name)));
  };
  expectKey("job", ledger::embeddingsJobKey("tok", "prof", "id"));
  expectKey("lookup", ledger::embeddingsJobLookupKey("tok", "id"));
  expectKey("input", ledger::embeddingsJobInputKey("tok", "prof", "job", "hash"));
  expectKey("cacheIndex", ledger::embeddingsCacheIndexKey("prof", 1.5, "hash"));
  expectKey("globalPrefix", ledger::embeddingsCacheGlobalIndexPrefix);
  expectKey("globalIndex", ledger::embeddingsCacheGlobalIndexKey(12, "prof", "hash"));
  expectKey("byHash", ledger::embeddingsCacheIndexByHashKey("prof", "hash"));
  expectKey("cache", ledger::embeddingsCacheKey("prof", "hash"));
  expectKey("rate", ledger::VOYAGE_RATE_LIMIT_KEY);
  expectKey("apiKey", ledger::VOYAGE_API_KEY_KV_KEY);

  const json::object& constants = vectors().at("constants").as_object();
  const auto number = [&](const char* name) { return rt::getNumber(constants, name).value(); };
  const auto text = [&](const char* name) { return std::string(constants.at(name).as_string()); };
  EXPECT_EQ(ledger::EMBEDDINGS_MAX_INPUTS_PER_REQUEST, number("EMBEDDINGS_MAX_INPUTS_PER_REQUEST"));
  EXPECT_EQ(ledger::EMBEDDINGS_MAX_CHARS_PER_INPUT, number("EMBEDDINGS_MAX_CHARS_PER_INPUT"));
  EXPECT_EQ(ledger::EMBEDDINGS_MAX_TOTAL_CHARS, number("EMBEDDINGS_MAX_TOTAL_CHARS"));
  EXPECT_EQ(ledger::EMBEDDINGS_TIMEOUT_MS, number("EMBEDDINGS_TIMEOUT_MS"));
  EXPECT_EQ(ledger::EMBEDDINGS_CACHE_EVICT_BATCH, number("EMBEDDINGS_CACHE_EVICT_BATCH"));
  EXPECT_EQ(ledger::EMBEDDINGS_CACHE_EVICT_MAX_BATCH, number("EMBEDDINGS_CACHE_EVICT_MAX_BATCH"));
  EXPECT_EQ(ledger::EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES, number("EMBEDDINGS_CACHE_QUOTA_MAX_RETRIES"));
  EXPECT_EQ(ledger::EMBEDDINGS_JOB_TTL_MS, number("EMBEDDINGS_JOB_TTL_MS"));
  EXPECT_EQ(ledger::EMBEDDINGS_JOB_LOCK_MS, number("EMBEDDINGS_JOB_LOCK_MS"));
  EXPECT_EQ(ledger::EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS, number("EMBEDDINGS_IDEMPOTENCY_MAX_RESPONSE_CHUNKS"));
  EXPECT_EQ(ledger::EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS, number("EMBEDDINGS_IDEMPOTENCY_LEDGER_TTL_MS"));
  EXPECT_EQ(ledger::EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS, number("EMBEDDINGS_IDEMPOTENCY_RESPONSE_TTL_MS"));
  EXPECT_EQ(ledger::EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS, number("EMBEDDINGS_IDEMPOTENCY_MAX_KEY_CHARS"));
  EXPECT_EQ(std::string(ledger::VOYAGE_EMBEDDINGS_URL), text("VOYAGE_EMBEDDINGS_URL"));
  EXPECT_EQ(std::string(ledger::VOYAGE_EMBEDDINGS_MODEL), text("VOYAGE_EMBEDDINGS_MODEL"));
  EXPECT_EQ(ledger::VOYAGE_DEFAULT_DIMENSIONS, number("VOYAGE_DEFAULT_DIMENSIONS"));
  EXPECT_EQ(std::string(ledger::VOYAGE_OUTPUT_DTYPE), text("VOYAGE_OUTPUT_DTYPE"));
  EXPECT_EQ(ledger::VOYAGE_RATE_LIMIT_RPM, number("VOYAGE_RATE_LIMIT_RPM"));
  EXPECT_EQ(ledger::VOYAGE_RATE_LIMIT_TPM, number("VOYAGE_RATE_LIMIT_TPM"));
  const auto numbers = [](const rt::OrderedSet<double>& set) {
    json::array out;
    for (const double value : set) out.push_back(rt::makeNumber(value));
    return json::value(std::move(out));
  };
  const auto strings = [](const rt::OrderedSet<std::string>& set) {
    json::array out;
    for (const std::string& value : set) out.push_back(json::value(value));
    return json::value(std::move(out));
  };
  EXPECT(rt::deepEqual(numbers(ledger::EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES), constants.at("EMBEDDINGS_RETRYABLE_UPSTREAM_STATUSES")));
  EXPECT(rt::deepEqual(numbers(ledger::VOYAGE_SUPPORTED_DIMENSIONS), constants.at("VOYAGE_SUPPORTED_DIMENSIONS")));
  EXPECT(rt::deepEqual(strings(ledger::UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS), constants.at("UOS_SYNC_EMBEDDINGS_ALLOWED_KEYS")));
  EXPECT(rt::deepEqual(strings(ledger::UOS_EMBEDDINGS_JOB_ALLOWED_KEYS), constants.at("UOS_EMBEDDINGS_JOB_ALLOWED_KEYS")));
}

TEST("job records keep the TS member order and round-trip through their JSON pair") {
  ledger::EmbeddingsJobRecord record{
    .id = "job-1",
    .status = "failed",
    .created_at_ms = 1,
    .updated_at_ms = 2,
    .model = "voyage-4-large",
    .cache_profile_key = "profile",
    .upstream = "voyage",
    .upstream_model = "voyage-4-large",
    .input_type = "document",
    .dimensions = 1024,
    .output_dtype = "float",
    .encoding_format = "base64",
    .truncation = true,
    .input_hashes = {"h1", "h2"},
    .input_count = 2,
    .total_chars = 10,
    .usage_total_tokens = 0,
    .retry_after_seconds = std::nullopt,
    .locked_until_ms = 5,
    .error = ledger::EmbeddingsJobRecord::Error{.message = "boom", .type = "upstream_error"},
  };
  EXPECT_EQ(rt::jsonStringify(json::value_from(record)),
            std::string("{\"id\":\"job-1\",\"status\":\"failed\",\"created_at_ms\":1,\"updated_at_ms\":2,\"model\":\"voyage-4-large\",\"cache_profile_key\":\"profile\","
                        "\"upstream\":\"voyage\",\"upstream_model\":\"voyage-4-large\",\"input_type\":\"document\",\"dimensions\":1024,\"output_dtype\":\"float\","
                        "\"encoding_format\":\"base64\",\"truncation\":true,\"input_hashes\":[\"h1\",\"h2\"],\"input_count\":2,\"total_chars\":10,"
                        "\"usage_total_tokens\":0,\"retry_after_seconds\":null,\"locked_until_ms\":5,\"error\":{\"message\":\"boom\",\"type\":\"upstream_error\"}}"));
  EXPECT(json::value_to<ledger::EmbeddingsJobRecord>(json::value_from(record)) == record);
  record.error->code = "upstream_429";
  EXPECT(json::value_to<ledger::EmbeddingsJobRecord>(json::value_from(record)) == record);
  record.error = std::nullopt;
  EXPECT(json::value_to<ledger::EmbeddingsJobRecord>(json::value_from(record)) == record);
  const ledger::EmbeddingsJobInputRecord input{.v = 1, .iv_b64 = "iv", .data_b64 = "data", .created_at_ms = 3};
  EXPECT_EQ(rt::jsonStringify(json::value_from(input)), std::string("{\"v\":1,\"iv_b64\":\"iv\",\"data_b64\":\"data\",\"created_at_ms\":3}"));
  EXPECT(json::value_to<ledger::EmbeddingsJobInputRecord>(json::value_from(input)) == input);
  const ledger::EmbeddingsJobLookupRecord lookup{.cache_profile_key = "profile"};
  EXPECT(json::value_to<ledger::EmbeddingsJobLookupRecord>(json::value_from(lookup)) == lookup);
  const ledger::VoyageRateLimitState state{.window_start_ms = 1, .requests = 2, .tokens = 3};
  EXPECT_EQ(rt::jsonStringify(json::value_from(state)), std::string("{\"window_start_ms\":1,\"requests\":2,\"tokens\":3}"));
  EXPECT(json::value_to<ledger::VoyageRateLimitState>(json::value_from(state)) == state);
  const ledger::ParsedEmbeddingsRequest parsed{
    .model = "voyage-4-large",
    .inputs = {"a"},
    .total_chars = 1,
    .profile = {.input_type = "query", .dimensions = 512, .output_dtype = "float", .encoding_format = "float", .truncation = false, .cache_profile_key = "k"},
  };
  EXPECT(json::value_to<ledger::ParsedEmbeddingsRequest>(json::value_from(parsed)) == parsed);
  EXPECT_THROWS_MESSAGE(json::value_to<ledger::EmbeddingsJobRecord>(json::value(json::object{{"id", 1}})), rt::TypeError, "EmbeddingsJobRecord.id is number, not string");
}

ASYNC_TEST("a response chunk with a character above U+00FF exceeds the native 64 KiB value limit (TS behaviour kept)") {
  // Not recorded from Bun here: rt::MemoryKv reproduces the guarded native store, which measures a value by
  // its JSC serialization. A 48,000-unit chunk holding a character above U+00FF is serialized as UTF-16
  // (about 96 KB), so the chunk write fails with Error "ValueTooLong" and storeEmbeddingsIdempotencySuccess
  // logs and returns false; a Latin-1 chunk of the same length fits.
  double clock = 1'800'000'000'000;
  rt::globalDateNow() = [&clock] { return clock; };
  const auto kv = std::make_shared<rt::MemoryKv>([&clock] { return clock; });
  rt::console::Capture capture({rt::console::Method::error});
  const ledger::AcquireEmbeddingsIdempotencyLeaseParams params{.kv = kv, .principal = "principal", .idempotencyKey = "key", .fingerprint = FP1, .requestId = "req"};
  auto acquiring = ledger::acquireEmbeddingsIdempotencyLease(params);
  const ledger::EmbeddingsIdempotencyAcquireResult result = co_await std::move(acquiring);
  ASSERT(std::holds_alternative<ledger::EmbeddingsIdempotencyAcquireResultAcquired>(result));
  const EmbeddingsIdempotencyLease lease = std::get<ledger::EmbeddingsIdempotencyAcquireResultAcquired>(result).lease;

  auto storingWide = ledger::storeEmbeddingsIdempotencySuccess(lease, makeResponse(repeat("a", 47'999) + "\xC4\x80" + repeat("b", 10)));
  const bool storedWide = co_await std::move(storingWide);
  EXPECT(!storedWide);
  ASSERT_EQ(capture.calls().size(), std::size_t{1});
  EXPECT(capture.calls()[0].args.at(0) == "[ai.ubq.fi] embeddings idempotency response write failed:");
  EXPECT_EQ(capture.calls()[0].args.at(1).errorInfo().message, std::string("ValueTooLong"));

  auto storingLatin1 = ledger::storeEmbeddingsIdempotencySuccess(lease, makeResponse(repeat("a", 47'999) + "\xC3\xA9" + repeat("b", 10)));
  const bool storedLatin1 = co_await std::move(storingLatin1);
  EXPECT(storedLatin1);
  EXPECT_EQ(capture.calls().size(), std::size_t{1});
}
