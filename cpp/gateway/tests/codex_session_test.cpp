// Port of tests/codex-session.test.ts (every case).
// Links: tests/codex_session_test.cpp src/codex/session.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
//        tests/helpers/counting_kv.cpp
#include "uos/codex/session.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/kv.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"

#include <exception>
#include <memory>
#include <string>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using uos::codex::session::CodexSessionObservation;
  using uos::codex::session::observeCodexSession;
  using uos::tests::helpers::counting_kv::CountingKv;
  using uos::tests::helpers::counting_kv::encodeKey;

  // `{ session_id: id }`
  json::value sessionMetadata(const std::string& sessionId) {
    json::object metadata;
    metadata["session_id"] = sessionId;
    return json::value(std::move(metadata));
  }
}

ASYNC_TEST("codex session observer classifies first, continuing, and hour-idle requests") {
  const auto kv = std::make_shared<CountingKv>();
  uos::kv::setKvForTest(kv);
  rt::Reason failure;
  try {
    auto observingFirst = observeCodexSession(std::string("key-a"), sessionMetadata("session-a"), 1'000'000);
    const CodexSessionObservation first = co_await std::move(observingFirst);
    EXPECT_EQ(first.state, std::string("new"));
    EXPECT_EQ(first.continuation_only_candidate, false);
    EXPECT_EQ(first.session_id_present, true);

    auto observingContinuing = observeCodexSession(std::string("key-a"), sessionMetadata("session-a"), 1'000'001);
    const CodexSessionObservation continuing = co_await std::move(observingContinuing);
    EXPECT_EQ(continuing.state, std::string("continuation"));
    EXPECT_EQ(continuing.continuation_age_ms, std::optional<double>(1));
    EXPECT_EQ(continuing.continuation_only_candidate, false);

    auto observingIdle = observeCodexSession(std::string("key-a"), sessionMetadata("session-a"), 1'000'000 + 60 * 60 * 1000);
    const CodexSessionObservation hourIdle = co_await std::move(observingIdle);
    EXPECT_EQ(hourIdle.state, std::string("continuation"));
    EXPECT_EQ(hourIdle.continuation_age_ms, std::optional<double>(60 * 60 * 1000));
    EXPECT_EQ(hourIdle.continuation_only_candidate, true);

    auto observingOther = observeCodexSession(std::string("key-b"), sessionMetadata("session-a"), 1'000'000 + 60 * 60 * 1000);
    const CodexSessionObservation otherApiKey = co_await std::move(observingOther);
    EXPECT_EQ(otherApiKey.state, std::string("new"));
    EXPECT_EQ(otherApiKey.continuation_only_candidate, false);
    static const rt::Regex RAW_IDENTITY("session-a|key-a|key-b", "");
    EXPECT(kv->entries.size() > 0);
    for (const auto& [encoded, entry] : kv->entries) {
      EXPECT(!RAW_IDENTITY.test(rt::jsonStringify(entry.value.toJson())));
      EXPECT(!RAW_IDENTITY.test(encodeKey(entry.key)));
    }
  } catch (...) {
    failure = std::current_exception();
  }
  uos::kv::setKvForTest(nullptr);
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("codex session observer resets the continuation window on a new session") {
  const auto kv = std::make_shared<CountingKv>();
  uos::kv::setKvForTest(kv);
  rt::Reason failure;
  try {
    auto observingFirst = observeCodexSession(std::string("key-a"), sessionMetadata("session-a"), 2'000'000);
    co_await std::move(observingFirst);
    auto observingNew = observeCodexSession(std::string("key-a"), sessionMetadata("session-b"), 2'000'000 + 60 * 60 * 1000);
    const CodexSessionObservation newSession = co_await std::move(observingNew);
    EXPECT_EQ(newSession.state, std::string("new"));
    EXPECT_EQ(newSession.continuation_only_candidate, false);

    auto observingOld = observeCodexSession(std::string("key-a"), sessionMetadata("session-a"), 2'000'000 + 60 * 60 * 1000 + 1);
    const CodexSessionObservation oldSession = co_await std::move(observingOld);
    EXPECT_EQ(oldSession.state, std::string("continuation"));
    EXPECT_EQ(oldSession.continuation_age_ms, std::optional<double>(1));
    EXPECT_EQ(oldSession.continuation_only_candidate, false);
  } catch (...) {
    failure = std::current_exception();
  }
  uos::kv::setKvForTest(nullptr);
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("codex session observer fails closed for missing metadata or KV") {
  uos::kv::setKvForTest(nullptr);
  auto observingMissing = observeCodexSession(std::string("key-a"), std::nullopt, 3'000'000);
  const CodexSessionObservation missing = co_await std::move(observingMissing);
  const CodexSessionObservation expected{
    .metadata_present = false,
    .session_id_present = false,
    .session_id_hash = std::nullopt,
    .state = "unknown",
    .continuation_age_ms = std::nullopt,
    .continuation_only_candidate = false,
  };
  EXPECT(missing == expected);
  EXPECT_EQ(json::value_from(missing), json::value_from(expected));

  json::object turn;
  turn["request_kind"] = "turn";
  auto observingMalformed = observeCodexSession(std::string("key-a"), json::value(std::move(turn)), 3'000'000);
  const CodexSessionObservation malformed = co_await std::move(observingMalformed);
  EXPECT_EQ(malformed.metadata_present, true);
  EXPECT_EQ(malformed.state, std::string("unknown"));
  EXPECT_EQ(malformed.session_id_present, false);
}

