#pragma once
#include "uos/authority/contracts.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>

namespace uos::worker::transport {
  using authority::contracts::AccountBinding;
  using authority::contracts::DispatchProvider;
  using authority::contracts::GatewayRoute;
  using authority::contracts::TerminalEvidence;

  struct ValidatedGatewayInput {
    GatewayRoute route;
    std::string model;
    bool stream;
    json::object body;
  };
  struct UpstreamAttempt {
    DispatchProvider provider;
    std::optional<AccountBinding> account;
    double maximumExposureMicrocredits;
  };
  struct UpstreamResult {
    rt::Response response;
    rt::Promise<TerminalEvidence> terminal;
  };
  /** Each implementation pins destinations, provider translation, credentials and schema/catalog policy. */
  class UpstreamTransport {
  public:
    virtual ~UpstreamTransport() = default;
    virtual rt::Task<ValidatedGatewayInput> validate(GatewayRoute route, json::object body) = 0;
    virtual rt::Task<UpstreamAttempt> attempt(ValidatedGatewayInput input, std::optional<AccountBinding> account) = 0;
    /** Resolving onDispatched is mandatory immediately before the actual provider fetch. */
    virtual rt::Task<UpstreamResult> dispatch(ValidatedGatewayInput input, UpstreamAttempt attempt, rt::AbortSignal signal, std::function<rt::Task<void>()> onDispatched) = 0;
  };

  // The fixed capability `new HttpUpstreamTransport(capability)` is built from.
  struct HttpUpstreamCapability {
    DispatchProvider provider;
    std::function<rt::Task<rt::Response>(rt::Request request)> fetch;
    std::string baseUrl;
    std::function<rt::Task<rt::Headers>(std::optional<AccountBinding> account)> headers;
    std::function<rt::Task<ValidatedGatewayInput>(GatewayRoute route, json::object body)> validate;
    std::function<UpstreamResult(rt::Response response, ValidatedGatewayInput input)> observe;
    double maximumExposureMicrocredits;
  };

  /** Fixed HTTP upstream capability for official Responses/chat pass-through fixtures/adapters. */
  class HttpUpstreamTransport : public UpstreamTransport {
  public:
    explicit HttpUpstreamTransport(HttpUpstreamCapability capability_);

    rt::Task<ValidatedGatewayInput> validate(GatewayRoute route, json::object body) override;
    rt::Task<UpstreamAttempt> attempt(ValidatedGatewayInput input, std::optional<AccountBinding> account) override;
    rt::Task<UpstreamResult> dispatch(ValidatedGatewayInput input, UpstreamAttempt attempt, rt::AbortSignal signal, std::function<rt::Task<void>()> onDispatched) override;

  private:
    const HttpUpstreamCapability _capability;
  };
}
