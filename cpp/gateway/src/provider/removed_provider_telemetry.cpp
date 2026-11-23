#include "uos/provider/removed_provider_telemetry.hpp"

namespace uos::provider::removed_provider_telemetry {
  rt::Promise<void> recordRemovedProviderTelemetry([[maybe_unused]] const json::object& _input) { return rt::Promise<void>::resolve(); }
}
