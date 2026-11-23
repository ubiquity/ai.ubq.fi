#include "uos/admin/codex_reset.hpp"
#include "uos/http.hpp"
#include "uos/utils.hpp"
#include "uos/rt/regex.hpp"
namespace uos::admin::codex_reset {
  rt::Task<rt::Response> handleAdminCodexResetConsume(rt::Request request) { return handleAdminCodexResetConsume(std::move(request), {}); }
  rt::Task<rt::Response> handleAdminCodexResetConsume(rt::Request request, codex::manual_reset::ManualResetDependencies dependencies) {
    json::value body = nullptr;
    try { rt::Promise reading(request.json()); body = co_await reading.wait(); } catch (...) {}
    const auto hash = rt::getString(body, "account_id_hash");
    static const rt::Regex identity("^[a-f0-9]{64}$");
    if (!utils::isRecord(body) || !body.is_object() || rt::objectKeys(body.as_object()).size() != 1 || !hash || hash->size() != 64 || !identity.test(*hash)) co_return http::openaiError(400, "Only account_id_hash is accepted.", "invalid_request_error");
    try {
      rt::Promise consuming(codex::manual_reset::consumeManualCodexReset(std::string(*hash), dependencies, request.signal())); const auto result = co_await consuming.wait();
      co_return http::json(200, json::value_from(result), rt::Headers{{"Cache-Control", "no-store"}});
    } catch (const codex::manual_reset::ManualResetError& error) {
      json::object value{{"error", json::object{{"type", "invalid_request_error"}, {"code", error.code}, {"message", error.message}}}};
      if (error.resetStatus) value["reset_status"] = *error.resetStatus;
      co_return http::json(error.status, value, rt::Headers{{"Cache-Control", "no-store"}});
    } catch (...) { co_return http::openaiError(503, "Banked reset is unavailable; no account priority is confirmed.", "codex_reset_unavailable"); }
  }
}
