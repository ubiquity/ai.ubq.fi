#pragma once
// Port of src/live/calls.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <string>

namespace uos::live::calls {
  rt::Task<rt::Response> handleLiveCallCreate(rt::Request req, std::string principal);
}
