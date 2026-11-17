#pragma once
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

namespace uos::provider::removed_provider_telemetry {
  rt::Promise<void> recordRemovedProviderTelemetry(const json::object& _input);
}
