#pragma once
#include "uos/authority/authority_rpc.hpp"
#include "uos/authority/contracts.hpp"
#include "uos/rt/async.hpp"

#include <memory>
#include <string>

namespace uos::authority::delivery_outbox {
  using authority_rpc::AuthorityOperations;
  using contracts::AdmissionHandle;
  using contracts::DeliveryOutcome;
  using contracts::TerminalEvidence;

  /** Durable observations are individually replayable after a lost response/acknowledgment. */
  class DeliveryOutbox {
  public:
    DeliveryOutbox(std::shared_ptr<AuthorityOperations> authority_, AdmissionHandle handle_);

    rt::Task<void> terminal(std::string attemptId, TerminalEvidence evidence, bool finalAttempt = true);
    rt::Task<void> delivery(DeliveryOutcome outcome);

  private:
    const std::shared_ptr<AuthorityOperations> _authority;
    const AdmissionHandle _handle;
  };
}
