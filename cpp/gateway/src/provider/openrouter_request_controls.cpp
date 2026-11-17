#include "uos/provider/openrouter_request_controls.hpp"
#include "uos/http.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <optional>
#include <string>
#include <utility>

namespace uos::provider::openrouter_request_controls {
  namespace {
    rt::Task<std::optional<rt::OrderedSet<std::string>>> requestExtensions(json::object body) {
      if (!rt::hasOwn(body, "cache_control")) co_return std::nullopt;
      const auto raw = utils::getString(rt::get(body, "model"));
      const auto model = raw ? rt::trim(*raw) : std::string();
      static const rt::Regex CLAUDE_MODEL(R"(^~?anthropic/claude-)");
      if (model.empty() || !CLAUDE_MODEL.test(model)) co_return std::nullopt;
      rt::Promise<std::optional<selection::ProviderSelection>> selecting(selection::loadProviderSelectionCached());
      const auto selected = co_await selecting.wait();
      if (!selection::isProviderEnabled("openrouter", selected)) co_return std::nullopt;
      rt::Promise<std::optional<std::string>> resolving(openrouter::resolveOpenRouterUpstreamModel(model));
      const auto resolved = co_await resolving.wait();
      co_return resolved ? std::optional<rt::OrderedSet<std::string>>(OPENROUTER_CLAUDE_REQUEST_EXTENSION_KEYS) : std::nullopt;
    }
  }
  rt::Task<std::optional<rt::OrderedSet<std::string>>> openRouterPromptCacheRequestExtensions(const json::object& body) { return requestExtensions(body); }
  std::optional<rt::Response> validateOpenRouterPromptCacheRequestControl(const json::object& body) {
    const auto* control = rt::get(body, "cache_control");
    const auto invalid = [](const std::string& message, const std::string& param) {
      http::OpenaiErrorOptions options;
      options.param = std::optional<std::string>(param);
      return http::openaiError(400, message, "invalid_request_error", options);
    };
    if (!utils::isRecord(control) || control->is_array()) return invalid("cache_control must be an object", "cache_control");
    std::optional<std::string> unknown;
    for (const auto& key : rt::objectKeys(control->as_object())) {
      if (key != "type" && key != "ttl") { unknown = key; break; }
    }
    if (unknown && !unknown->empty()) return invalid("Unknown cache control field: " + *unknown, "cache_control." + *unknown);
    if (rt::getString(*control, "type") != "ephemeral") return invalid("cache_control.type must be ephemeral", "cache_control.type");
    const auto* ttl = rt::get(*control, "ttl");
    if (ttl && rt::getString(ttl) != "5m" && rt::getString(ttl) != "1h") return invalid("cache_control.ttl must be 5m or 1h", "cache_control.ttl");
    return std::nullopt;
  }
}
