#pragma once
// Portable HTTP portion of tests/helpers/responses-conformance-client.ts; installed Codex execution is outside this wave.
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/env.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace uos::tests::helpers::responses_conformance_client {
  // Source-relative assets are staged only inside the isolated wrapper workdir; the repository remains read-only.
  inline void stageGatewayAssets() {
    struct Assets {
      std::vector<std::string> created;
      Assets() {
        const auto temporary=rt::env::get("TMPDIR");const auto directory=rt::process::cwd();
        if(!temporary||!directory.starts_with(rt::path::dirname(*temporary)+"/"))throw rt::JsError("Gateway assets require the wrapper task workdir");
        const auto root=rt::path::resolve(rt::path::dirname(__FILE__),"../../../..");
        const std::array<std::string,3> names{"static","favicon.png","favicon-32.png"};
        for(const auto& name:names){const auto target=rt::path::join(root,name);const auto destination=rt::path::join(directory,name);if(rt::fs::existsSync(destination)){if(rt::fs::realpathSync(destination)!=rt::fs::realpathSync(target))throw rt::JsError("Gateway asset fixture refuses an unexpected existing path");}else{rt::fs::symlinkSync(target,destination);created.push_back(destination);}}
      }
      ~Assets(){for(const auto& path:created)rt::fs::rmSync(path);}
    };
    static Assets assets;
    (void)assets;
  }
  struct ConformanceLimits { std::size_t bytes; std::size_t events; double deadlineMs; };
  inline constexpr ConformanceLimits CONFORMANCE_LIMITS{262'144,256,5'000};
  inline const std::string CONFORMANCE_CALL_ID="call_fixture_echo_1";
  inline const std::string CONFORMANCE_ARGUMENTS=R"({"text":"fixture-argument-ok"})";
  inline const std::string CONFORMANCE_RESULT="fixture-tool-ok";
  inline const std::string CONFORMANCE_FINAL="fixture-final-ok";
  using ConformanceEvent=json::object;
  struct ConformanceMarker { std::string kind; double atMs; ConformanceEvent detail; };
  class ConformanceJournal : public std::enable_shared_from_this<ConformanceJournal> {
  public:
    std::vector<ConformanceMarker> entries;
    void add(std::string kind, ConformanceEvent detail={});
    rt::Promise<ConformanceMarker> wait(std::function<bool(const ConformanceMarker&)> predicate, double timeoutMs=CONFORMANCE_LIMITS.deadlineMs);
  private:
    struct Waiter { std::size_t id; std::function<bool(const ConformanceMarker&)> predicate; rt::Resolve<ConformanceMarker> resolve; rt::Timeout timer; };
    std::vector<Waiter> waiters;
    std::size_t nextId=0;
  };
  std::shared_ptr<ConformanceJournal> createConformanceJournal();
  ConformanceEvent conformanceRecord(const json::value& value);
  json::object conformanceHeaders(const rt::Headers& headers);
  rt::URL conformanceEndpoint(const std::string& endpoint, const std::string& path="/v1/responses");
  struct ConformanceWireCapture {
    struct Request { std::string path; std::string body; } request;
    int status=0;
    json::object headers;
    std::vector<std::string> chunksBase64;
    std::vector<ConformanceEvent> events;
    std::string outcome="open";
    std::optional<std::string> error{};
  };
  json::value toJson(const ConformanceWireCapture& capture);
  struct ConformanceResponse {
    std::shared_ptr<ConformanceWireCapture> capture;
    rt::Promise<ConformanceWireCapture> finished;
    std::function<rt::Promise<ConformanceEvent>(std::string)> waitFor;
    std::function<void()> disconnect;
  };
  rt::Task<std::shared_ptr<ConformanceResponse>> openConformanceResponse(std::string endpoint,std::string token,ConformanceEvent body);
  struct ConformanceRequestOptions { std::optional<std::string> model{}; std::optional<std::string> namespace_{}; };
  ConformanceEvent conformanceRequest(ConformanceRequestOptions options={});
  ConformanceEvent completedConformanceResponse(const ConformanceWireCapture& capture);
  ConformanceEvent conformanceContinuation(const ConformanceEvent& first,const ConformanceWireCapture& capture,const json::value& output=json::value(CONFORMANCE_RESULT));
  struct ConformanceRoundTripOptions { std::string endpoint; std::string token; std::optional<ConformanceEvent> request{}; std::function<void(int,const ConformanceEvent&)> acknowledge; };
  rt::Task<std::vector<ConformanceWireCapture>> runConformanceRoundTrip(ConformanceRoundTripOptions options);
  json::value normalizeConformanceValue(const json::value& value);
}
