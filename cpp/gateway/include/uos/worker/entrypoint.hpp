#pragma once
#include "uos/authority/authority_rpc.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/authority/delivery_outbox.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/worker/transport.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace uos::worker::entrypoint {
  using authority::authority_rpc::AuthorityOperations;
  using authority::contracts::AuthorityScope;
  using authority::contracts::DeliveryOutcome;
  using transport::UpstreamResult;
  using transport::UpstreamTransport;

  struct BoundaryPrincipal {
    std::string principalId;
    double policyVersion;
  };
  struct BoundaryLifetime {
    // `waitUntil(task: Promise<unknown>)`
    std::function<void(rt::Promise<void> task)> waitUntil{};
  };
  struct BoundaryDelivery {
    rt::Promise<DeliveryOutcome> completed;
  };
  struct WorkerBoundaryDependencies {
    AuthorityScope scope;
    std::shared_ptr<AuthorityOperations> authority;
    std::shared_ptr<UpstreamTransport> upstream;
    std::function<rt::Task<std::optional<BoundaryPrincipal>>(rt::Request request)> authenticate{};
    std::function<double()> now{};
  };

  // The object createWorkerBoundary returns: `{ fetch(request, lifetime, delivery?) }`.
  class WorkerBoundary {
  public:
    explicit WorkerBoundary(std::shared_ptr<const WorkerBoundaryDependencies> dependencies_);
    // `fetch(request, lifetime)` / `fetch(request, lifetime, delivery)`
    rt::Task<rt::Response> fetch(rt::Request request, BoundaryLifetime lifetime) const;
    rt::Task<rt::Response> fetch(rt::Request request, BoundaryLifetime lifetime, BoundaryDelivery delivery) const;

  private:
    std::shared_ptr<const WorkerBoundaryDependencies> dependencies;
  };

  /** Native-free factory; no runtime binding, configuration, credential source or live route is installed. */
  WorkerBoundary createWorkerBoundary(WorkerBoundaryDependencies dependencies);
}
