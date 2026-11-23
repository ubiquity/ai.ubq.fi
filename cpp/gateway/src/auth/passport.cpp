#include "uos/auth/passport.hpp"
#include "uos/rt/memory.hpp"

#include "uos/auth/passport_sessions.hpp"
#include "uos/http.hpp"
#include "uos/utils.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <utility>

namespace uos::auth::passport {
  namespace {
    using passport_sessions::PassportSessionError;
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr std::string_view ISSUER = "https://passport.ubq.fi";
    constexpr std::string_view ORIGIN = "https://ai.ubq.fi";
    const std::string CALLBACK = std::string(ORIGIN) + "/auth/callback";
    constexpr std::string_view SESSION_COOKIE = "__Host-uos_ai_passport_session";
    constexpr std::string_view STATE_COOKIE = "__Host-uos_ai_passport_state";
    constexpr std::string_view VERIFIER_COOKIE = "__Host-uos_ai_passport_verifier";
    constexpr double JWKS_TTL_MS = 300'000;
    constexpr std::string_view COOKIE_ATTRIBUTES = "; Path=/; HttpOnly; Secure; SameSite=Lax";

    // `PassportClaims`, once validClaims accepted them.
    struct PassportClaims {
      std::string iss;
      std::string sub;
      std::optional<std::string> handle;
      bool isAdmin;
      std::optional<std::vector<std::string>> scopes;
      std::string aud;
      std::string clientId;
      std::string auth;
      double iat;
      double exp;
      std::string jti;
      // `agentTokenId?: string | null`: absent, null or a string.
      rt::Nullable<std::string> agentTokenId;
    };

    struct JwksCache {
      double expiresAt;
      rt::OrderedMap<std::string, rt::crypto::CryptoKey> keys;
    };
    std::optional<JwksCache> jwksCache;

    // `class PassportError extends Error` (its name stays "Error").
    class PassportError : public rt::JsError {
    public:
      double status;
      explicit PassportError(double status_ = 401)
        : rt::JsError(status_ == 502 ? "Passport verification is unavailable." : "Unauthorized"), status(status_) {}
    };

    rt::Response errorResponseFor(double status) {
      const bool unavailable = status == 502 || status == 503;
      http::OpenaiErrorOptions options;
      options.headers = rt::Headers{{"Cache-Control", "no-store"}, {"Referrer-Policy", "no-referrer"}};
      return http::openaiError(status, unavailable ? "Passport verification is unavailable." : "Unauthorized",
                               std::string(unavailable ? "passport_unavailable" : "invalid_api_key"), std::move(options));
    }

    // `errorResponse(error: unknown)`: the status of a PassportError / PassportSessionError, else 401.
    rt::Response errorResponse(const std::exception_ptr& error) {
      double status = 401;
      try {
        if (error) std::rethrow_exception(error);
      } catch (const PassportError& passportError) {
        status = passportError.status;
      } catch (const PassportSessionError& sessionError) {
        status = sessionError.status;
      } catch (...) {
        status = 401;
      }
      return errorResponseFor(status);
    }
    rt::Response errorResponse(const PassportError& error) { return errorResponseFor(error.status); }

    std::optional<std::string> cookie(const rt::Request& req, std::string_view name) {
      std::vector<std::string> values;
      for (const std::string& part : rt::split(req.headers().get("cookie").value_or(""), ";")) values.push_back(rt::trim(part));
      const std::string prefix = std::string(name) + "=";
      std::vector<std::string> matches;
      for (const std::string& part : values) {
        if (rt::startsWith(part, prefix)) matches.push_back(part);
      }
      return matches.size() == 1 ? std::optional<std::string>(rt::slice(matches[0], static_cast<double>(rt::length(name)) + 1)) : std::nullopt;
    }

    std::string sessionCookie(const std::string& token) {
      return std::string(SESSION_COOKIE) + "=" + token + "; Max-Age=" + rt::numberToString(passport_sessions::PASSPORT_COOKIE_MAX_AGE_SECONDS) +
             std::string(COOKIE_ATTRIBUTES);
    }

    // node:http receives Caddy's HTTP request with the original Host. Never derive
    // the registered client, issuer or callback from forwarded hosts or user input.
    bool isPassportRequest(const rt::Request& req) {
      const rt::URL url(req.url());
      return url.host() == "ai.ubq.fi" && (url.protocol() == "https:" || (url.protocol() == "http:" && req.headers().get("x-forwarded-proto") == "https"));
    }

    // `await response.body?.cancel()`
    rt::Task<void> cancelBody(rt::Response response) {
      if (std::optional<rt::ByteStream> body = response.body()) {
        auto cancelling = body->cancel();
        co_await std::move(cancelling);
      }
    }

    rt::Task<json::value> boundedJson(rt::Response response, double maxBytes) {
      if (!response.ok() || !response.body()) {
        auto cancelling = cancelBody(response);
        co_await std::move(cancelling);
        throw PassportError(502);
      }
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = response.body()->getReader();
      rt::TextDecoder decoder;
      double total = 0;
      std::string text;
      rt::Reason failure;
      json::value parsed;
      try {
        for (;;) {
          auto reading = reader.read();
          const rt::ReadableStreamReadResult<rt::ByteChunk> item = co_await std::move(reading);
          if (item.done) break;
          total += static_cast<double>(item.value->size());
          if (total > maxBytes) throw PassportError(502);
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

    // `value === undefined || value === text`
    bool absentOr(const json::value* value, std::string_view text) {
      if (!value) return true;
      const std::optional<std::string_view> member = rt::getString(value);
      return member && *member == text;
    }

    bool isRsaSigningKey(const json::value& value) {
      const std::optional<std::string_view> kty = rt::getString(rt::get(value, "kty"));
      return utils::isRecord(value) && kty && *kty == "RSA" && absentOr(rt::get(value, "alg"), "RS256") && absentOr(rt::get(value, "use"), "sig");
    }

    rt::Task<rt::OrderedMap<std::string, rt::crypto::CryptoKey>> loadJwks(std::string kid) {
      if (jwksCache && jwksCache->expiresAt > rt::dateNow() && jwksCache->keys.has(kid)) co_return jwksCache->keys;
      std::optional<rt::OrderedMap<std::string, rt::crypto::CryptoKey>> loaded;
      try {
        rt::RequestInit init;
        init.headers = rt::Headers{{"Accept", "application/json"}};
        init.redirect = "error";
        init.signal = rt::AbortSignal::timeout(10'000);
        const std::string url = std::string(ISSUER) + "/api/auth/jwks";
        auto call = rt::fetch(rt::RequestInfo(url), std::move(init));
        const rt::Response response = co_await std::move(call);
        auto reading = boundedJson(response, 262'144);
        const json::value body = co_await std::move(reading);
        const json::array* keyList = rt::getArray(rt::get(body, "keys"));
        if (!utils::isRecord(body) || !keyList || keyList->empty()) throw PassportError(502);
        rt::OrderedMap<std::string, rt::crypto::CryptoKey> keys;
        std::size_t keyBytes = 0;
        for (const json::value& value : *keyList) {
          if (!isRsaSigningKey(value)) continue;
          const std::optional<std::string_view> keyId = rt::getString(rt::get(value, "kid"));
          if (!keyId || keyId->empty() || rt::length(*keyId) > 200 || keys.has(std::string(*keyId))) throw PassportError(502);
          const std::optional<std::string_view> modulus = rt::getString(rt::get(value, "n"));
          const std::optional<std::string_view> exponent = rt::getString(rt::get(value, "e"));
          if (!modulus || !exponent) throw PassportError(502);
          const std::size_t modulusBytes = utils::base64UrlDecode(*modulus).size();
          if (modulusBytes < 256 || modulusBytes > 1024 || utils::base64UrlDecode(*exponent).size() > 8) throw PassportError(502);
          json::object jwk;
          jwk["kty"] = "RSA";
          jwk["n"] = *modulus;
          jwk["e"] = *exponent;
          jwk["alg"] = "RS256";
          jwk["ext"] = true;
          const rt::crypto::CryptoKey key =
            rt::crypto::subtle::importKey("jwk", jwk, rt::crypto::Algorithm{.name = "RSASSA-PKCS1-v1_5", .hash = "SHA-256"}, false, {"verify"});
          co_await rt::microtask();
          keys.set(std::string(*keyId), key);
          keyBytes += sizeof(rt::crypto::CryptoKey) + modulusBytes + 8 + 2 * (keyId->size() + 1);
        }
        if (keys.size() == 0) throw PassportError(502);
        jwksCache = JwksCache{.expiresAt = rt::dateNow() + JWKS_TTL_MS, .keys = keys};
        rt::memory::setCache(rt::memory::Cache::passport_jwks, keys.size(), keyBytes);
        loaded = std::move(keys);
      } catch (...) {
        loaded = std::nullopt;
      }
      if (!loaded) throw PassportError(502);
      co_return *loaded;
    }

    // `typeof value === "string" && value.length > 0 && value.length <= limit`
    bool boundedString(const json::value* value, double limit) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && !text->empty() && static_cast<double>(rt::length(*text)) <= limit;
    }

    bool isText(const json::value* value, std::string_view expected) {
      const std::optional<std::string_view> text = rt::getString(value);
      return text && *text == expected;
    }

    std::optional<PassportClaims> validClaims(const json::value& value) {
      if (!utils::isRecord(value)) return std::nullopt;
      const double now = rt::Math::floor(rt::dateNow() / 1000);
      const json::value* handle = rt::get(value, "handle");
      const json::value* scopes = rt::get(value, "scopes");
      const json::value* agentTokenId = rt::get(value, "agentTokenId");
      const std::optional<bool> isAdmin = rt::getBool(rt::get(value, "isAdmin"));
      const std::optional<double> iat = rt::getNumber(rt::get(value, "iat"));
      const std::optional<double> exp = rt::getNumber(rt::get(value, "exp"));
      const bool scopesValid =
        !scopes || (scopes->is_array() && scopes->as_array().size() <= 128 && std::all_of(scopes->as_array().begin(), scopes->as_array().end(), [](const json::value& scope) {
                      return scope.is_string() && rt::length(std::string_view(scope.get_string())) <= 500;
                    }));
      const bool valid = isText(rt::get(value, "iss"), ISSUER) && isText(rt::get(value, "aud"), "ai") && isText(rt::get(value, "clientId"), "ai") &&
                         boundedString(rt::get(value, "sub"), 500) && boundedString(rt::get(value, "jti"), 500) && boundedString(rt::get(value, "auth"), 160) &&
                         isAdmin && (!handle || (handle->is_string() && rt::length(std::string_view(handle->get_string())) <= 500)) && scopesValid && iat &&
                         rt::Number::isSafeInteger(*iat) && *iat <= now + 60 && exp && rt::Number::isSafeInteger(*exp) && *exp > now && *exp > *iat &&
                         (!agentTokenId || agentTokenId->is_null() || boundedString(agentTokenId, 500));
      if (!valid) return std::nullopt;
      std::optional<std::vector<std::string>> scopeList;
      if (scopes) {
        scopeList.emplace();
        for (const json::value& scope : scopes->as_array()) scopeList->emplace_back(scope.get_string());
      }
      rt::Nullable<std::string> agent;
      if (agentTokenId) agent = agentTokenId->is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(std::string(agentTokenId->get_string()));
      return PassportClaims{
        .iss = std::string(ISSUER),
        .sub = std::string(*rt::getString(rt::get(value, "sub"))),
        .handle = handle ? std::optional<std::string>(std::string(handle->get_string())) : std::nullopt,
        .isAdmin = *isAdmin,
        .scopes = std::move(scopeList),
        .aud = "ai",
        .clientId = "ai",
        .auth = std::string(*rt::getString(rt::get(value, "auth"))),
        .iat = *iat,
        .exp = *exp,
        .jti = std::string(*rt::getString(rt::get(value, "jti"))),
        .agentTokenId = agent,
      };
    }

    rt::Task<PassportClaims> verifyToken(std::string token) {
      static const rt::Regex PART("^[A-Za-z0-9_-]+$", "");
      if (token.empty() || rt::length(token) > 32'768) throw PassportError();
      const std::vector<std::string> parts = rt::split(token, ".");
      if (parts.size() != 3 || std::any_of(parts.begin(), parts.end(), [](const std::string& part) { return part.empty() || !PART.test(part); })) {
        throw PassportError();
      }
      const json::value header = rt::jsonParse(rt::decodeUtf8(utils::base64UrlDecode(parts[0])));
      const json::value claimsValue = rt::jsonParse(rt::decodeUtf8(utils::base64UrlDecode(parts[1])));
      if (!utils::isRecord(header) || !isText(rt::get(header, "alg"), "RS256") || !isText(rt::get(header, "typ"), "JWT") || !boundedString(rt::get(header, "kid"), 200)) {
        throw PassportError();
      }
      const std::optional<PassportClaims> claims = validClaims(claimsValue);
      constexpr std::array<std::string_view, 5> FORBIDDEN_HEADERS{"jku", "x5u", "x5c", "jwk", "crit"};
      if (std::any_of(FORBIDDEN_HEADERS.begin(), FORBIDDEN_HEADERS.end(), [&header](std::string_view name) { return rt::get(header, name) != nullptr; }) || !claims) {
        throw PassportError();
      }
      const std::string kid(*rt::getString(rt::get(header, "kid")));
      auto loading = loadJwks(kid);
      const rt::OrderedMap<std::string, rt::crypto::CryptoKey> keys = co_await std::move(loading);
      const rt::crypto::CryptoKey* key = keys.get(kid);
      if (!key) throw PassportError();
      const bool verified =
        rt::crypto::subtle::verify("RSASSA-PKCS1-v1_5", *key, utils::base64UrlDecode(parts[2]), rt::encodeUtf8(parts[0] + "." + parts[1]));
      co_await rt::microtask();
      if (!verified) throw PassportError();
      co_return *claims;
    }

    bool isPassportBearer(const std::optional<std::string>& token) {
      if (!token || rt::split(*token, ".").size() != 3) return false;
      try {
        const json::value header = rt::jsonParse(rt::decodeUtf8(utils::base64UrlDecode(rt::split(*token, ".")[0])));
        return utils::isRecord(header) && rt::getString(rt::get(header, "alg")).has_value();
      } catch (...) {
        return false;
      }
    }

    std::optional<rt::Response> passportAdminRejection(const rt::Request& req, const PassportAuthMethod& method) {
      if (method.user.agent_token_id) return errorResponse(PassportError());
      if (!method.user.is_admin) return http::openaiError(403, "Forbidden", std::string("forbidden"));
      const std::string requestMethod = req.method();
      if (requestMethod == "GET" || requestMethod == "HEAD" || requestMethod == "OPTIONS") return std::nullopt;
      const std::optional<std::string> origin = req.headers().get("origin");
      const std::optional<std::string> site = req.headers().get("sec-fetch-site");
      if ((origin && *origin != ORIGIN) || (site && *site != "same-origin" && *site != "none")) return http::openaiError(403, "Forbidden", std::string("forbidden"));
      return std::nullopt;
    }

    rt::Response redirect(const std::string& location, const std::vector<std::string>& cookies) {
      rt::Headers headers{{"Location", location}, {"Cache-Control", "no-store"}, {"Referrer-Policy", "no-referrer"}};
      for (const std::string& value : cookies) headers.append("Set-Cookie", value);
      rt::ResponseInit init;
      init.status = 302;
      init.headers = std::move(headers);
      return rt::Response(rt::BodyInit(nullptr), std::move(init));
    }

    std::string clearStateCookie() { return std::string(STATE_COOKIE) + "=; Max-Age=0" + std::string(COOKIE_ATTRIBUTES); }
    std::string clearVerifierCookie() { return std::string(VERIFIER_COOKIE) + "=; Max-Age=0" + std::string(COOKIE_ATTRIBUTES); }

    rt::Task<rt::Response> callback(rt::Request req) {
      static const rt::Regex STATE("^[A-Za-z0-9_-]{43}\\.(?:admin|chat)$", "");
      static const rt::Regex VERIFIER("^[A-Za-z0-9_-]{43}$", "");
      std::optional<rt::Response> success;
      rt::Reason failure;
      try {
        rt::URL url(req.url());
        const rt::URLSearchParams& params = url.searchParams();
        const std::optional<std::string> state = params.get("state");
        const std::optional<std::string> code = params.get("code");
        const std::optional<std::string> codeVerifier = cookie(req, VERIFIER_COOKIE);
        const json::value codeValue = code ? json::value(*code) : json::value(nullptr);
        if (!state || state->empty() || !STATE.test(*state) || state != cookie(req, STATE_COOKIE) || !codeVerifier || codeVerifier->empty() ||
            !VERIFIER.test(*codeVerifier) || !boundedString(code ? &codeValue : nullptr, 500) || params.getAll("state").size() != 1 || params.getAll("code").size() != 1)
          throw PassportError();
        json::object exchange;
        exchange["code"] = *code;
        exchange["clientId"] = "ai";
        exchange["audience"] = "ai";
        exchange["origin"] = std::string(ORIGIN);
        exchange["redirectUri"] = CALLBACK;
        exchange["codeVerifier"] = *codeVerifier;
        exchange["appSession"] = true;
        rt::RequestInit init;
        init.method = "POST";
        init.headers = rt::Headers{{"Content-Type", "application/json"}, {"Accept", "application/json"}};
        init.body = rt::BodyInit(rt::jsonStringify(json::value(exchange)));
        init.redirect = "error";
        init.signal = rt::AbortSignal::timeout(10'000);
        const std::string url_ = std::string(ISSUER) + "/api/auth/sso/exchange";
        auto call = rt::fetch(rt::RequestInfo(url_), std::move(init));
        const rt::Response response = co_await std::move(call);
        if (response.status() >= 400 && response.status() < 500) {
          auto cancelling = cancelBody(response);
          co_await std::move(cancelling);
          throw PassportError();
        }
        auto reading = boundedJson(response, 65'536);
        const json::value body = co_await std::move(reading);
        const std::optional<std::string_view> accessToken = rt::getString(rt::get(body, "accessToken"));
        if (!utils::isRecord(body) || !accessToken) throw PassportError();
        auto verifying = verifyToken(std::string(*accessToken));
        const PassportClaims claims = co_await std::move(verifying);
        if (!claims.agentTokenId.isNullish()) throw PassportError();
        const json::value* appSession = rt::get(body, "appSession");
        const json::value grant = appSession ? *appSession : json::value(nullptr);
        auto creating = passport_sessions::createPassportSession(grant, claims.sub, cookie(req, SESSION_COOKIE));
        const std::string token = co_await std::move(creating);
        success = redirect(rt::endsWith(*state, ".admin") ? "/admin" : "/chat", {sessionCookie(token), clearStateCookie(), clearVerifierCookie()});
      } catch (...) {
        failure = std::current_exception();
      }
      if (success) co_return *success;
      rt::Response response = errorResponse(failure);
      response.headers().append("Set-Cookie", clearStateCookie());
      response.headers().append("Set-Cookie", clearVerifierCookie());
      co_return response;
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PassportAuthMethod::User& value) {
    out = Writer()
            .required("sub", value.sub)
            .required("handle", value.handle)
            .required("is_admin", value.is_admin)
            .required("scopes", value.scopes)
            .required("auth", value.auth)
            .orNull("exp", value.exp)
            .orNull("jti", value.jti)
            .orNull("agent_token_id", value.agent_token_id)
            .done();
  }

  PassportAuthMethod::User tag_invoke(const json::value_to_tag<PassportAuthMethod::User>&, const json::value& value) {
    const Reader in(value, "PassportAuthMethod.user");
    return {
      .sub = in.required<std::string>("sub"),
      .handle = in.required<std::string>("handle"),
      .is_admin = in.required<bool>("is_admin"),
      .scopes = in.required<std::vector<std::string>>("scopes"),
      .auth = in.required<std::string>("auth"),
      .exp = in.orNull<double>("exp"),
      .jti = in.orNull<std::string>("jti"),
      .agent_token_id = in.orNull<std::string>("agent_token_id"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PassportAuthMethod& value) {
    out = Writer().required("kind", std::string(PassportAuthMethod::kind)).required("principal", value.principal).required("user", value.user).done();
  }

  PassportAuthMethod tag_invoke(const json::value_to_tag<PassportAuthMethod>&, const json::value& value) {
    const Reader in(value, "PassportAuthMethod");
    if (in.required<std::string>("kind") != PassportAuthMethod::kind) in.fail("kind", rt::get(value, "kind"), "\"passport_session\"");
    return {.principal = in.required<std::string>("principal"), .user = in.required<PassportAuthMethod::User>("user")};
  }

  void resetPassportCacheForTest() {
    jwksCache = std::nullopt;
    rt::memory::setCache(rt::memory::Cache::passport_jwks, 0, 0);
  }

  rt::Response renewPassportSessionCookie(const rt::Request& req, rt::Response response) {
    const std::optional<std::string> token = cookie(req, SESSION_COOKIE);
    if (response.ok() && token && !token->empty() && passport_sessions::isPersistentPassportSession(*token)) response.headers().append("Set-Cookie", sessionCookie(*token));
    return response;
  }

  rt::Task<std::optional<PassportResult>> authenticatePassportSession(rt::Request req) {
    const std::optional<std::string> token = cookie(req, SESSION_COOKIE);
    if (!token) co_return std::nullopt;
    if (!isPassportRequest(req)) co_return PassportResult(PassportResultNotOk{.response = errorResponse(PassportError())});
    std::optional<PassportResultOk> accepted;
    rt::Reason failure;
    try {
      if (passport_sessions::isPersistentPassportSession(*token)) {
        auto reading = passport_sessions::getPassportSession(*token);
        const passport_sessions::PassportSessionIdentity user = co_await std::move(reading);
        accepted = PassportResultOk{
          .method =
            PassportAuthMethod{
              .principal = "passport-user:" + user.subject,
              .user =
                {
                  .sub = user.subject,
                  .handle = user.handle,
                  .is_admin = user.isAdmin,
                  .scopes = user.scopes,
                  .auth = "sso",
                  .exp = std::nullopt,
                  .jti = std::nullopt,
                  .agent_token_id = std::nullopt,
                },
            },
        };
      } else {
        auto verifying = verifyToken(*token);
        const PassportClaims claims = co_await std::move(verifying);
        accepted = PassportResultOk{
          .method =
            PassportAuthMethod{
              .principal = "passport-user:" + claims.sub,
              .user =
                {
                  .sub = claims.sub,
                  .handle = claims.handle.value_or(""),
                  .is_admin = claims.isAdmin,
                  .scopes = claims.scopes.value_or(std::vector<std::string>{}),
                  .auth = claims.auth,
                  .exp = claims.exp,
                  .jti = claims.jti,
                  .agent_token_id = claims.agentTokenId.toOptional(),
                },
            },
        };
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (accepted) co_return PassportResult(std::move(*accepted));
    rt::Response response = errorResponse(failure);
    if (passport_sessions::isPersistentPassportSession(*token) && response.status() == 401) {
      response.headers().append("Set-Cookie", std::string(SESSION_COOKIE) + "=; Max-Age=0" + std::string(COOKIE_ATTRIBUTES));
    }
    co_return PassportResult(PassportResultNotOk{.response = response});
  }

  rt::Task<std::optional<PassportAdminResult>> authenticatePassportAdmin(rt::Request req) {
    if (isPassportBearer(http::getBearerToken(req))) co_return PassportAdminResult(PassportAdminResultNotOk{.response = errorResponse(PassportError())});
    auto authenticating = authenticatePassportSession(req);
    const std::optional<PassportResult> passport = co_await std::move(authenticating);
    if (!passport) co_return std::nullopt;
    if (const PassportResultNotOk* rejected = std::get_if<PassportResultNotOk>(&*passport)) co_return PassportAdminResult(*rejected);
    const PassportAuthMethod& method = std::get<PassportResultOk>(*passport).method;
    const std::optional<rt::Response> rejection = passportAdminRejection(req, method);
    if (rejection) co_return PassportAdminResult(PassportAdminResultNotOk{.response = *rejection});
    co_return PassportAdminResult(PassportAdminResultOk{.token = "", .method = method});
  }

  rt::Task<std::optional<rt::Response>> handlePassportRoute(rt::Request req, std::string path) {
    if (path != "/auth/login" && path != "/auth/callback" && path != "/auth/logout") co_return std::nullopt;
    if (!isPassportRequest(req)) co_return errorResponse(PassportError());
    if (req.method() != (path == "/auth/logout" ? "POST" : "GET")) co_return http::openaiError(405, "Method not allowed", std::string("method_not_allowed"));
    if (path == "/auth/callback") {
      auto handling = callback(req);
      const rt::Response response = co_await std::move(handling);
      co_return response;
    }
    if (path == "/auth/logout") {
      const std::optional<std::string> origin = req.headers().get("origin");
      const std::optional<std::string> site = req.headers().get("sec-fetch-site");
      if ((origin && *origin != ORIGIN) || (site && *site != "same-origin" && *site != "none")) {
        co_return http::openaiError(403, "Forbidden", std::string("forbidden"));
      }
      std::optional<rt::Response> revoked;
      rt::Reason failure;
      try {
        auto revoking = passport_sessions::revokePassportSession(cookie(req, SESSION_COOKIE).value_or(""));
        const std::string upstream = co_await std::move(revoking);
        rt::ResponseInit init;
        init.status = 204;
        init.headers = rt::Headers{{"Set-Cookie", std::string(SESSION_COOKIE) + "=; Max-Age=0" + std::string(COOKIE_ATTRIBUTES)},
                                   {"Cache-Control", "no-store"},
                                   {"X-UOS-Passport-Upstream-Revoke", upstream}};
        revoked = rt::Response(rt::BodyInit(nullptr), std::move(init));
      } catch (...) {
        failure = std::current_exception();
      }
      if (revoked) co_return *revoked;
      co_return errorResponse(failure);
    }
    rt::URL requestUrl(req.url());
    const std::string target = requestUrl.searchParams().get("return_to") == "/admin" ? "admin" : "chat";
    rt::Bytes stateBytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(stateBytes));
    const std::string state = utils::base64UrlEncode(stateBytes) + "." + target;
    rt::Bytes verifierBytes(32);
    rt::crypto::getRandomValues(std::span<std::uint8_t>(verifierBytes));
    const std::string verifier = utils::base64UrlEncode(verifierBytes);
    const rt::Bytes digest = rt::crypto::subtle::digest("SHA-256", rt::encodeUtf8(verifier));
    co_await rt::microtask();
    const std::string challenge = utils::base64UrlEncode(digest);
    rt::URL authorize(std::string(ISSUER) + "/authorize");
    const std::array<std::pair<std::string_view, std::string>, 5> parameters{{
      {"client_id", "ai"},
      {"audience", "ai"},
      {"origin", std::string(ORIGIN)},
      {"redirect_uri", CALLBACK},
      {"state", state},
    }};
    for (const auto& [name, value] : parameters) authorize.searchParams().set(name, value);
    authorize.searchParams().set("code_challenge", challenge);
    authorize.searchParams().set("code_challenge_method", "S256");
    co_return redirect(authorize.toString(), {std::string(STATE_COOKIE) + "=" + state + "; Max-Age=600" + std::string(COOKIE_ATTRIBUTES),
                                              std::string(VERIFIER_COOKIE) + "=" + verifier + "; Max-Age=600" + std::string(COOKIE_ATTRIBUTES)});
  }
}
