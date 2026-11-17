#include "uos/authority/delivery_outbox.hpp"

#include <utility>

namespace uos::authority::delivery_outbox {
  namespace {
    // The operations run on copies of the capability and handle, so a pending
    // observation does not depend on the outbox object staying alive.
    rt::Task<void> observeTerminal(std::shared_ptr<AuthorityOperations> authority, AdmissionHandle handle, std::string attemptId, TerminalEvidence evidence,
                                   bool finalAttempt) {
      rt::Task<void> observing = authority->observeTerminal(handle, std::move(attemptId), std::move(evidence), finalAttempt);
      co_await std::move(observing);
      rt::Task<std::string> settling = authority->settle(handle);
      co_await std::move(settling);
    }

    rt::Task<void> observeDelivery(std::shared_ptr<AuthorityOperations> authority, AdmissionHandle handle, DeliveryOutcome outcome) {
      rt::Task<void> observing = authority->observeDelivery(handle, std::move(outcome));
      co_await std::move(observing);
      rt::Task<std::string> settling = authority->settle(handle);
      co_await std::move(settling);
    }
  }

  DeliveryOutbox::DeliveryOutbox(std::shared_ptr<AuthorityOperations> authority_, AdmissionHandle handle_) : _authority(std::move(authority_)), _handle(std::move(handle_)) {}

  rt::Task<void> DeliveryOutbox::terminal(std::string attemptId, TerminalEvidence evidence, bool finalAttempt) {
    return observeTerminal(_authority, _handle, std::move(attemptId), std::move(evidence), finalAttempt);
  }

  rt::Task<void> DeliveryOutbox::delivery(DeliveryOutcome outcome) { return observeDelivery(_authority, _handle, std::move(outcome)); }
}
