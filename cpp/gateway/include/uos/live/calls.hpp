#pragma once
// Port of src/live/calls.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <memory>
#include <optional>
#include <string>

namespace uos::live::calls {
  struct FailedLiveCallCapture {
    std::string requestId;
    std::optional<rt::Promise<void>> deliveryCompleted;
    std::shared_ptr<bool> projected;
  };

  rt::Task<rt::Response> handleLiveCallCreate(rt::Request req, std::string principal,
                                             std::optional<FailedLiveCallCapture> capture = std::nullopt);
}
