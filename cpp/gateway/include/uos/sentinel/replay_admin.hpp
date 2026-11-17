#pragma once
// Port of src/sentinel/replay-admin.ts.
#include "uos/sentinel/replay_read.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"

#include <functional>
#include <memory>
#include <string>

namespace uos::sentinel::replay_admin {
  // `Readonly<{ getKv?: typeof getKv; listEncryptedSentinelReplays?: ...; listEncryptedSentinelIncidentReplays?: ...;
  // listEncryptedSentinelReplaysByRequestId?: ... }>`: an empty function is undefined. `typeof getKv` resolves to a
  // null pointer for null. The request-id lister is called as the handler calls it, without `nowMs`.
  struct SentinelReplayAdminDependencies {
    std::function<rt::Promise<std::shared_ptr<rt::Kv>>()> getKv{};
    std::function<rt::Task<replay_read::EncryptedSentinelReplayPage>(rt::Kv& kv, replay_read::ListEncryptedSentinelReplaysOptions options)>
      listEncryptedSentinelReplays{};
    std::function<rt::Task<replay_read::EncryptedSentinelReplayPage>(rt::Kv& kv, replay_read::ListEncryptedSentinelIncidentReplaysOptions options)>
      listEncryptedSentinelIncidentReplays{};
    std::function<rt::Task<replay_read::EncryptedSentinelRequestReplays>(rt::Kv& kv, std::string requestId)> listEncryptedSentinelReplaysByRequestId{};
  };

  // The overload without `dependencies` is `{}`. Both are plain functions returning the handler's task (the
  // dependencies are copied into it), so `co_await handleAdminSentinelReplayCaptures(req, dependencies)` is safe
  // under the GCC 12.2 coroutine rules.
  [[nodiscard]] rt::Task<rt::Response> handleAdminSentinelReplayCaptures(rt::Request req);
  [[nodiscard]] rt::Task<rt::Response> handleAdminSentinelReplayCaptures(rt::Request req, const SentinelReplayAdminDependencies& dependencies);
}
