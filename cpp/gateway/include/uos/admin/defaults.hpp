#pragma once
// Port of src/admin/defaults.ts.
#include "uos/metered_quota.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <functional>
namespace uos::admin::defaults {
  rt::Task<rt::Response> handleAdminKvMigrationImport(rt::Request req);
  rt::Task<rt::Response> handleAdminKvMigrationValidate();
  struct AdminDefaultsDependencies { std::function<rt::Task<metered_quota::MeteredQuotaDiagnostics>()> getMeteredQuotaDiagnostics{}; };
  rt::Task<rt::Response> handleAdminDefaults(rt::Request req);
  rt::Task<rt::Response> handleAdminDefaults(rt::Request req, AdminDefaultsDependencies dependencies);
}
