#include "uos/auth/passport_sessions.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"
#include "uos/utils.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <array>
#include <exception>
#include <memory>
#include <utility>

namespace uos::auth::passport_sessions {
  namespace {
    constexpr std::string_view ISSUER = "https://auth.ubq.fi";
    const rt::Regex& LOCAL_TOKEN() {
      static const rt::Regex pattern("^uos_ai_passport_session_[A-Za-z0-9_-]{43}$", "");
      return pattern;
    }
    const rt::Regex& APP_TOKEN() {
      static const rt::Regex pattern("^uos_app_session_[A-Za-z0-9_-]{43}$", "");
      return pattern;
    }
    struct BindingMember {
      std::string_view name;
      std::string_view expected;
    };
    constexpr std::array<BindingMember, 4> BINDING{{
      {"clientId", "ai"},
      {"audience", "ai"},
      {"origin", "https://ai.ubq.fi"},
      {"redirectUri", "https://ai.ubq.fi/auth/callback"},
    }};

    // A record `validRecord` accepted: version 1 with the exact BINDING members, so only the
    // varying members are kept.
    struct SessionRecord {
      std::string subject;
      std::string createdAt;
      std::string appSessionToken;
    };

    json::object bindingJson() {
      json::object binding;
      for (const BindingMember& member : BINDING) binding[member.name] = member.expected;
      return binding;
    }

    // `typeof value === "string" && value.length > 0 && value.length <= limit`
    bool boundedText(std::string_view value, double limit) { return !value.empty() && static_cast<double>(rt::length(value)) <= limit; }
    bool boundedString(const json::value* value, double limit) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && boundedText(*text, limit);
    }
    bool boundedString(const rt::KvValue& value, double limit) { return value.isString() && boundedText(value.asString(), limit); }

    // `value[name] === expected` for a string.
    bool isText(const json::value* value, std::string_view expected) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && *text == expected;
    }
    bool isText(const rt::KvValue& value, std::string_view expected) { return value.isString() && value.asString() == expected; }

    bool matchesBinding(const json::value& value) {
      for (const BindingMember& member : BINDING) {
        if (!isText(rt::get(value, member.name), member.expected)) return false;
      }
      return true;
    }
    bool matchesBinding(const rt::KvValue& value) {
      for (const BindingMember& member : BINDING) {
        if (!isText(value[member.name], member.expected)) return false;
      }
      return true;
    }

    rt::Task<rt::KvKey> sessionKey(std::string token) {
      auto hashing = utils::sha256Hex(token);
      const std::string digest = co_await std::move(hashing);
      co_return rt::KvKey{"uos_ai", "auth", "passport", "sessions", digest};
    }

    // `typeof value === "object" && value !== null` (utils.isRecord)

    // `validRecord(value): value is SessionRecord`
    std::optional<SessionRecord> validRecord(const rt::KvValue& value) {
      if (!(rt::kvIsObject(value) && value["version"].isNumber() && value["version"].asNumber() == 1 && boundedString(value["subject"], 500) &&
            boundedString(value["createdAt"], 100) && rt::Number::isFinite(rt::Date::parse(value["createdAt"].asString())) &&
            value["appSessionToken"].isString() && APP_TOKEN().test(value["appSessionToken"].asString()) && matchesBinding(value))) {
        return std::nullopt;
      }
      return SessionRecord{.subject = value["subject"].asString(), .createdAt = value["createdAt"].asString(), .appSessionToken = value["appSessionToken"].asString()};
    }

    rt::Task<json::value> readJson(rt::Response response) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw PassportSessionError(503);
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();
      rt::TextDecoder decoder;
      double bytes = 0;
      std::string text;
      rt::Reason failure;
      json::value parsed;
      try {
        for (;;) {
          auto reading = reader.read();
          const rt::ReadableStreamReadResult<rt::ByteChunk> item = co_await std::move(reading);
          if (item.done) break;
          bytes += static_cast<double>(item.value->size());
          if (bytes > 65'536) throw PassportSessionError(503);
          text += decoder.decode(*item.value, {.stream = true});
        }
        parsed = rt::jsonParse(text + decoder.decode());
      } catch (...) {
        failure = std::current_exception();
      }
      try {
        auto cancelling = reader.cancel();
        co_await std::move(cancelling);
      } catch (...) {
      }
      reader.releaseLock();
      if (failure) std::rethrow_exception(failure);
      co_return parsed;
    }

    rt::Task<rt::Response> sessionRequest(SessionRecord record, bool revoke) {
      rt::RequestInit init;
      init.method = "POST";
      init.headers = rt::Headers{{"Authorization", "Bearer " + record.appSessionToken}, {"Content-Type", "application/json"}, {"Accept", "application/json"}};
      init.body = rt::BodyInit(rt::jsonStringify(json::value(bindingJson())));
      init.redirect = "error";
      init.signal = rt::AbortSignal::timeout(10'000);
      const std::string url = std::string(ISSUER) + "/api/auth/sso/session" + (revoke ? "/revoke" : "");
      auto call = rt::fetch(rt::RequestInfo(url), std::move(init));
      rt::Response response = co_await std::move(call);
      co_return response;
    }

    // `await response.body?.cancel()`
    rt::Task<void> cancelBody(rt::Response response) {
      if (std::optional<rt::ByteStream> body = response.body()) {
        auto cancelling = body->cancel();
        co_await std::move(cancelling);
      }
    }

    PassportSessionIdentity identityFromResponse(const json::value& body, const SessionRecord& record) {
      const json::value* session = rt::get(body, "session");
      const json::value* user = rt::get(body, "user");
      const json::value* authenticated = rt::get(body, "authenticated");
      if (!utils::isRecord(body) || !(authenticated && authenticated->is_bool() && authenticated->get_bool()) || !utils::isRecord(session) ||
          !utils::isRecord(user))
        throw PassportSessionError(503);
      const std::optional<std::string_view> handle = rt::getString(rt::get(user, "handle"));
      const std::optional<bool> isAdmin = rt::getBool(rt::get(user, "isAdmin"));
      const json::array* scopes = rt::getArray(rt::get(user, "scopes"));
      if (!matchesBinding(*session) || !isText(rt::get(session, "subject"), record.subject) || !isText(rt::get(session, "createdAt"), record.createdAt) ||
          !isText(rt::get(user, "id"), record.subject) || !handle || rt::length(*handle) > 500 || !isAdmin || !scopes || scopes->size() > 128 ||
          !std::all_of(scopes->begin(), scopes->end(), [](const json::value& scope) { return scope.is_string() && rt::length(std::string_view(scope.get_string())) <= 500; }))
        throw PassportSessionError(503);
      std::vector<std::string> scopeList;
      for (const json::value& scope : *scopes) scopeList.emplace_back(scope.get_string());
      return {.subject = record.subject, .handle = std::string(*handle), .isAdmin = *isAdmin, .scopes = std::move(scopeList)};
    }
  }

  bool isPersistentPassportSession(const std::string& token) { return LOCAL_TOKEN().test(token); }

  rt::Task<std::string> createPassportSession(json::value grant, std::string subject) {
    auto creating = createPassportSession(std::move(grant), std::move(subject), std::nullopt);
    std::string token = co_await std::move(creating);
    co_return token;
  }

  rt::Task<std::string> createPassportSession(json::value grant, std::string subject, std::optional<std::string> previousToken) {
    const std::optional<std::string_view> createdAt = rt::getString(grant, "createdAt");
    const std::optional<std::string_view> grantToken = rt::getString(grant, "token");
    if (!utils::isRecord(grant) || !boundedText(subject, 500) || !isText(rt::get(grant, "subject"), subject) || !boundedString(rt::get(grant, "createdAt"), 100) ||
        !rt::Number::isFinite(rt::Date::parse(*createdAt)) || !grantToken || !APP_TOKEN().test(*grantToken) || !matchesBinding(grant))
      throw PassportSessionError(503);
    auto pending = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
    if (!kv) throw PassportSessionError(503);
    rt::Bytes random(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(random));
    const std::string token = "uos_ai_passport_session_" + utils::base64UrlEncode(random);
    auto keying = sessionKey(token);
    const rt::KvKey key = co_await std::move(keying);
    rt::KvObject record;
    record.set("version", rt::KvValue(1));
    record.set("subject", rt::KvValue(subject));
    for (const BindingMember& member : BINDING) record.set(std::string(member.name), rt::KvValue(member.expected));
    record.set("createdAt", rt::KvValue(std::string(*createdAt)));
    record.set("appSessionToken", rt::KvValue(std::string(*grantToken)));
    bool failed = false;
    try {
      std::optional<rt::KvKey> previousKey;
      if (previousToken && !previousToken->empty() && isPersistentPassportSession(*previousToken)) {
        auto previousKeying = sessionKey(*previousToken);
        previousKey = co_await std::move(previousKeying);
      }
      rt::KvValue previous(nullptr);
      if (previousKey) {
        auto reading = kv->get(*previousKey);
        const rt::KvEntryMaybe<> entry = co_await std::move(reading);
        previous = entry.value;
      }
      std::unique_ptr<rt::AtomicOperation> transaction = kv->atomic();
      transaction->check(rt::AtomicCheck(key, std::nullopt)).set(key, rt::KvValue(record));
      if (previousKey) transaction->delete_(*previousKey);
      auto committing = transaction->commit();
      const rt::KvCommitResult commit = co_await std::move(committing);
      if (!commit.ok) throw PassportSessionError(503);
      if (const std::optional<SessionRecord> replaced = validRecord(previous)) {
        try {
          auto requesting = sessionRequest(*replaced, true);
          const rt::Response response = co_await std::move(requesting);
          auto cancelling = cancelBody(response);
          co_await std::move(cancelling);
        } catch (...) {
          // The replaced local session is already irrevocably removed.
        }
      }
    } catch (...) {
      failed = true;
    }
    if (failed) throw PassportSessionError(503);
    co_return token;
  }

  rt::Task<PassportSessionIdentity> getPassportSession(std::string token) {
    if (!isPersistentPassportSession(token)) throw PassportSessionError(401);
    auto pending = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
    if (!kv) throw PassportSessionError(503);
    rt::Reason failure;
    std::optional<PassportSessionIdentity> identity;
    try {
      auto keying = sessionKey(token);
      const rt::KvKey key = co_await std::move(keying);
      auto reading = kv->get(key);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (!entry.value.truthy()) throw PassportSessionError(401);
      const std::optional<SessionRecord> record = validRecord(entry.value);
      if (!record) throw PassportSessionError(503);
      auto requesting = sessionRequest(*record, false);
      const rt::Response response = co_await std::move(requesting);
      if (response.status() == 401) {
        try {
          auto cancelling = cancelBody(response);
          co_await std::move(cancelling);
        } catch (...) {
        }
        auto deleting = kv->delete_(key);
        co_await std::move(deleting);
        throw PassportSessionError(401);
      }
      if (!response.ok()) {
        auto cancelling = cancelBody(response);
        co_await std::move(cancelling);
        throw PassportSessionError(503);
      }
      auto parsing = readJson(response);
      const json::value body = co_await std::move(parsing);
      identity = identityFromResponse(body, *record);
      // Logout owns deletion. An in-flight introspection can never republish it.
      auto rereading = kv->get(key);
      const rt::KvEntryMaybe<> current = co_await std::move(rereading);
      const std::optional<SessionRecord> currentRecord = validRecord(current.value);
      if (!currentRecord || currentRecord->appSessionToken != record->appSessionToken) throw PassportSessionError(401);
    } catch (const PassportSessionError&) {
      failure = std::current_exception();
    } catch (...) {
      failure = std::make_exception_ptr(PassportSessionError(503));
    }
    if (failure) std::rethrow_exception(failure);
    co_return *identity;
  }

  rt::Task<std::string> revokePassportSession(std::string token) {
    if (!isPersistentPassportSession(token)) co_return "not_applicable";
    auto pending = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
    if (!kv) throw PassportSessionError(503);
    rt::KvValue value;
    bool failed = false;
    try {
      auto keying = sessionKey(token);
      const rt::KvKey key = co_await std::move(keying);
      auto reading = kv->get(key);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      value = entry.value;
      auto deleting = kv->delete_(key);
      co_await std::move(deleting);
    } catch (...) {
      failed = true;
    }
    if (failed) throw PassportSessionError(503);
    const std::optional<SessionRecord> record = validRecord(value);
    if (!record) co_return "not_applicable";
    std::string outcome = "unavailable";
    try {
      auto requesting = sessionRequest(*record, true);
      const rt::Response response = co_await std::move(requesting);
      auto cancelling = cancelBody(response);
      co_await std::move(cancelling);
      outcome = response.status() == 204 ? "revoked" : "unavailable";
    } catch (...) {
      outcome = "unavailable";
    }
    co_return outcome;
  }
}
