#include "uos/provider/gemini.hpp"

#include "uos/admin/gemini_quota.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/deepseek/responses_payload.hpp"
#include "uos/deepseek/responses_stream.hpp"
#include "uos/http.hpp"
#include "uos/input_normalization.hpp"
#include "uos/openai.hpp"
#include "uos/provider/health.hpp"
#include "uos/request_policy.hpp"
#include "uos/responses_request.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::provider::gemini {
  namespace {
    namespace tel = openai_telemetry;
    namespace payload = deepseek::responses_payload;
    namespace rs = responses_stream;

    constexpr std::size_t MAX_PROMPT_BYTES = 8 * 1024 * 1024;
    constexpr std::size_t MAX_OUTPUT_BYTES = 32 * 1024 * 1024;
    constexpr std::size_t MAX_STDERR_BYTES = 128 * 1024;
    constexpr std::size_t MAX_LINE_BYTES = 4 * 1024 * 1024;

    struct GeminiError : rt::JsError {
      int status_code;
      std::string code;
      GeminiError(int status, std::string code_, std::string message_)
        : rt::JsError(std::move(message_)), status_code(status), code(std::move(code_)) {}
    };

    const GeminiError* geminiErrorOf(const rt::Reason& reason) noexcept {
      if (!reason) return nullptr;
      try {
        std::rethrow_exception(reason);
      } catch (const GeminiError& error) {
        return &error;
      } catch (...) {
      }
      return nullptr;
    }

    std::optional<double> nonnegativeNumber(const json::value* value) {
      if (!value || !rt::isNumber(*value)) return std::nullopt;
      const double number = rt::toNumber(*value);
      return rt::Number::isFinite(number) && number >= 0 ? std::optional(number) : std::nullopt;
    }

    json::value jsonOrNull(const json::value* value) { return value ? *value : json::value(nullptr); }

    payload::DeepSeekResponsesEcho responsesEcho(const json::object& rawRecord) {
      const auto field = [&rawRecord](const char* key) { return jsonOrNull(rt::get(rawRecord, key)); };
      const auto instructions = rt::getString(rawRecord, "instructions");
      return payload::DeepSeekResponsesEcho{field("tools"), field("tool_choice"), field("parallel_tool_calls"),
        instructions && !rt::trim(*instructions).empty() ? std::optional<std::string>(*instructions) : std::nullopt};
    }

    bool hasDeclaredTools(const json::object& rawRecord) {
      const auto* tools = rt::get(rawRecord, "tools");
      return tools && !tools->is_null() && !(tools->is_array() && tools->as_array().empty());
    }

    std::optional<std::string> textFromContent(const ::uos::types::MessageContentItem& item) {
      if (const auto* text = std::get_if<::uos::types::MessageContentItemInputText>(&item)) return text->text;
      if (const auto* text = std::get_if<::uos::types::MessageContentItemOutputText>(&item)) return text->text;
      return std::nullopt;
    }

    struct Prompt {
      std::string text;
      std::optional<std::string> schema{};
    };

    void appendMessage(std::string& prompt, std::string_view role, const std::vector<::uos::types::MessageContentItem>& content) {
      std::vector<std::string> text;
      for (const auto& item : content) {
        const auto part = textFromContent(item);
        if (!part) throw GeminiError(400, "gemini_unsupported_content", "Gemini subscription accepts text content only.");
        if (!rt::trim(*part).empty()) text.push_back(*part);
      }
      if (text.empty()) return;
      prompt += "\n\n" + std::string(role) + ":\n" + rt::join(text, "\n");
    }

    std::optional<std::string> jsonSchemaForResponses(const json::object& rawRecord) {
      const auto* text = rt::getObject(rawRecord, "text");
      const auto* format = text ? rt::getObject(*text, "format") : nullptr;
      if (!format) return std::nullopt;
      const auto type = rt::getString(*format, "type");
      if (type == std::optional<std::string_view>("json_object")) return std::string(R"({"type":"object"})");
      if (type != std::optional<std::string_view>("json_schema")) return std::nullopt;
      const auto* schema = rt::get(*format, "schema");
      return schema ? std::optional<std::string>(rt::jsonStringify(*schema)) : std::nullopt;
    }

    std::optional<std::string> jsonSchemaForChat(const json::object& rawRecord) {
      const auto* format = rt::getObject(rawRecord, "response_format");
      if (!format) return std::nullopt;
      const auto type = rt::getString(*format, "type");
      if (type == std::optional<std::string_view>("json_object")) return std::string(R"({"type":"object"})");
      if (type != std::optional<std::string_view>("json_schema")) return std::nullopt;
      const auto* definition = rt::getObject(*format, "json_schema");
      const auto* schema = definition ? rt::get(*definition, "schema") : nullptr;
      return schema ? std::optional<std::string>(rt::jsonStringify(*schema)) : std::nullopt;
    }

    Prompt promptForResponses(const json::object& rawRecord, const std::vector<::uos::types::ResponseInputItem>& input) {
      Prompt result;
      const auto instructions = rt::getString(rawRecord, "instructions");
      if (instructions && !rt::trim(*instructions).empty()) result.text = "System instructions:\n" + std::string(*instructions);
      for (const auto& item : input) {
        if (const auto* message = std::get_if<::uos::types::ResponseMessageItem>(&item)) {
          const std::string role = message->role == "assistant" ? "Assistant" : message->role == "developer" || message->role == "system" ? "Developer" : "User";
          appendMessage(result.text, role, message->content);
          continue;
        }
        throw GeminiError(400, "gemini_unsupported_content", "Gemini subscription accepts message text only; tool and generated-item replay is not supported.");
      }
      if (rt::trim(result.text).empty()) throw GeminiError(400, "gemini_empty_prompt", "Gemini subscription requires non-empty input.");
      result.text += "\n\nAnswer the latest user request directly. Do not invoke tools, browse, execute commands, or modify files.";
      result.schema = jsonSchemaForResponses(rawRecord);
      return result;
    }

    Prompt promptForChat(const json::object& rawRecord) {
      const auto* messages = rt::getArray(rawRecord, "messages");
      if (!messages) throw GeminiError(400, "gemini_invalid_messages", "messages must be an array.");
      Prompt result;
      for (std::size_t index = 0; index < messages->size(); ++index) {
        const auto normalized = input_normalization::normalizeChatMessage((*messages)[index], static_cast<double>(index));
        if (const auto* failure = std::get_if<input_normalization::NormalizationResultNotOk>(&normalized)) {
          throw GeminiError(400, "gemini_invalid_messages", failure->message);
        }
        const auto& value = std::get<input_normalization::NormalizationResultOk<input_normalization::NormalizedChatMessage>>(normalized).value;
        if (value.instruction && !rt::trim(*value.instruction).empty()) result.text += (result.text.empty() ? "System instructions:\n" : "\n\n") + *value.instruction;
        for (const auto& item : value.input) {
          if (const auto* message = std::get_if<::uos::types::ResponseMessageItem>(&item)) {
            const std::string role = message->role == "assistant" ? "Assistant" : message->role == "developer" || message->role == "system" ? "Developer" : "User";
            appendMessage(result.text, role, message->content);
            continue;
          }
          throw GeminiError(400, "gemini_unsupported_content", "Gemini subscription accepts message text only; tool calls and tool outputs are not supported.");
        }
      }
      if (rt::trim(result.text).empty()) throw GeminiError(400, "gemini_empty_prompt", "Gemini subscription requires non-empty messages.");
      result.text += "\n\nAnswer the latest user request directly. Do not invoke tools, browse, execute commands, or modify files.";
      result.schema = jsonSchemaForChat(rawRecord);
      return result;
    }

    std::string agyPath() {
      const auto home = rt::env::get("HOME");
      if (!home || home->empty()) throw GeminiError(503, "gemini_cli_unavailable", "The Antigravity CLI home is unavailable.");
      const std::string path = rt::path::join(*home, ".local/bin/agy");
      const auto info = rt::fs::lstatSync(path);
      if (!info.isFile() || info.isSymbolicLink() || info.uid != rt::process::getuid() || info.nlink != 1 || (info.mode & 077) != 0 ||
          (info.mode & 0100) == 0) {
        throw GeminiError(503, "gemini_cli_unavailable", "The owner-private Antigravity CLI executable is unavailable.");
      }
      return path;
    }

    std::string workRoot() {
      const std::string data = rt::path::resolve(rt::process::cwd(), ".data");
      if (!rt::fs::existsSync(data)) rt::fs::mkdirSync(data, {.mode = 0700});
      const auto info = rt::fs::lstatSync(data);
      if (!info.isDirectory() || info.isSymbolicLink() || info.uid != rt::process::getuid() || (info.mode & 077) != 0) {
        throw GeminiError(503, "gemini_cli_workspace_unavailable", "The Gemini workspace parent is not owner-private.");
      }
      return data;
    }

    rt::Task<std::string> readBounded(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, std::size_t maximum, std::string_view label) {
      std::string output;
      for (;;) {
        const auto result = co_await reader.read();
        if (result.done) break;
        if (!result.value) continue;
        if (output.size() + result.value->size() > maximum) {
          throw GeminiError(502, "gemini_output_too_large", std::string(label) + " exceeded its gateway limit.");
        }
        output.append(reinterpret_cast<const char*>(result.value->data()), result.value->size());
      }
      co_return output;
    }

    rt::Task<std::string> readBounded(rt::ByteStream stream, std::size_t maximum, std::string_view label) {
      auto reader = stream.getReader();
      co_return co_await readBounded(std::move(reader), maximum, label);
    }

    std::vector<std::string> deniedActions(const json::value* value) {
      std::vector<std::string> actions;
      if (!value || !value->is_array()) return actions;
      for (const auto& entry : value->as_array()) {
        std::string action;
        if (const auto type = utils::getString(rt::get(entry, "action"))) action = *type;
        if (const auto name = utils::getString(rt::get(entry, "display_name"))) action += (action.empty() ? "" : ":") + *name;
        if (!action.empty()) actions.push_back(std::move(action));
      }
      return actions;
    }

    std::optional<GeminiCliRunner>& testRunner() {
      static std::optional<GeminiCliRunner> runner;
      return runner;
    }

    struct ParsedCli {
      GeminiCliTurn turn;
      std::string status;
    };

    ParsedCli parseCliOutput(std::string_view output, const std::string& stderrText, int exitCode) {
      ParsedCli parsed;
      std::optional<json::object> result;
      std::optional<json::object> lastObject;
      std::size_t start = 0;
      while (start < output.size()) {
        const std::size_t end = output.find('\n', start);
        const std::string_view line = output.substr(start, end == std::string_view::npos ? output.size() - start : end - start);
        start = end == std::string_view::npos ? output.size() : end + 1;
        if (line.size() > MAX_LINE_BYTES) throw GeminiError(502, "gemini_output_too_large", "Antigravity emitted an oversized stream-json event.");
        if (rt::trim(line).empty()) continue;
        json::value event;
        try {
          event = rt::jsonParse(line);
        } catch (...) {
          throw GeminiError(502, "gemini_cli_invalid_json", "Antigravity emitted malformed stream-json output.");
        }
        if (!event.is_object()) continue;
        lastObject = event.as_object();
        if (rt::getString(event, "event") == std::optional<std::string_view>("result")) {
          if (const auto* value = rt::getObject(event, "result")) result = *value;
        } else if (rt::getString(event, "status")) {
          result = event.as_object();
        }
      }
      if (!result) {
        if (exitCode != 0) {
          const std::string detail = rt::trim(stderrText);
          throw GeminiError(502, "gemini_cli_failed", detail.empty() ? "Antigravity CLI exited without a result." : "Antigravity CLI failed: " + rt::slice(detail, 0, 500));
        }
        throw GeminiError(502, "gemini_cli_missing_result", "Antigravity CLI produced no result event.");
      }
      const auto status = rt::getString(*result, "status");
      if (status) parsed.status = *status;
      const auto response = rt::getString(*result, "response");
      if (response) parsed.turn.response = *response;
      if (const auto conversation = rt::getString(*result, "conversation_id"); conversation && !conversation->empty()) parsed.turn.conversationId = *conversation;
      parsed.turn.rawResult = *result;
      if (const auto* usage = rt::getObject(*result, "usage")) parsed.turn.usage = *usage;
      parsed.turn.deniedActions = deniedActions(rt::get(*result, "denied_actions"));
      if (parsed.status != "SUCCESS") {
        throw GeminiError(502, "gemini_cli_failed", "Antigravity CLI returned status " + (parsed.status.empty() ? std::string("unknown") : parsed.status) + ".");
      }
      if (rt::trim(parsed.turn.response).empty() && !parsed.turn.deniedActions.empty()) {
        throw GeminiError(502, "gemini_tool_denied", "Antigravity requested a tool that headless gateway inference cannot approve.");
      }
      if (rt::trim(parsed.turn.response).empty()) throw GeminiError(502, "gemini_empty_response", "Antigravity returned an empty response.");
      return parsed;
    }
  }

  std::optional<std::string> geminiUpstreamModelFor(std::string_view model) {
    const std::string trimmed = rt::trim(model);
    if (std::find(GEMINI_MODEL_IDS.begin(), GEMINI_MODEL_IDS.end(), trimmed) == GEMINI_MODEL_IDS.end()) return std::nullopt;
    return trimmed;
  }

  bool isGeminiModel(std::string_view model) { return geminiUpstreamModelFor(model).has_value(); }

  std::optional<tel::UsageTokens> geminiUsageTokens(const json::object& usage) {
    const auto input = nonnegativeNumber(rt::get(usage, "input_tokens"));
    const auto output = nonnegativeNumber(rt::get(usage, "output_tokens"));
    const auto total = nonnegativeNumber(rt::get(usage, "total_tokens"));
    const auto cached = nonnegativeNumber(rt::get(usage, "cache_read_tokens"));
    if (!input || !output || !total) return std::nullopt;
    tel::UsageTokens result{
      .inputTokens = input,
      .cachedInputTokens = cached,
      .cacheWriteInputTokens = std::nullopt,
      .outputTokens = output,
      .totalTokens = total,
      .status = "reported",
    };
    return result;
  }

  void setGeminiCliRunnerForTest(GeminiCliRunner runner) { testRunner() = std::move(runner); }

  rt::Task<GeminiCliTurn> runGeminiCli(const GeminiCliOptions& options) {
    if (testRunner()) return runGeminiCli(options, *testRunner());
    return runGeminiCli(options, {});
  }

  rt::Task<GeminiCliTurn> runGeminiCli(const GeminiCliOptions& options, const GeminiCliRunner& runner) {
    if (runner) {
      auto task = runner(options);
      co_return co_await std::move(task);
    }
    if (options.prompt.size() > MAX_PROMPT_BYTES) throw GeminiError(413, "gemini_prompt_too_large", "Gemini subscription prompt exceeds 8 MiB.");
    if (options.signal && options.signal->aborted()) rt::rethrow(options.signal->reason());
    const std::string executable = agyPath();
    const std::string directory = rt::fs::mkdtempSync(rt::path::join(workRoot(), "antigravity-run-"));
    auto cleanup = rt::scopeExit([directory] {
      try { rt::fs::rmSync(directory, {.recursive = true, .force = true}); } catch (...) {}
    });
    std::vector<std::string> command{executable};
    if (options.streamJson) command.insert(command.end(), {"--input-format", "stream-json", "--output-format", "stream-json"});
    else command.insert(command.end(), {"-p", options.prompt, "--output-format", "json"});
    command.insert(command.end(), {"--model", options.model, "--print-timeout", "120s", "--log-file", rt::path::join(directory, "agy.log")});
    if (!options.mode.empty()) command.insert(command.end(), {"--mode", options.mode});
    if (options.jsonSchema) {
      const std::string schemaPath = rt::path::join(directory, "schema.json");
      rt::fs::writeFileSync(schemaPath, *options.jsonSchema, {.flag = "wx", .mode = 0600});
      command.insert(command.end(), {"--json-schema", schemaPath});
    }
    if (options.disableSlashCommands) command.insert(command.end(), "--disable-slash-commands");
    const rt::process::Subprocess child = rt::process::spawn(command,
      {.cwd = directory, .stdin = options.streamJson ? rt::process::Stdio::pipe : rt::process::Stdio::ignore,
        .stdout = rt::process::Stdio::pipe, .stderr = rt::process::Stdio::pipe});
    if (options.onDispatch) options.onDispatch();
    if (options.onInit) options.onInit();
    rt::EventListener abort = [child] { child.kill(); };
    if (options.signal) {
      if (options.signal->aborted()) child.kill();
      else options.signal->addEventListener("abort", abort, {.once = true});
    }
    auto removeAbort = rt::scopeExit([options, abort] { if (options.signal) options.signal->removeEventListener("abort", abort); });
    if (options.streamJson) {
      const json::object input{{"event", "user"}, {"message", json::object{{"content", options.prompt}}}};
      auto sink = child.stdin();
      sink.write(rt::jsonStringify(input) + "\n");
      auto ending = sink.end();
      co_await ending.wait();
    }
    rt::Promise<std::string> stdoutBody(readBounded(child.stdout(), MAX_OUTPUT_BYTES, "Antigravity stdout"));
    rt::Promise<std::string> stderrBody(readBounded(child.stderr(), MAX_STDERR_BYTES, "Antigravity stderr"));
    const std::string output = co_await stdoutBody.wait();
    const std::string errors = co_await stderrBody.wait();
    const int exitCode = co_await child.exited().wait();
    if (options.signal && options.signal->aborted()) rt::rethrow(options.signal->reason());
    co_return parseCliOutput(output, errors, exitCode).turn;
  }

  namespace {
    struct Prepared {
      Prompt prompt;
      bool stream = false;
      bool includeUsage = false;
      std::string reasoning{};
    };

    Prepared prepareCommon(const json::object& rawRecord, Prompt prompt) {
      const auto parsedStream = request_policy::parseStreamField(rt::get(rawRecord, "stream"));
      if (const auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&parsedStream)) {
        throw GeminiError(400, "gemini_invalid_stream", failure->message);
      }
      Prepared prepared{.prompt = std::move(prompt), .stream = std::get<request_policy::ParseStreamFieldOk>(parsedStream).value};
      const auto streamOptions = request_policy::parseChatStreamOptions(rt::get(rawRecord, "stream_options"));
      if (const auto* failure = std::get_if<request_policy::ParseFieldNotOk>(&streamOptions)) {
        throw GeminiError(400, "gemini_invalid_stream_options", failure->message);
      }
      prepared.includeUsage = std::get<request_policy::ParseChatStreamOptionsOk>(streamOptions).includeUsage;
      return prepared;
    }

    std::string reasoningForModel(const std::string& model) {
      if (rt::endsWith(model, "-high")) return "high";
      if (rt::endsWith(model, "-medium")) return "medium";
      return "low";
    }

    std::string normalizedId(std::string_view raw, std::string_view prefix) {
      std::string id(prefix);
      for (const char value : raw) {
        if ((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9')) id.push_back(value);
        if (id.size() >= 48) break;
      }
      if (id.size() == prefix.size()) id += rt::crypto::randomUUID();
      return id;
    }

    json::object chatUsageObject(const tel::UsageTokens& usage) {
      json::object details;
      if (usage.cachedInputTokens) details["cached_tokens"] = rt::makeNumber(*usage.cachedInputTokens);
      json::object result{{"prompt_tokens", rt::makeNumber(*usage.inputTokens)}, {"completion_tokens", rt::makeNumber(*usage.outputTokens)},
        {"total_tokens", rt::makeNumber(*usage.totalTokens)}};
      if (!details.empty()) result["prompt_tokens_details"] = std::move(details);
      return result;
    }

    json::object syntheticCompletion(const std::string& id, const std::string& model, double created, const std::string& text,
      const std::optional<tel::UsageTokens>& usage) {
      json::object result{{"id", id}, {"object", "chat.completion"}, {"created", rt::makeNumber(created)}, {"model", model},
        {"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"message", json::object{{"role", "assistant"}, {"content", text}}},
          {"finish_reason", "stop"}}}}};
      if (usage) result["usage"] = chatUsageObject(*usage);
      return result;
    }

    rt::Response failureResponse(const rt::Reason& reason, const tel::UsageContext* context) {
      if (const auto* error = geminiErrorOf(reason)) {
        return http::openaiError(error->status_code, error->message, error->code);
      }
      upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Antigravity CLI failed:", reason);
      tel::recordStreamTerminalType(context, "error");
      return http::openaiError(502, "Antigravity subscription failed to produce a response.", "gemini_upstream_error");
    }

    rt::Task<GeminiCliTurn> runWithAdmission(const Prepared& prepared, const std::string& model, const tel::UsageContext* context,
      const GeminiHandlerOptions& options, bool& admitted) {
      if (context && context->beforeProviderDispatch) {
        auto dispatching = context->beforeProviderDispatch("gemini");
        co_await std::move(dispatching);
      }
      tel::recordAttemptedProvider(context, "gemini");
      GeminiCliOptions optionsForCli{.model = model, .prompt = prepared.prompt.text, .jsonSchema = prepared.prompt.schema,
        .onDispatch = [context] { tel::recordFirstProviderDispatch(context); },
        .onInit = [context] { tel::recordFirstProviderHeaders(context); }};
      admitted = true;
      auto task = runGeminiCli(optionsForCli, options.runner);
      auto turn = co_await std::move(task);
      tel::recordFirstProviderHeaders(context);
      co_return turn;
    }
  }

  rt::Task<rt::Response> handleGeminiChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const tel::UsageContext* usageContext, const GeminiHandlerOptions& handlerOptions) {
    static_cast<void>(req);
    std::optional<GeminiCliTurn> turn;
    rt::Reason failure;
    bool admitted = false;
    try {
      const auto model = geminiUpstreamModelFor(modelRaw);
      if (!model) throw GeminiError(400, "gemini_model_unknown", "Gemini subscription does not support model " + modelRaw + ".");
      if (hasDeclaredTools(rawRecord)) throw GeminiError(400, "gemini_tools_unsupported", "Gemini subscription Chat Completions is text-only and does not translate caller tools.");
      auto prepared = prepareCommon(rawRecord, promptForChat(rawRecord));
      prepared.reasoning = reasoningForModel(*model);
      if (usageContext && usageContext->responseTelemetry) {
        usageContext->responseTelemetry->provider = "gemini";
        usageContext->responseTelemetry->reasoning = prepared.reasoning;
        usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(rt::get(rawRecord, "max_completion_tokens"));
      }
      const tel::RecordRequestUsageDetails details{.model = modelRaw, .route = "chat.completions", .stream = prepared.stream, .reasoning = prepared.reasoning};
      co_await tel::recordRequestUsage(usageContext, details).wait();
      auto task = runWithAdmission(prepared, *model, usageContext, handlerOptions, admitted);
      turn = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (admitted) health::recordGeminiProviderHealth("upstream_error", 502);
      co_return failureResponse(failure, usageContext);
    }
    const auto usage = geminiUsageTokens(turn->usage);
    const double created = rt::Math::floor(rt::dateNow() / 1000);
    const std::string id = normalizedId(turn->conversationId.value_or(""), "chatcmpl-");
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = turn->conversationId;
    if (handlerOptions.refreshQuota) {
      rt::spawn(admin::gemini_quota::refreshSnapshot(rt::path::resolve(rt::process::cwd(), ".data"),
                  admin::gemini_quota::GeminiQuotaOptions{.runner = handlerOptions.runner, .force_refresh = true}, rt::dateNow()),
        rt::ignoreRejection);
    }
    chat::stream_translation::markChatSemanticOutput(usageContext);
    tel::recordTerminalUsage(usageContext, usage, true);
    tel::recordStreamTerminalType(usageContext, "response.completed");
    tel::recordStreamTerminal(usageContext);
    health::recordGeminiProviderHealth("success", 200, {}, turn->conversationId);
    if (!turn->conversationId) { /* A successful result without an ID remains successful; no correlation is invented. */ }
    rt::Headers headers{{"x-uos-upstream", "gemini"}};
    if (turn->conversationId) headers.set("x-uos-provider-request-id", *turn->conversationId);
    if (!std::get<request_policy::ParseStreamFieldOk>(request_policy::parseStreamField(rt::get(rawRecord, "stream"))).value) {
      json::object body{{"id", id}, {"object", "chat.completion"}, {"created", rt::makeNumber(created)}, {"model", modelRaw},
        {"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"message", json::object{{"role", "assistant"}, {"content", turn->response}}},
          {"finish_reason", "stop"}}}}};
      if (usage) body["usage"] = chatUsageObject(*usage);
      if (!turn->deniedActions.empty()) body["uos_warnings"] = json::value_from(turn->deniedActions);
      co_return http::json(200, body, headers);
    }
    const auto streamOptions = request_policy::parseChatStreamOptions(rt::get(rawRecord, "stream_options"));
    const bool includeUsage = std::get<request_policy::ParseChatStreamOptionsOk>(streamOptions).includeUsage;
    std::string stream = "data: " + rt::jsonStringify(json::object{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)},
      {"model", modelRaw}, {"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"delta", json::object{{"role", "assistant"}, {"content", turn->response}}},
        {"finish_reason", "stop"}}}}}) + "\n\n";
    if (includeUsage && usage) stream += "data: " + rt::jsonStringify(json::object{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)},
      {"model", modelRaw}, {"choices", json::array{}}, {"usage", chatUsageObject(*usage)}}) + "\n\n";
    stream += "data: [DONE]\n\n";
    headers.set("Content-Type", "text/event-stream");
    headers.set("Cache-Control", "no-cache");
    co_return rt::Response(stream, {.status = 200, .headers = headers});
  }

  rt::Task<rt::Response> handleGeminiResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const tel::UsageContext* usageContext, const GeminiHandlerOptions& handlerOptions) {
    static_cast<void>(req);
    std::optional<GeminiCliTurn> turn;
    rt::Reason failure;
    bool admitted = false;
    bool streamRequested = false;
    try {
      const auto model = geminiUpstreamModelFor(modelRaw);
      if (!model) throw GeminiError(400, "gemini_model_unknown", "Gemini subscription does not support model " + modelRaw + ".");
      if (hasDeclaredTools(rawRecord)) throw GeminiError(400, "gemini_tools_unsupported", "Gemini subscription Responses is text-only and does not translate caller tools.");
      const auto input = ::uos::responses_request::normalizeResponsesInput(rt::get(rawRecord, "input"));
      if (const auto* invalidInput = std::get_if<::uos::responses_request::ResponsesStepNotOk>(&input)) co_return invalidInput->response;
      auto prepared = prepareCommon(rawRecord, promptForResponses(rawRecord,
        std::get<::uos::responses_request::ResponsesStepOk<std::vector<::uos::types::ResponseInputItem>>>(input).value));
      streamRequested = prepared.stream;
      prepared.reasoning = reasoningForModel(*model);
      if (usageContext && usageContext->responseTelemetry) {
        usageContext->responseTelemetry->provider = "gemini";
        usageContext->responseTelemetry->reasoning = prepared.reasoning;
        usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(rt::get(rawRecord, "max_output_tokens"));
        usageContext->responseTelemetry->semanticOutputObserved = false;
      }
      const tel::RecordRequestUsageDetails details{.model = modelRaw, .route = "responses", .stream = prepared.stream, .reasoning = prepared.reasoning};
      co_await tel::recordRequestUsage(usageContext, details).wait();
      auto task = runWithAdmission(prepared, *model, usageContext, handlerOptions, admitted);
      turn = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (admitted) health::recordGeminiProviderHealth("upstream_error", 502);
      co_return failureResponse(failure, usageContext);
    }
    const auto usage = geminiUsageTokens(turn->usage);
    const double created = rt::Math::floor(rt::dateNow() / 1000);
    const std::string rawId = turn->conversationId.value_or("");
    const std::string responseId = normalizedId(rawId, "resp_");
    const std::string chatId = normalizedId(rawId, "chatcmpl-");
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = turn->conversationId;
    const auto echo = responsesEcho(rawRecord);
    const auto completion = syntheticCompletion(chatId, modelRaw, created, turn->response, usage);
    rt::Headers headers{{"x-uos-upstream", "gemini"}};
    if (turn->conversationId) headers.set("x-uos-provider-request-id", *turn->conversationId);
    if (handlerOptions.refreshQuota) {
      rt::spawn(admin::gemini_quota::refreshSnapshot(rt::path::resolve(rt::process::cwd(), ".data"),
                  admin::gemini_quota::GeminiQuotaOptions{.runner = handlerOptions.runner, .force_refresh = true}, rt::dateNow()),
        rt::ignoreRejection);
    }
    chat::stream_translation::markChatSemanticOutput(usageContext);
    tel::recordTerminalUsage(usageContext, usage, true);
    tel::recordStreamTerminalType(usageContext, "response.completed");
    tel::recordStreamTerminal(usageContext);
    health::recordGeminiProviderHealth("success", 200, {}, turn->conversationId);
    auto body = payload::toDeepSeekResponsesPayload(completion, modelRaw, responseId, echo);
    if (!turn->deniedActions.empty()) body["uos_warnings"] = json::value_from(turn->deniedActions);
    if (!streamRequested) co_return http::json(200, body, headers);
    auto translator = deepseek::responses_stream::createDeepSeekResponsesStreamTranslator(modelRaw, responseId, echo, created);
    auto events = translator.open();
    auto pushed = translator.push(json::object{{"choices", json::array{json::object{{"index", rt::makeNumber(0.0)},
      {"delta", json::object{{"role", "assistant"}, {"content", turn->response}}}, {"finish_reason", "stop"}}}}, {"usage", *rt::get(completion, "usage")}});
    events.insert(events.end(), pushed.begin(), pushed.end());
    auto terminal = translator.finish();
    events.insert(events.end(), terminal.begin(), terminal.end());
    std::string encoded;
    double sequence = 0;
    for (auto& event : events) {
      rt::objectSet(event, "sequence_number", rt::makeNumber(sequence++));
      encoded += deepseek::responses_stream::encodeResponsesEvent(event);
    }
    headers.set("Content-Type", "text/event-stream");
    headers.set("Cache-Control", "no-cache");
    co_return rt::Response(encoded, {.status = 200, .headers = headers});
  }
}
