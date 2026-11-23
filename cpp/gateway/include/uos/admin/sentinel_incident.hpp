#pragma once
// Port of src/admin/sentinel-incident.ts.
#include "uos/sentinel/incident_outbox.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"

#include <functional>
#include <memory>

namespace uos::admin::sentinel_incident {
  // `Readonly<{ getKv?: typeof getKv; listSentinelIncidentIndexRows?: typeof listSentinelIncidentIndexRows }>`:
  // an empty function is undefined. `typeof getKv` resolves to a null pointer for null.
  struct SentinelIncidentAdminDependencies {
    std::function<rt::Promise<std::shared_ptr<rt::Kv>>()> getKv{};
    std::function<rt::Task<sentinel::incident_outbox::SentinelIncidentIndexPage>(rt::Kv& kv, sentinel::incident_outbox::SentinelIncidentIndexListOptions options)>
      listSentinelIncidentIndexRows{};
  };

  // The overload without `dependencies` is `{}`.
  [[nodiscard]] rt::Task<rt::Response> handleAdminSentinelIncidents(rt::Request req);
  [[nodiscard]] rt::Task<rt::Response> handleAdminSentinelIncidents(rt::Request req, SentinelIncidentAdminDependencies dependencies);
}
