#include "uos/authority/native_kernel_contracts.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/record.hpp"

#include <string>

namespace uos::authority::native_kernel_contracts {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaPolicyV2& value) {
    out = Writer()
      .required("v", value.v)
      .required("scope", value.scope)
      .required("owner", value.owner)
      .orUndefined("repo", value.repo)
      .required("usage_limit_requests", value.usage_limit_requests)
      .required("window_ms", value.window_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelQuotaPolicyV2 tag_invoke(const json::value_to_tag<KernelQuotaPolicyV2>&, const json::value& value) {
    const Reader in(value, "KernelQuotaPolicyV2");
    return {
      .v = in.required<double>("v"),
      .scope = in.required<std::string>("scope"),
      .owner = in.required<std::string>("owner"),
      .repo = in.orUndefined<std::string>("repo"),
      .usage_limit_requests = in.required<double>("usage_limit_requests"),
      .window_ms = in.required<double>("window_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaWindowV2& value) {
    out = Writer()
      .required("v", value.v)
      .required("scope", value.scope)
      .required("owner", value.owner)
      .orUndefined("repo", value.repo)
      .required("usage_requests", value.usage_requests)
      .required("reserved_requests", value.reserved_requests)
      .required("usage_reset_at_ms", value.usage_reset_at_ms)
      .required("applied_window_ms", value.applied_window_ms)
      .required("created_at_ms", value.created_at_ms)
      .required("updated_at_ms", value.updated_at_ms)
      .done();
  }

  KernelQuotaWindowV2 tag_invoke(const json::value_to_tag<KernelQuotaWindowV2>&, const json::value& value) {
    const Reader in(value, "KernelQuotaWindowV2");
    return {
      .v = in.required<double>("v"),
      .scope = in.required<std::string>("scope"),
      .owner = in.required<std::string>("owner"),
      .repo = in.orUndefined<std::string>("repo"),
      .usage_requests = in.required<double>("usage_requests"),
      .reserved_requests = in.required<double>("reserved_requests"),
      .usage_reset_at_ms = in.required<double>("usage_reset_at_ms"),
      .applied_window_ms = in.required<double>("applied_window_ms"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelQuotaReservationRowV2& value) {
    out = Writer()
      .required("v", value.v)
      .required("scope", value.scope)
      .required("owner", value.owner)
      .orUndefined("repo", value.repo)
      .required("request_id", value.request_id)
      .required("route", value.route)
      .required("window_created_at_ms", value.window_created_at_ms)
      .required("window_reset_at_ms", value.window_reset_at_ms)
      .required("state", value.state)
      .orNull("terminal_intent", value.terminal_intent)
      .required("reserved_at_ms", value.reserved_at_ms)
      .required("lease_expires_at_ms", value.lease_expires_at_ms)
      .orNull("committed_at_ms", value.committed_at_ms)
      .orNull("released_at_ms", value.released_at_ms)
      .orNull("release_reason", value.release_reason)
      .done();
  }

  KernelQuotaReservationRowV2 tag_invoke(const json::value_to_tag<KernelQuotaReservationRowV2>&, const json::value& value) {
    const Reader in(value, "KernelQuotaReservationRowV2");
    return {
      .v = in.required<double>("v"),
      .scope = in.required<std::string>("scope"),
      .owner = in.required<std::string>("owner"),
      .repo = in.orUndefined<std::string>("repo"),
      .request_id = in.required<std::string>("request_id"),
      .route = in.required<std::string>("route"),
      .window_created_at_ms = in.required<double>("window_created_at_ms"),
      .window_reset_at_ms = in.required<double>("window_reset_at_ms"),
      .state = in.required<std::string>("state"),
      .terminal_intent = in.orNull<std::string>("terminal_intent"),
      .reserved_at_ms = in.required<double>("reserved_at_ms"),
      .lease_expires_at_ms = in.required<double>("lease_expires_at_ms"),
      .committed_at_ms = in.orNull<double>("committed_at_ms"),
      .released_at_ms = in.orNull<double>("released_at_ms"),
      .release_reason = in.orNull<std::string>("release_reason"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const KernelDefaultWindowCutoverV2& value) {
    out = Writer()
      .required("v", value.v)
      .required("id", value.id)
      .required("created_at_ms", value.created_at_ms)
      .required("expires_at_ms", value.expires_at_ms)
      .done();
  }

  KernelDefaultWindowCutoverV2 tag_invoke(const json::value_to_tag<KernelDefaultWindowCutoverV2>&, const json::value& value) {
    const Reader in(value, "KernelDefaultWindowCutoverV2");
    return {
      .v = in.required<double>("v"),
      .id = in.required<std::string>("id"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
    };
  }
}
