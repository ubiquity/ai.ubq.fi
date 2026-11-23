#pragma once
// Complete owned-export return values and argument mutations recorded while the original Bun
// assertions passed. Handler/normalizer assertions outside these exports remain separate gates.
#include "uos/cache/scope_targets.hpp"
#include "uos/deepseek/chat_projection.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/deepseek/responses_stream.hpp"
#include "uos/deepseek/flash_aliases.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/config.hpp"
#include "uos/utils.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json_document.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"
#include <zlib.h>
#include <limits>
#include <map>

namespace uos::tests::helpers::deepseek_port_replay {
  namespace cp = deepseek::chat_projection;
  namespace fp = deepseek::forwarded_payload_policy;
  namespace rp = deepseek::responses_payload;
  namespace rs = deepseek::responses_stream;
  namespace rr = deepseek::responses;
  namespace st = cache::scope_targets;
  inline json::value undefinedValue() { return json::object{{"$undefined", true}}; }
  inline bool isUndefined(const json::value* value) { return !value || (value->is_object() && rt::getBool(*value, "$undefined").value_or(false)); }
  inline json::value jsonValue(const json::value& value) {
    if (value.is_object()) {
      if (const auto number = rt::getString(value, "$number")) {
        if (*number == "NaN") return rt::makeNumber(rt::Number::NaN);
        return rt::makeNumber(*number == "Infinity" ? rt::Number::POSITIVE_INFINITY : rt::Number::NEGATIVE_INFINITY);
      }
      json::object out;
      for (const auto& entry : value.as_object()) if (!isUndefined(&entry.value())) rt::objectDefine(out, std::string_view(entry.key().data(), entry.key().size()), jsonValue(entry.value()));
      return out;
    }
    if (value.is_array()) { json::array out; for (const auto& part : value.as_array()) out.push_back(isUndefined(&part) ? json::value(nullptr) : jsonValue(part)); return out; }
    return value;
  }
  inline const json::value* argument(const json::array& args, std::size_t index) { const auto* value = rt::get(args, index); return isUndefined(value) ? nullptr : value; }
  inline std::string stringArgument(const json::array& args, std::size_t index, const std::string& fallback = "") { const auto* value = argument(args, index); return value && value->is_string() ? rt::toString(*value) : fallback; }
  inline json::value valueArgument(const json::array& args, std::size_t index) { const auto* value = argument(args, index); return value ? jsonValue(*value) : json::value(nullptr); }
  inline const rr::ChatOnlyResponsesProfile& profile(const json::array& args, std::size_t index) {
    const auto* value = argument(args, index);
    const auto id = value ? rt::getString(*value, "$profile") : std::nullopt;
    return id == "lithos" ? rr::LITHOS_RESPONSES_PROFILE : id == "cerebras" ? rr::CEREBRAS_RESPONSES_PROFILE : rr::DEEPSEEK_RESPONSES_PROFILE;
  }
  inline rp::DeepSeekResponsesEcho echo(const json::value& value) {
    const auto raw = jsonValue(value);
    const auto* tools = rt::get(raw, "tools"), *choice = rt::get(raw, "tool_choice"), *parallel = rt::get(raw, "parallel_tool_calls");
    return {tools ? *tools : json::value(nullptr), choice ? *choice : json::value(nullptr), parallel ? *parallel : json::value(nullptr), utils::getString(rt::get(raw, "instructions"))};
  }
  inline rt::OrderedMap<std::string, rr::OriginalToolName> toolNames(const json::value* value) {
    rt::OrderedMap<std::string, rr::OriginalToolName> names;
    const auto* pairs = value ? rt::getArray(*value, "$map") : nullptr;
    if (pairs) for (const auto& pair : *pairs) names.set(rt::toString(pair.as_array()[0]), {utils::getString(rt::get(pair.as_array()[1], "name")).value_or(""), utils::getString(rt::get(pair.as_array()[1], "namespace"))});
    return names;
  }
  inline json::value encodeToolNames(const rt::OrderedMap<std::string, rr::OriginalToolName>& names) {
    json::array pairs;
    for (const auto& [name, original] : names) pairs.push_back(json::array{json::value(name), json::object{{"name", original.name}, {"namespace", original.namespace_ ? json::value(*original.namespace_) : json::value(nullptr)}}});
    return json::object{{"$map", std::move(pairs)}};
  }
  inline rt::OrderedSet<std::string> customNames(const json::value* value) {
    rt::OrderedSet<std::string> names;
    if (const auto* values = value ? rt::getArray(*value, "$set") : nullptr) for (const auto& name : *values) names.add(rt::toString(name));
    return names;
  }
  inline json::value encodeCustomNames(const rt::OrderedSet<std::string>& names) {
    json::array values; for (const auto& name : names) values.push_back(json::value(name)); return json::object{{"$set", std::move(values)}};
  }
  inline json::value encodeFailure(const rr::DeepSeekResponsesFailure& value) {
    json::object out{{"ok", false}, {"message", value.message}, {"param", value.param}};
    rt::objectSet(out, "code", value.code); rt::objectSet(out, "kind", value.kind); return out;
  }
  template<class T, class Encode> json::value encodeResult(const rr::DeepSeekResponsesResult<T>& value, Encode encode) {
    if (const auto* error = std::get_if<rr::DeepSeekResponsesFailure>(&value)) return encodeFailure(*error);
    return json::object{{"ok", true}, {"value", encode(std::get<rr::DeepSeekResponsesResultOk<T>>(value).value)}};
  }
  template<class T, class Encode> json::array encodeRows(const std::vector<T>& rows, Encode encode) { json::array out; for (const auto& row : rows) out.push_back(encode(row)); return out; }
  inline json::array encodeElisions(const std::vector<fp::ForwardedPayloadElision>& rows) { return encodeRows(rows, [](const auto& row) { return fp::toJson(row); }); }
  inline json::array encodeExemptions(const std::vector<fp::ForwardedPayloadExemption>& rows) { return encodeRows(rows, [](const auto& row) { return fp::toJson(row); }); }
  inline json::value encodeBody(const cp::DeepSeekResponsesChatBody& value) {
    json::array warnings; for (const auto& warning : value.warnings) warnings.push_back(json::value(warning));
    return json::object{{"body", value.body}, {"toolNames", encodeToolNames(value.toolNames)}, {"customToolNames", encodeCustomNames(value.customToolNames)},
      {"elisions", encodeElisions(value.elisions)}, {"exemptions", encodeExemptions(value.exemptions)}, {"promptAdmission", fp::toJson(value.promptAdmission)}, {"warnings", warnings}};
  }
  inline std::vector<fp::ForwardedPayloadElision> elisionsFrom(const json::value* value) {
    std::vector<fp::ForwardedPayloadElision> out;
    if (value && value->is_array()) for (const auto& row : value->as_array()) out.push_back({rt::toString(rt::get(row, "path")), utils::getString(rt::get(row, "callId")), rt::toString(rt::get(row, "kind")),
      rt::getNumber(row, "originalBytes").value_or(0), rt::getNumber(row, "forwardedBytes").value_or(0), rt::getNumber(row, "omittedBytes").value_or(0)});
    return out;
  }
  inline std::string firstDifference(const json::value* actual, const json::value* expected, const std::string& path = "$") {
    if (!actual || !expected) return path + ": missing member";
    if (rt::deepEqual(*actual, *expected)) return "";
    if (actual->is_object() && expected->is_object()) {
      for (const auto& entry : expected->as_object()) { const std::string key(entry.key()); const auto difference = firstDifference(rt::get(*actual, key), &entry.value(), path + "." + key); if (!difference.empty()) return difference; }
      return path + ": unexpected member";
    }
    if (actual->is_array() && expected->is_array()) {
      if (actual->as_array().size() != expected->as_array().size()) return path + ": array length " + rt::numberToString(static_cast<double>(actual->as_array().size())) + " vs " + rt::numberToString(static_cast<double>(expected->as_array().size()));
      for (std::size_t i = 0; i < actual->as_array().size(); ++i) { auto difference = firstDifference(&actual->as_array()[i], &expected->as_array()[i], path + "[" + rt::numberToString(static_cast<double>(i)) + "]"); if (!difference.empty()) return difference; }
    }
    return path + ": " + rt::slice(rt::jsonStringify(*actual), 0, 180) + " != " + rt::slice(rt::jsonStringify(*expected), 0, 180);
  }
  inline void equal(const json::value& actual, const json::value& expected, const std::string& label) {
    if (!rt::deepEqual(actual, expected)) throw rt::JsError(label + " " + firstDifference(&actual, &expected));
  }
  inline rt::Task<void> replayCase(json::value row) {
    std::map<double, rs::DeepSeekResponsesStreamTranslator> streams;
    for (const auto& raw : row.as_object().at("calls").as_array()) {
      const auto& call = raw.as_object();
      const auto fn = rt::toString(rt::get(call, "fn"));
      const auto& args = call.at("args").as_array();
      json::array after = args;
      json::value actual;
      std::optional<json::object> error;
      try {
        if (fn == "toDeepSeekChatMessages") {
          auto input = valueArgument(args, 0);
          const auto instructions = argument(args, 1) && argument(args, 1)->is_string() ? std::optional(rt::toString(*argument(args, 1))) : std::nullopt;
          auto elisions = elisionsFrom(argument(args, 3));
          std::vector<fp::ForwardedPayloadExemption> exemptions;
          const auto result = cp::toDeepSeekChatMessages(argument(args, 0) ? &input : nullptr, instructions, stringArgument(args, 2, "reduce"), elisions, exemptions);
          actual = encodeResult(result, [](const auto& value) { return json::value(value); });
          if (args.size() > 3) after[3] = encodeElisions(elisions);
          if (args.size() > 4) after[4] = encodeExemptions(exemptions);
        } else if (fn == "toDeepSeekResponsesChatBody") {
          const auto body = valueArgument(args, 0).as_object();
          fp::PromptAdmissionProjection projection;
          if (argument(args, 4)) projection = [](const json::object& value) { return provider::cerebras::projectCerebrasRequest(value); };
          const auto result = cp::toDeepSeekResponsesChatBody(body, stringArgument(args, 1), argument(args, 2) && rt::truthy(*argument(args, 2)), profile(args, 3), projection);
          actual = encodeResult(result, [](const auto& value) { return encodeBody(value); });
        } else if (fn == "toDeepSeekResponsesPayload") {
          actual = rp::toDeepSeekResponsesPayload(valueArgument(args, 0).as_object(), stringArgument(args, 1), stringArgument(args, 2), echo(valueArgument(args, 3)), toolNames(argument(args, 4)), customNames(argument(args, 5)), profile(args, 6));
        } else if (fn == "toResponsesUsage") {
          auto value = valueArgument(args, 0);
          auto usage = rp::toResponsesUsage(argument(args, 0) ? &value : nullptr, profile(args, 1)); actual = usage ? json::value(*usage) : json::value(nullptr);
        } else if (fn == "deepSeekResponsesTerminalKind") {
          auto value = valueArgument(args, 0); actual = rs::deepSeekResponsesTerminalKind(argument(args, 0) ? &value : nullptr, profile(args, 1));
        } else if (fn == "encodeResponsesEvent") actual = rs::encodeResponsesEvent(valueArgument(args, 0).as_object());
        else if (fn == "createDeepSeekResponsesStreamTranslator") {
          const auto id = rt::getNumber(call, "stream").value();
          streams.emplace(id, rs::createDeepSeekResponsesStreamTranslator(stringArgument(args, 0), stringArgument(args, 1), echo(valueArgument(args, 2)), rt::toNumber(valueArgument(args, 3)), toolNames(argument(args, 4)), customNames(argument(args, 5)), profile(args, 6)));
          continue;
        } else if (rt::startsWith(fn, "stream.")) {
          auto& stream = streams.at(rt::getNumber(call, "stream").value());
          if (fn == "stream.open") actual = encodeRows(stream.open(), [](const auto& value) { return value; });
          else if (fn == "stream.push") actual = encodeRows(stream.push(valueArgument(args, 0).as_object()), [](const auto& value) { return value; });
          else if (fn == "stream.finish") actual = encodeRows(stream.finish(), [](const auto& value) { return value; });
          else if (fn == "stream.terminalKind") actual = stream.terminalKind();
          else if (fn == "stream.upstreamFinishReason") { const auto reason = stream.upstreamFinishReason(); actual = reason ? json::value(*reason) : json::value(nullptr); }
          else { const auto value = stream.answerBearingOutput(); actual = json::object{{"text", value.text}, {"refusal", value.refusal}, {"toolCallCount", value.toolCallCount}}; }
        } else if (fn == "derivePromptCacheScopeTargetInventory") {
          auto input = valueArgument(args, 0);
          st::DerivePromptCacheScopeTargetInventoryInput options{std::nullopt, utils::getString(rt::get(input, "catalogVersionstamp")), nullptr, utils::getString(rt::get(input, "codexAuthPoolVersionstamp")), std::nullopt};
          if (const auto* snapshot = rt::get(input, "snapshot"); snapshot && snapshot->is_object()) options.snapshot = snapshot->as_object();
          if (const auto* pool = rt::get(input, "codexAuthPool")) options.codexAuthPool = *pool;
          if (const auto* roster = rt::get(input, "meteredFallbackRoster"); roster && roster->is_object()) options.meteredFallbackRoster = json::value_to<st::MeteredFallbackRoster>(*roster);
          auto task = st::derivePromptCacheScopeTargetInventory(options);
          auto value = co_await std::move(task);
          actual = json::value_from(value);
        } else if (fn == "resolveDeepSeekFlashAliases") {
          deepseek::flash_aliases::DeepSeekFlashCatalogInput input;
          const auto value = valueArgument(args, 0);
          const auto assign = [&](std::string_view key, std::optional<std::vector<deepseek::flash_aliases::CatalogModel>>& target) {
            if (const auto* values = rt::getArray(value, key)) { std::vector<deepseek::flash_aliases::CatalogModel> models; for (const auto& model : *values) models.push_back({rt::toString(rt::get(model, "id"))}); target = std::move(models); }
          };
          assign("lithos", input.lithos); assign("openrouter", input.openrouter); assign("surplus", input.surplus); assign("openlux", input.openlux);
          const auto aliases = deepseek::flash_aliases::resolveDeepSeekFlashAliases(input).models;
          actual = json::object{{"models", json::object{{"lithos", aliases.lithos}, {"deepseek", aliases.deepseek}, {"openrouter", aliases.openrouter}, {"surplus", aliases.surplus}, {"openlux", aliases.openlux}}}};
        } else throw rt::JsError("unhandled recorded export " + fn);
      } catch (const rt::JsError& failure) { error = json::object{{"name", failure.name}, {"message", failure.message}}; }
      const auto* expectedError = rt::get(call, "error");
      if (expectedError) equal(error ? json::value(*error) : json::value(nullptr), *expectedError, fn + " error");
      else {
        if (error) throw rt::JsError(fn + " unexpected " + rt::jsonStringify(*error));
        equal(actual, jsonValue(call.at("result")), fn);
        if (const auto* expectedAfter = rt::get(call, "argsAfter")) equal(jsonValue(after), jsonValue(*expectedAfter), fn + " argument mutation");
      }
    }
  }

  // The checked-in compaction trace bundle has its own explicit, larger budget.
  // Decode this trusted fixture as one gzip member into a fixed-size buffer.
  inline rt::Bytes inflateLargeFixture(rt::Bytes compressed, std::size_t maxFixtureBytes) {
    if (compressed.size() > rt::limits::kStoredGzipBytes || maxFixtureBytes >= std::numeric_limits<uInt>::max())
      throw rt::JsError("Invalid compressed fixture size");
    rt::Bytes bytes(maxFixtureBytes + 1);
    z_stream stream{};
    stream.next_in = compressed.data();
    stream.avail_in = static_cast<uInt>(compressed.size());
    stream.next_out = bytes.data();
    stream.avail_out = static_cast<uInt>(bytes.size());
    if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) throw rt::JsError("Cannot initialize gzip fixture decoder");
    const auto finish = rt::scopeExit([&] { inflateEnd(&stream); });
    if (inflate(&stream, Z_FINISH) != Z_STREAM_END || stream.avail_in != 0 || stream.total_out > maxFixtureBytes)
      throw rt::JsError("Invalid gzip fixture or fixture exceeds its byte limit");
    bytes.resize(stream.total_out);
    return bytes;
  }

  inline bool registerReplay(std::string_view compressed, std::size_t maxFixtureBytes = rt::limits::kJsonDocumentBytes) {
    auto encoded = rt::fromBase64(compressed);
    const bool large = maxFixtureBytes > rt::limits::kStoredGzipBytes;
    const auto bytes = large ? inflateLargeFixture(std::move(encoded), maxFixtureBytes) : rt::gzip::gunzipSync(encoded);
    auto vectors = large ? rt::jsonParseWithLimit({reinterpret_cast<const char*>(bytes.data()), bytes.size()}, maxFixtureBytes)
                         : rt::jsonParseWithLimit(rt::decodeUtf8(bytes), maxFixtureBytes);
    for (auto& row : vectors.as_array()) {
      auto name = rt::toString(rt::get(row, "name"));
      rt::test::registry().push_back({name + " [owned API parity]", [row = std::move(row)] {
        rt::env::ScopedEnvironment environment({{"HOME", "/nonexistent/w05c6-test-home"}});
        const auto savedConfig = config::config.codexAuthJsonB64;
        config::config.codexAuthJsonB64.clear();
        const auto savedNow = rt::globalDateNow();
        rt::globalDateNow() = [] { return 1780000000000.0; };
        const auto restore = rt::scopeExit([savedConfig, savedNow] { config::config.codexAuthJsonB64 = savedConfig; rt::globalDateNow() = savedNow; });
        rt::FakeClock clock; rt::Loop loop(clock); auto task = replayCase(row); rt::blockOn(loop, std::move(task));
      }});
    }
    return true;
  }
}
