#include "uos/worker/transport.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/url.hpp"

#include <utility>

namespace uos::worker::transport {
  using authority::contracts::AuthorityError;

  namespace {
    rt::Task<UpstreamAttempt> resolved(UpstreamAttempt attempt) { co_return attempt; }
  }

  HttpUpstreamTransport::HttpUpstreamTransport(HttpUpstreamCapability capability_) : _capability(std::move(capability_)) {
    const rt::URL base(_capability.baseUrl);
    const std::string protocol = base.protocol();
    if ((protocol != "http:" && protocol != "https:") || !base.username().empty() || !base.password().empty() || !base.search().empty() || !base.hash().empty() ||
        base.pathname() != "/")
      throw AuthorityError("invalid_fixed_upstream", 400);
  }

  rt::Task<ValidatedGatewayInput> HttpUpstreamTransport::validate(GatewayRoute route, json::object body) {
    rt::Task<ValidatedGatewayInput> validating = _capability.validate(route, std::move(body));
    ValidatedGatewayInput input = co_await std::move(validating);
    const std::optional<std::string_view> bodyModel = rt::getString(input.body, "model");
    if (input.route != route || input.model.empty() || !bodyModel || *bodyModel != input.model) throw AuthorityError("invalid_validated_request", 400);
    co_return input;
  }

  rt::Task<UpstreamAttempt> HttpUpstreamTransport::attempt(ValidatedGatewayInput, std::optional<AccountBinding> account) {
    if (_capability.provider == "codex" && !account) throw AuthorityError("subscription_unavailable", 503);
    return resolved(UpstreamAttempt{
      .provider = _capability.provider,
      .account = _capability.provider == "codex" ? account : std::nullopt,
      .maximumExposureMicrocredits = _capability.maximumExposureMicrocredits,
    });
  }

  rt::Task<UpstreamResult> HttpUpstreamTransport::dispatch(ValidatedGatewayInput input, UpstreamAttempt attempt, rt::AbortSignal signal,
                                                           std::function<rt::Task<void>()> onDispatched) {
    if (attempt.provider != _capability.provider) throw AuthorityError("provider_capability_mismatch", 403);
    rt::Task<rt::Headers> building = _capability.headers(attempt.account);
    rt::Headers headers = co_await std::move(building);
    headers.set("content-type", "application/json");
    rt::RequestInit init;
    init.method = "POST";
    init.headers = headers;
    init.body = rt::BodyInit(rt::jsonStringify(input.body));
    init.signal = signal;
    init.redirect = "manual";
    rt::Request request(rt::URL(input.route, _capability.baseUrl), std::move(init));
    signal.throwIfAborted();
    rt::Task<void> dispatching = onDispatched();
    co_await std::move(dispatching);
    // Once dispatch intent is durable, ambiguous fetch failures keep accounting pending.
    const auto fetchUpstream = _capability.fetch;
    rt::Task<rt::Response> fetching = fetchUpstream(request);
    rt::Response response = co_await std::move(fetching);
    if (response.status() >= 300 && response.status() < 400) {
      if (std::optional<rt::ByteStream> body = response.body()) {
        rt::Task<void> cancelling = body->cancel();
        co_await std::move(cancelling);
      } else {
        co_await rt::microtask();
      }
      throw AuthorityError("upstream_redirect_refused", 502);
    }
    // Observe one streaming body; cloning would allow a faster terminal reader to buffer unbounded data.
    co_return _capability.observe(response, input);
  }
}
