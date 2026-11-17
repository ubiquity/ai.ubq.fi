#pragma once
// Test-only synthetic authority and real HTTP fixture from tests/helpers/responses-conformance-fixture.ts.
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/inference_admission.hpp"
#include "uos/runtime/http.hpp"
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace uos::tests::helpers::responses_conformance_fixture {
  namespace client=responses_conformance_client;
  inline const std::string CONFORMANCE_SOURCE_SHA="d94fef52ccc76e0d5aa019154f11e47d5e10fba6";
  struct FixtureOptions {
    std::optional<std::string> toolName{};
    std::optional<bool> holdToolDelta{};
    std::optional<double> usageLimit{};
    std::optional<std::string> captureDirectory{};
    std::optional<std::string> providerOrigin{};
    std::optional<double> maxDispatches{};
  };
  struct RecordedRequest {
    std::string path;
    std::string body;
    json::object headers;
    struct Response { int status;json::object headers;std::vector<std::string> chunksBase64; };
    std::optional<Response> response{};
  };
  struct ConformanceMock : std::enable_shared_from_this<ConformanceMock> {
    std::vector<RecordedRequest> calls;
    std::shared_ptr<client::ConformanceJournal> journal;
    std::string origin;
    std::function<void(int,const client::ConformanceEvent&)> receipt;
    std::function<rt::Task<void>()> stop;
    struct State;
    std::shared_ptr<State> state;
  };
  rt::Task<std::shared_ptr<ConformanceMock>> startConformanceMock(FixtureOptions options={},std::shared_ptr<client::ConformanceJournal> journal={});
  struct ResponsesConformanceFixture : std::enable_shared_from_this<ResponsesConformanceFixture> {
    std::string endpoint;
    std::string token;
    std::shared_ptr<client::ConformanceJournal> journal;
    std::vector<RecordedRequest> inbound;
    std::vector<client::ConformanceEvent> terminals;
    std::vector<RecordedRequest>& calls;
    inference_admission::InferenceAdmissionController controller;
    json::object storage;
    json::object expected;
    explicit ResponsesConformanceFixture(std::shared_ptr<ConformanceMock> upstream);
    rt::Task<json::object> ledger(std::string requestId);
    rt::Task<void> save(json::value captures);
    rt::Task<void> save(const std::vector<client::ConformanceWireCapture>& captures);
    rt::Task<void> save(const client::ConformanceWireCapture& capture);
    client::ConformanceEvent verifyReopenedLedger() const;
    void acknowledge(int turn,const client::ConformanceEvent& event);
    rt::Task<void> revoke();
    rt::Task<void> stop();
    struct State;
    std::shared_ptr<State> state;
    std::shared_ptr<ConformanceMock> upstream;
  };
  rt::Task<std::shared_ptr<ResponsesConformanceFixture>> startResponsesConformanceFixture(FixtureOptions options={});
}
