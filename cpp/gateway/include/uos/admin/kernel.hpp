#pragma once
// Port of src/admin/kernel.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"

namespace uos::admin::kernel {
  rt::Task<rt::Response> handleAdminKernelPubKeysList();
  rt::Task<rt::Response> handleAdminKernelPubKeysCreate(rt::Request req);
  rt::Task<rt::Response> handleAdminKernelPubKeysDelete(rt::Request req);
  rt::Task<rt::Response> handleAdminKernelPolicyQueueList();
  rt::Task<rt::Response> handleAdminKernelUsageGet(rt::Request req);
  rt::Task<rt::Response> handleAdminKernelUsageSet(rt::Request req);
  rt::Task<rt::Response> handleAdminKernelUsageDelete(rt::Request req);
  rt::Task<rt::Response> handleAdminProvidersQuotaProjection();
  rt::Task<rt::Response> handleAdminProvidersQuotaProjection(rt::Request request);
  rt::Task<rt::Response> handleAdminProvidersQuotaProjectionBackfill();
  rt::Task<rt::Response> handleAdminProvidersQuotaProjectionBackfill(rt::Request request);
  rt::Task<rt::Response> handleAdminCodexResetSettings(rt::Request request);
  rt::Task<rt::Response> handleAdminCodexOverageUsage(rt::Request request);
}
