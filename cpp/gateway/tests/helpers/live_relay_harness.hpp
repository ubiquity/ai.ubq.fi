#pragma once
// w05c5's live-module fixture: isolated synthetic accounts, an in-memory store, and mocked provider fetch.
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/config.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/utils.hpp"
#include "uos/kv.hpp"
#include "uos/live/upstream.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/types.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::tests::helpers::live_relay_harness {
  inline constexpr double NOW = 1'700'000'000'000;
  inline const std::string CLIENT_TOKEN="u_"+std::string(64,'c');
  inline constexpr std::string_view ACCOUNT_A="live-calls-http-account-a";
  inline constexpr std::string_view ACCOUNT_B="live-calls-http-account-b";
  inline constexpr std::string_view CALL_ID="rtc_live_calls_http";
  inline constexpr std::string_view PRINCIPAL="api-key:live-calls-http-key";
  inline constexpr std::string_view BOUNDARY="codex-realtime-call-boundary";
  inline constexpr std::string_view SDP_OFFER="v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
  inline constexpr std::string_view SDP_ANSWER="v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
  inline json::object session(){return {{"model","gpt-live-1-codex"},{"instructions","be brief"},{"delegation",json::object{{"type","client"}}},{"audio",json::object{{"output",json::object{{"voice","cove"}}}}}};}
  inline ::uos::types::CodexAuthState codexAccount(const std::string& id){return {.access_token=id+"-access-token",.refresh_token=id+"-refresh-token",.account_id=id,.updated_at_ms=NOW};}
  inline ::uos::types::CodexAuthState expiredCodexAccount(const std::string& id){auto result=codexAccount(id);result.access_token=rt::toBase64Url(rt::encodeUtf8("{\"alg\":\"none\"}"))+"."+rt::toBase64Url(rt::encodeUtf8("{\"exp\":1699999940}"))+".signature";return result;}
  inline std::string realtimeCallBody(const std::string& sdp,const json::value& value){return "--"+std::string(BOUNDARY)+"\r\nContent-Disposition: form-data; name=\"sdp\"\r\nContent-Type: application/sdp\r\n\r\n"+sdp+"\r\n--"+std::string(BOUNDARY)+"\r\nContent-Disposition: form-data; name=\"session\"\r\nContent-Type: application/json\r\n\r\n"+rt::jsonStringify(value)+"\r\n--"+std::string(BOUNDARY)+"--\r\n";}
  inline rt::Request callRequest(const std::string& body,rt::Headers headers={}){
    if(!headers.has("content-type"))headers.set("content-type","multipart/form-data; boundary="+std::string(BOUNDARY));
    headers.set("authorization","Bearer "+CLIENT_TOKEN);
    rt::RequestInit init;init.method="POST";init.headers=headers;init.body=body;return rt::Request("http://fixture.invalid/v1/live",init);
  }
  inline rt::Request callRequest(){return callRequest(realtimeCallBody(std::string(SDP_OFFER),session()));}
  inline rt::Response acceptedAnswer(){return rt::Response(std::string(SDP_ANSWER),rt::ResponseInit{.status=201,.headers=rt::Headers{{"Location","/v1/realtime/calls/"+std::string(CALL_ID)}}});}
  struct UpstreamCall { std::string url;rt::Headers headers;std::string bodyText; };
  struct Fixture {
    std::optional<std::string> scratch=rt::env::get("TMPDIR");
    bool savedDeploy=config::config.isDeploy;
    rt::env::ScopedEnvironment clean{{{"HOME","/nonexistent/w05c5"}}};
    std::string savedAuth=config::config.codexAuthJsonB64;
    rt::console::Capture logging{{rt::console::Method::info,rt::console::Method::warn}};
    std::shared_ptr<rt::MemoryKv> kv=std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    std::vector<UpstreamCall> calls;
    std::vector<std::string> refreshTokens;
    std::function<rt::Task<rt::Response>(UpstreamCall)> responder=[](UpstreamCall){return rt::fulfilledTask(acceptedAnswer());};
    std::function<rt::Task<rt::Response>(std::string)> refresh=[](std::string){return rt::fulfilledTask(rt::Response::json(json::object{{"access_token","live-calls-refreshed-access-token"},{"refresh_token","live-calls-refreshed-refresh-token"}}));};
    std::unique_ptr<rt::GlobalFetchOverride> fetch;
    Fixture(){
      if(scratch)rt::env::set("TMPDIR",*scratch);
      config::config.isDeploy=true;api_key_policy::resetApiKeyPolicyCacheForTest();
      config::config.codexAuthJsonB64.clear();rt::globalDateNow()=[] {return NOW;};::uos::kv::setKvForTest(kv);
      codex::auth::resetCodexAuthCacheForTest();codex::account_routing::resetCodexAccountRoutingForTest();models::codex_models_availability::resetCodexAccountModelsCacheForTest();provider::health::resetProviderHealthThrottleForTest();
      provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const provider::capacity_events::ProviderCapacitySampleTriggerOptions&){});
      live::upstream::setLiveUpstreamBasesForTest({.callsBaseUrl="http://live-provider.invalid"});
      fetch=std::make_unique<rt::GlobalFetchOverride>(rt::Fetch([this](rt::RequestInfo info,rt::RequestInit init)->rt::Task<rt::Response>{
        std::string url;if(const auto* text=std::get_if<std::string>(&info))url=*text;else if(const auto* value=std::get_if<rt::URL>(&info))url=value->href();else url=std::get<rt::Request>(info).url();
        std::string body;if(init.body)if(const auto* text=std::get_if<std::string>(&*init.body))body=*text;
        if(url==codex::auth::CODEX_REFRESH_TOKEN_URL){const auto payload=rt::jsonParse(body);const auto token=std::string(rt::getString(payload,"refresh_token").value_or(""));refreshTokens.push_back(token);auto task=refresh(token);auto result=co_await std::move(task);co_return result;}
        if(!rt::startsWith(url,"http://live-provider.invalid/"))throw rt::JsError("unmapped provider URL refused by fixture");
        UpstreamCall call{url,init.headers.value_or(rt::Headers()),body};calls.push_back(call);auto task=responder(call);auto result=co_await std::move(task);co_return result;
      }));
    }
    ~Fixture(){fetch.reset();live::upstream::setLiveUpstreamBasesForTest({});provider::capacity_events::setProviderCapacitySampleTriggerForTest({});provider::health::resetProviderHealthThrottleForTest();codex::auth::resetCodexAuthCacheForTest();codex::account_routing::resetCodexAccountRoutingForTest();models::codex_models_availability::resetCodexAccountModelsCacheForTest();::uos::kv::setKvForTest(nullptr);config::config.codexAuthJsonB64=savedAuth;config::config.isDeploy=savedDeploy;api_key_policy::resetApiKeyPolicyCacheForTest();}
    rt::Task<void> setAuthPool(std::vector<::uos::types::CodexAuthState> accounts){const ::uos::types::CodexAuthPoolState pool{.accounts=std::move(accounts),.updated_at_ms=rt::dateNow()};const rt::KvValue value(json::value_from(pool));auto task=kv->set(codex::auth::CODEX_AUTH_POOL_KV_KEY,value);co_await std::move(task);codex::auth::resetCodexAuthCacheForTest();}
    rt::Task<void> seedApiKey(){
      const std::string token=CLIENT_TOKEN;auto hashing=utils::sha256Base64Url(token);const auto hash=co_await std::move(hashing);
      json::object common{{"expires_at_ms",-1},{"revoked_at_ms",nullptr},{"usage_limit_requests",-1},{"usage_requests",0},{"usage_reset_at_ms",NOW+3'600'000},{"window_ms",3'600'000},{"usage_quota_version",3},{"paid_fallback_enabled",false},{"paid_fallback_limit_microcredits",0},{"paid_fallback_spent_microcredits",0},{"paid_fallback_reserved_microcredits",0},{"paid_fallback_reservation_request_id",nullptr}};
      auto key=common;key["id"]="live-calls-http-key";key["name"]="Live calls HTTP key";key["prefix"]=token.substr(0,10);key["hash"]=hash;key["created_at_ms"]=NOW;key["paid_fallback_model_ids"]=json::array{};key["paid_fallback_quota_per_credit"]=0;key["paid_fallback_max_exposure_microcredits"]=json::object{};key["paid_fallback_pricing_checked_at_ms"]=NOW;
      common["id"]="live-calls-http-key";const rt::KvValue keyValue(key),hashValue(common);auto writing=kv->set(rt::KvKey{"ubq_ai","api_keys","id","live-calls-http-key"},keyValue);co_await std::move(writing);auto writingHash=kv->set(rt::KvKey{"ubq_ai","api_keys","hash",hash},hashValue);co_await std::move(writingHash);
    }
    rt::Task<void> seed(){auto key=seedApiKey();co_await std::move(key);std::vector<::uos::types::CodexAuthState> accounts{codexAccount(std::string(ACCOUNT_A)),codexAccount(std::string(ACCOUNT_B))};auto task=setAuthPool(accounts);co_await std::move(task);}
    rt::Task<void> settle(){auto task=codex::dispatch::awaitPendingCodexProbeTransitions();co_await std::move(task);}
  };
}
