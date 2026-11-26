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
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <array>
#include <csignal>
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
    constexpr std::size_t MAX_PROMPT_BYTES = 8 * 1024 * 1024;
    constexpr auto MAX_STDOUT_BYTES = rt::limits::kJsonDocumentBytes;
    constexpr auto MAX_STDERR_BYTES = std::min<std::size_t>(128 * 1024, rt::limits::kTextInputBytes);
    constexpr auto MAX_LINE_BYTES = std::min<std::size_t>(4 * 1024 * 1024, rt::limits::kSseLineBytes);

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

    const json::object* declaredTool(const json::value& tool) {
      if (!tool.is_object()) return nullptr;
      const auto* function = rt::getObject(tool, "function");
      return function ? function : &tool.as_object();
    }

    bool hasDeclaredTool(const json::object& rawRecord, std::string_view name) {
      const auto* tools = rt::getArray(rawRecord, "tools");
      if (!tools) return false;
      return std::any_of(tools->begin(), tools->end(), [name](const auto& tool) {
        const auto* function = declaredTool(tool);
        return function && rt::getString(*function, "name") == name;
      });
    }

    struct ToolCall {
      std::string name;
      std::string arguments;
    };

    std::optional<std::string> textFromContent(const ::uos::types::MessageContentItem& item) {
      if (const auto* text = std::get_if<::uos::types::MessageContentItemInputText>(&item)) return text->text;
      if (const auto* text = std::get_if<::uos::types::MessageContentItemOutputText>(&item)) return text->text;
      return std::nullopt;
    }

    struct Prompt {
      std::string text;
      std::optional<std::string> schema{};
    };

    std::string jsonText(const json::value& value) {
      return value.is_string() ? rt::toString(value) : rt::jsonStringify(value);
    }

    void appendGenericInputItem(std::string& prompt, const json::object& item) {
      const auto type = rt::getString(item, "type").value_or("");
      if (type == "function_call" || type == "custom_tool_call") {
        const auto name = rt::getString(item, "name").value_or("");
        const auto arguments = rt::get(item, "arguments") ? jsonText(*rt::get(item, "arguments")) : rt::getString(item, "input").value_or("");
        prompt += "\n\nAssistant tool call:\n" + std::string(name) + "\nArguments:\n" + std::string(arguments);
        return;
      }
      if (type == "function_call_output" || type == "custom_tool_call_output") {
        const auto callId = rt::getString(item, "call_id").value_or("");
        const auto* output = rt::get(item, "output");
        prompt += "\n\nTool result for " + std::string(callId) + ":\n" + (output ? jsonText(*output) : std::string());
        return;
      }
      if (type == "reasoning") return;
      throw GeminiError(400, "gemini_unsupported_content", "Gemini subscription does not support input item type " + std::string(type) + ".");
    }

    void appendToolProtocol(std::string& prompt, const json::object& rawRecord) {
      const auto* tools = rt::get(rawRecord, "tools");
      if (!tools || tools->is_null() || (tools->is_array() && tools->as_array().empty())) {
        prompt += "\n\nAnswer the latest user request directly. Do not invoke tools, browse, execute commands, or modify files.";
        return;
      }
      prompt += "\n\nThe Codex client owns tool execution. Available tool declarations:\n" + rt::jsonStringify(*tools);
      prompt += "\n\nWhen a tool is needed, reply with exactly one JSON object and no markdown or other text: "
                R"({"type":"tool_call","name":"<exact tool name>","arguments":{...}})";
      prompt += "\nUse only a name present in the declarations and supply arguments that satisfy its parameter schema. "
                "The client will execute the call and return a function_call_output item. When no tool is needed, answer normally without a tool_call object.";
    }

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
        appendGenericInputItem(result.text, std::get<json::object>(item));
      }
      if (rt::trim(result.text).empty()) throw GeminiError(400, "gemini_empty_prompt", "Gemini subscription requires non-empty input.");
      appendToolProtocol(result.text, rawRecord);
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
          appendGenericInputItem(result.text, std::get<json::object>(item));
        }
      }
      if (rt::trim(result.text).empty()) throw GeminiError(400, "gemini_empty_prompt", "Gemini subscription requires non-empty messages.");
      appendToolProtocol(result.text, rawRecord);
      result.schema = jsonSchemaForChat(rawRecord);
      return result;
    }

    std::optional<ToolCall> parseToolCall(const std::string& response, const json::object& rawRecord) {
      const std::string trimmed = rt::trim(response);
      if (trimmed.empty() || trimmed.front() != '{') return std::nullopt;
      json::value parsed;
      try {
        parsed = rt::jsonParse(trimmed);
      } catch (...) {
        return std::nullopt;
      }
      if (!parsed.is_object()) return std::nullopt;
      const auto type = rt::getString(parsed, "type");
      if (type != std::optional<std::string_view>("tool_call") && type != std::optional<std::string_view>("function_call")) return std::nullopt;
      const auto name = rt::getString(parsed, "name");
      if (!name || !hasDeclaredTool(rawRecord, *name)) return std::nullopt;
      const auto* arguments = rt::get(parsed, "arguments");
      return ToolCall{std::string(*name), arguments ? jsonText(*arguments) : std::string("{}")};
    }

    std::string agyPath() {
      const auto home = rt::env::get("HOME");
      if (!home || home->empty()) throw GeminiError(503, "gemini_cli_unavailable", "The Antigravity CLI home is unavailable.");
      const std::string path = rt::path::join(*home, ".local/bin/agy");
      try {
        const auto info = rt::fs::lstatSync(path);
        if (info.isFile() && !info.isSymbolicLink() && info.uid == rt::process::getuid() && info.nlink == 1 &&
            (info.mode & 077) == 0 && (info.mode & 0100) != 0) return path;
      } catch (const rt::fs::ErrnoError&) {
      }
      throw GeminiError(503, "gemini_cli_unavailable", "The owner-private Antigravity CLI executable is unavailable.");
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

    using PipeReader = rt::ReadableStreamDefaultReader<rt::ByteChunk>;

    rt::Task<std::string> readBounded(PipeReader reader, std::size_t maximum, std::string_view label, bool jsonLines = false) {
      std::string output;
      std::size_t lineBytes = 0;
      for (;;) {
        auto reading = reader.read();
        const auto result = co_await std::move(reading);
        if (result.done) break;
        if (!result.value) continue;
        if (result.value->size() > maximum - output.size()) {
          throw GeminiError(502, "gemini_output_too_large", std::string(label) + " exceeded its gateway limit.");
        }
        const std::string_view bytes(reinterpret_cast<const char*>(result.value->data()), result.value->size());
        if (jsonLines) {
          for (const char byte : bytes) {
            if (byte == '\n') lineBytes = 0;
            else if (++lineBytes > MAX_LINE_BYTES)
              throw GeminiError(502, "gemini_output_too_large", "Antigravity emitted an oversized stream-json event.");
          }
        }
        output.append(bytes);
      }
      co_return output;
    }

    rt::Task<void> endChildInput(rt::process::FileSink sink) {
      const auto ending = sink.end();
      try {
        co_await ending.wait();
      } catch (const rt::fs::ErrnoError& error) {
        // A CLI that exits early may still send a terminal error on its output pipes.
        if (error.code != "EPIPE") throw;
        sink.close();
      }
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

    std::function<void(const std::string&)> childConfinement;

    void logCliFailure(std::string_view label, std::string_view detail) {
      if (!detail.empty()) upstream_wire::logRedactedUpstreamError(std::string(label), rt::makeReason(rt::JsError("Antigravity diagnostic present")));
    }

    GeminiCliTurn parseCliOutput(std::string_view output, const std::string& stderrText, int exitCode) {
      std::optional<json::object> result;
      std::size_t start = 0;
      while (start < output.size()) {
        const std::size_t end = output.find('\n', start);
        const std::string_view line = output.substr(start, end == std::string_view::npos ? output.size() - start : end - start);
        start = end == std::string_view::npos ? output.size() : end + 1;
        if (rt::trim(line).empty()) continue;
        json::value event;
        try {
          event = rt::jsonParse(line);
        } catch (...) {
          throw GeminiError(502, "gemini_cli_invalid_json", "Antigravity emitted malformed stream-json output.");
        }
        if (!event.is_object()) continue;
        if (rt::getString(event, "event") == std::optional<std::string_view>("result")) {
          if (const auto* value = rt::getObject(event, "result")) result = *value;
        } else if (rt::getString(event, "status")) {
          result = event.as_object();
        }
      }
      if (!result) {
        if (exitCode != 0) {
          logCliFailure("[ai.ubq.fi] Antigravity CLI exited without a result:", stderrText);
          throw GeminiError(502, "gemini_cli_failed", "Antigravity CLI exited without a result.");
        }
        throw GeminiError(502, "gemini_cli_missing_result", "Antigravity CLI produced no result event.");
      }
      if (rt::getString(*result, "status") != std::optional<std::string_view>("SUCCESS") || exitCode != 0) {
        const auto error = rt::getString(*result, "error");
        logCliFailure("[ai.ubq.fi] Antigravity CLI returned an unsuccessful result:", error ? *error : stderrText);
        throw GeminiError(502, "gemini_cli_failed", "Antigravity CLI returned an unsuccessful result.");
      }
      GeminiCliTurn turn;
      const auto response = rt::getString(*result, "response");
      if (response) turn.response = *response;
      turn.conversationId = upstream_wire::normalizeProviderRequestId(rt::get(*result, "conversation_id"));
      if (const auto* usage = rt::getObject(*result, "usage")) turn.usage = *usage;
      turn.deniedActions = deniedActions(rt::get(*result, "denied_actions"));
      if (rt::trim(turn.response).empty() && !turn.deniedActions.empty()) {
        throw GeminiError(502, "gemini_tool_denied", "Antigravity requested a tool that headless gateway inference cannot approve.");
      }
      if (rt::trim(turn.response).empty()) throw GeminiError(502, "gemini_empty_response", "Antigravity returned an empty response.");
      turn.rawResult = std::move(*result);
      return turn;
    }
  }

  std::optional<std::string> geminiUpstreamModelFor(std::string_view model) {
    const std::string trimmed = rt::trim(model);
    return geminiModelDefinition(trimmed) ? std::optional<std::string>(std::move(trimmed)) : std::nullopt;
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
  void setGeminiChildConfinement(std::function<void(const std::string&)> confine) { childConfinement = std::move(confine); }

  rt::Task<GeminiCliTurn> runGeminiCli(GeminiCliOptions options) {
    if (testRunner()) return runGeminiCli(options, *testRunner());
    return runGeminiCli(std::move(options), {});
  }

  rt::Task<GeminiCliTurn> runGeminiCli(GeminiCliOptions options, GeminiCliRunner runner) {
    if (upstream_auth::followerMode() || (!runner && !geminiOperatorOptIn()))
      throw GeminiError(503, "gemini_disabled", "Gemini subscription inference is not enabled on this gateway.");
    if (runner) {
      auto task = runner(options);
      co_return co_await std::move(task);
    }
    if (options.prompt.size() > MAX_PROMPT_BYTES) throw GeminiError(413, "gemini_prompt_too_large", "Gemini subscription prompt exceeds 8 MiB.");
    if (options.signal && options.signal->aborted()) rt::rethrow(options.signal->reason());
    const rt::AbortController deadline;
    const auto timer = rt::setTimeout([deadline] {
      deadline.abort(rt::makeReason(rt::DOMException("Antigravity CLI exceeded the gateway deadline.", "TimeoutError")));
    }, std::min(options.timeoutMs, 120'000.0));
    const auto clearDeadline = rt::scopeExit([&] { rt::clearTimeout(timer); });
    const auto signal = options.signal ? rt::AbortSignal::any({*options.signal, deadline.signal()}) : deadline.signal();
    const auto home = rt::env::get("HOME");
    if (!home || home->empty()) throw GeminiError(503, "gemini_cli_unavailable", "The Antigravity CLI home is unavailable.");
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
    std::string input;
    if (options.streamJson) {
      const json::object event{{"event", "user"}, {"message", json::object{{"content", options.prompt}}}};
      input = rt::jsonStringify(event) + "\n";
    }
    std::vector<std::pair<std::string, std::string>> environment{
      {"HOME", *home}, {"PATH", "/usr/local/bin:/usr/bin:/bin"}, {"TMPDIR", directory}, {"LANG", "C.UTF-8"}};
    rt::process::SpawnOptions spawnOptions{.cwd = directory, .env = std::move(environment),
      .stdin = options.streamJson ? rt::process::Stdio::pipe : rt::process::Stdio::ignore, .stdout = rt::process::Stdio::pipe, .stderr = rt::process::Stdio::pipe};
    if (rt::process::platform == "linux") {
      spawnOptions.confine = [&directory] { if (childConfinement) childConfinement(directory); };
    }
    const auto child = [&] {
      try {
        return rt::process::spawn(std::move(command), std::move(spawnOptions));
      } catch (...) {
        upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Antigravity CLI could not start:", std::current_exception());
        throw GeminiError(503, "gemini_cli_unavailable", "The Antigravity CLI could not start.");
      }
    }();
    const auto terminateChild = rt::scopeExit([&] { child.kill(SIGKILL); });
    const auto exited = child.exited();
    std::optional<rt::process::FileSink> stdinPipe;
    std::optional<PipeReader> stdoutPipe, stderrPipe;
    const auto stop = [&] {
      child.kill(SIGKILL);
      if (stdinPipe) stdinPipe->close();
      if (stdoutPipe) static_cast<void>(stdoutPipe->cancelPromise().catch_(rt::ignoreRejection));
      if (stderrPipe) static_cast<void>(stderrPipe->cancelPromise().catch_(rt::ignoreRejection));
    };
    rt::EventListener abort(stop);
    const auto removeAbort = rt::scopeExit([&] { signal.removeEventListener("abort", abort); });
    rt::Reason failure;
    try {
      if (options.onDispatch) options.onDispatch();
      if (options.onInit) options.onInit();
      stdoutPipe.emplace(child.stdout().getReader());
      stderrPipe.emplace(child.stderr().getReader());
      if (signal.aborted()) rt::rethrow(signal.reason());
      signal.addEventListener("abort", abort, {.once = true});

      const rt::Promise<std::string> stdoutBody(readBounded(*stdoutPipe, MAX_STDOUT_BYTES, "Antigravity stdout", true));
      const rt::Promise<std::string> stderrBody(readBounded(*stderrPipe, MAX_STDERR_BYTES, "Antigravity stderr"));
      auto inputWritten = rt::Promise<void>::resolve();
      if (options.streamJson) {
        stdinPipe.emplace(child.stdin());
        stdinPipe->write(input);
        inputWritten = rt::Promise<void>(endChildInput(*stdinPipe));
      }
      auto finished = rt::all(stdoutBody, stderrBody, inputWritten, exited);
      const auto output = co_await finished.wait();
      if (signal.aborted()) rt::rethrow(signal.reason());
      co_return parseCliOutput(std::get<0>(output), std::get<1>(output), std::get<3>(output));
    } catch (...) {
      failure = std::current_exception();
    }
    stop();
    const auto reaped = co_await exited.wait();
    static_cast<void>(reaped);
    if (signal.aborted()) rt::rethrow(signal.reason());
    rt::rethrow(failure);
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
      const auto* definition = geminiModelDefinition(model);
      return definition ? std::string(definition->reasoning_effort) : std::string();
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
      const std::optional<tel::UsageTokens>& usage, const std::optional<ToolCall>& toolCall = std::nullopt,
      const std::string& callId = "") {
      json::object message{{"role", "assistant"}};
      std::string finishReason = "stop";
      if (toolCall) {
        message["content"] = nullptr;
        message["tool_calls"] = json::array{json::object{{"id", callId}, {"type", "function"},
          {"function", json::object{{"name", toolCall->name}, {"arguments", toolCall->arguments}}}}};
        finishReason = "tool_calls";
      } else {
        message["content"] = text;
      }
      json::object result{{"id", id}, {"object", "chat.completion"}, {"created", rt::makeNumber(created)}, {"model", model},
        {"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"message", std::move(message)}, {"finish_reason", finishReason}}}}};
      if (usage) result["usage"] = chatUsageObject(*usage);
      return result;
    }

    rt::Response failureResponse(const rt::Reason& reason, const tel::UsageContext* context) {
      if (reason) {
        try {
          std::rethrow_exception(reason);
        } catch (const api_key_policy::ApiKeyQuotaDispatchError& error) {
          tel::recordStreamTerminalType(context, "error");
          return upstream_wire::apiKeyQuotaDispatchErrorResponse(error);
        } catch (...) {
        }
      }
      if (const auto* error = geminiErrorOf(reason)) {
        tel::recordStreamTerminalType(context, "error");
        return http::openaiError(error->status_code, error->message, error->code);
      }
      if (reason && rt::isError(reason)) {
        const auto name = rt::errorName(reason);
        if (name == "AbortError") {
          tel::recordStreamTerminalType(context, "cancelled");
          return http::openaiError(499, "Request was cancelled.", "request_cancelled", {.type = "server_error", .param = std::optional<std::string>{}});
        }
        if (name == "TimeoutError") {
          tel::recordStreamTerminalType(context, "deadline");
          return http::openaiError(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", {.type = "server_error"});
        }
      }
      upstream_wire::logRedactedUpstreamError("[ai.ubq.fi] Antigravity CLI failed:", reason);
      tel::recordStreamTerminalType(context, "error");
      return http::openaiError(502, "Antigravity subscription failed to produce a response.", "gemini_upstream_error");
    }

    bool shouldRecordGeminiUpstreamFailure(const rt::Reason& reason) {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return false;
      } catch (const GeminiError& error) {
        return error.status_code >= 500 && error.status_code != 503;
      } catch (const rt::JsError& error) {
        return error.name != "AbortError" && error.name != "TimeoutError";
      } catch (...) {
        return true;
      }
    }

    rt::Response unavailableCliResponse() {
      if (geminiOperatorOptIn()) return http::openaiError(503, "The Antigravity CLI is unavailable.", "gemini_cli_unavailable");
      return http::openaiError(503, "Gemini subscription inference is not enabled on this gateway.", "gemini_disabled");
    }

    struct GeminiAdmissionGuard {
      std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
      bool transportStarted = false;
    };

    rt::Task<GeminiCliTurn> runWithAdmission(const Prepared& prepared, const std::string& model, const tel::UsageContext* context,
      const GeminiHandlerOptions& options, const rt::AbortSignal& signal, bool& admitted) {
      auto admission = std::make_shared<GeminiAdmissionGuard>();
      rt::Reason failure;
      try {
        if (signal.aborted()) rt::rethrow(signal.reason());
        if (context && context->beforeProviderDispatch) {
          auto dispatching = context->beforeProviderDispatch("gemini");
          admission->dispatch = co_await std::move(dispatching);
        }
        if (signal.aborted()) rt::rethrow(signal.reason());
        tel::recordAttemptedProvider(context, "gemini");
        GeminiCliOptions optionsForCli{.model = model, .prompt = prepared.prompt.text, .signal = signal, .jsonSchema = prepared.prompt.schema,
          .onDispatch = [context, admission, &admitted] {
            if (admission->dispatch) admission->dispatch->markTransportStarted();
            admission->transportStarted = true;
            admitted = true;
            tel::recordFirstProviderDispatch(context);
          },
          .onInit = [context] { tel::recordFirstProviderHeaders(context); }};
        auto task = runGeminiCli(optionsForCli, options.runner);
        auto turn = co_await std::move(task);
        tel::recordFirstProviderHeaders(context);
        co_return turn;
      } catch (...) {
        failure = std::current_exception();
      }
      if (admission->dispatch && !admission->transportStarted) {
        auto cancellation = admission->dispatch->cancelBeforeTransport();
        co_await std::move(cancellation);
      }
      std::rethrow_exception(failure);
    }
  }

  rt::Task<rt::Response> handleGeminiChatCompletions(rt::Request req, json::object rawRecord, std::string modelRaw,
    const tel::UsageContext* usageContext, GeminiHandlerOptions handlerOptions) {
    const rt::AbortSignal requestSignal = openai::inferenceSignal(req, usageContext);
    GeminiHandlerOptions effectiveOptions = handlerOptions;
    if (!effectiveOptions.runner && testRunner()) effectiveOptions.runner = *testRunner();
    if (!effectiveOptions.runner && !geminiCliConfigured())
      co_return unavailableCliResponse();
    std::optional<GeminiCliTurn> turn;
    rt::Reason failure;
    bool admitted = false;
    bool streamRequested = false;
    bool includeUsage = false;
    try {
      const auto model = geminiUpstreamModelFor(modelRaw);
      if (!model) throw GeminiError(400, "gemini_model_unknown", "Gemini subscription does not support model " + modelRaw + ".");
      auto prepared = prepareCommon(rawRecord, promptForChat(rawRecord));
      prepared.reasoning = reasoningForModel(*model);
      streamRequested = prepared.stream;
      includeUsage = prepared.includeUsage;
      if (usageContext && usageContext->responseTelemetry) {
        usageContext->responseTelemetry->provider = "gemini";
        usageContext->responseTelemetry->reasoning = prepared.reasoning;
        usageContext->responseTelemetry->outputTokenAllowance = rt::getNumber(rt::get(rawRecord, "max_completion_tokens"));
      }
      const tel::RecordRequestUsageDetails details{.model = modelRaw, .route = "chat.completions", .stream = prepared.stream, .reasoning = prepared.reasoning};
      const auto recording = tel::recordRequestUsage(usageContext, details);
      co_await recording.wait();
      auto task = runWithAdmission(prepared, *model, usageContext, effectiveOptions, requestSignal, admitted);
      turn = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (admitted && shouldRecordGeminiUpstreamFailure(failure)) health::recordGeminiProviderHealth("upstream_error", 502);
      co_return failureResponse(failure, usageContext);
    }
    const auto usage = geminiUsageTokens(turn->usage);
    const double created = rt::Math::floor(rt::dateNow() / 1000);
    const std::string id = normalizedId(turn->conversationId.value_or(""), "chatcmpl-");
    const auto toolCall = parseToolCall(turn->response, rawRecord);
    const std::string callId = toolCall ? normalizedId(turn->conversationId.value_or("") + toolCall->name + toolCall->arguments, "call_") : std::string();
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = turn->conversationId;
    if (effectiveOptions.refreshQuota) {
      rt::spawn(admin::gemini_quota::refreshSnapshot(rt::path::resolve(rt::process::cwd(), ".data"),
                  admin::gemini_quota::GeminiQuotaOptions{.runner = effectiveOptions.runner, .force_refresh = true}, rt::dateNow()),
        rt::ignoreRejection);
    }
    chat::stream_translation::markChatSemanticOutput(usageContext);
    tel::recordTerminalUsage(usageContext, usage, true);
    tel::recordStreamTerminalType(usageContext, "response.completed");
    tel::recordStreamTerminal(usageContext);
    health::recordGeminiProviderHealth("success", 200, {}, turn->conversationId);
    rt::Headers headers{{"x-uos-upstream", "gemini"}};
    if (turn->conversationId) headers.set("x-uos-provider-request-id", *turn->conversationId);
    if (!streamRequested) {
      auto body = syntheticCompletion(id, modelRaw, created, turn->response, usage, toolCall, callId);
      if (!turn->deniedActions.empty()) body["uos_warnings"] = json::array{"Antigravity requested a tool that headless gateway inference cannot approve."};
      co_return http::json(200, body, headers);
    }
    json::object delta{{"role", "assistant"}};
    const std::string finishReason = toolCall ? "tool_calls" : "stop";
    if (toolCall) {
      delta["tool_calls"] = json::array{json::object{{"index", rt::makeNumber(0.0)}, {"id", callId}, {"type", "function"},
        {"function", json::object{{"name", toolCall->name}, {"arguments", toolCall->arguments}}}}};
    } else {
      delta["content"] = turn->response;
    }
    std::string stream = "data: " + rt::jsonStringify(json::object{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)},
      {"model", modelRaw}, {"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"delta", std::move(delta)}, {"finish_reason", finishReason}}}}}) + "\n\n";
    if (includeUsage && usage) stream += "data: " + rt::jsonStringify(json::object{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)},
      {"model", modelRaw}, {"choices", json::array{}}, {"usage", chatUsageObject(*usage)}}) + "\n\n";
    stream += "data: [DONE]\n\n";
    headers.set("Content-Type", "text/event-stream");
    headers.set("Cache-Control", "no-cache");
    co_return rt::Response(stream, {.status = 200, .headers = headers});
  }

  rt::Task<rt::Response> handleGeminiResponses(rt::Request req, json::object rawRecord, std::string modelRaw,
    const tel::UsageContext* usageContext, GeminiHandlerOptions handlerOptions) {
    const rt::AbortSignal requestSignal = openai::inferenceSignal(req, usageContext);
    GeminiHandlerOptions effectiveOptions = handlerOptions;
    if (!effectiveOptions.runner && testRunner()) effectiveOptions.runner = *testRunner();
    if (!effectiveOptions.runner && !geminiCliConfigured())
      co_return unavailableCliResponse();
    std::optional<GeminiCliTurn> turn;
    rt::Reason failure;
    bool admitted = false;
    bool streamRequested = false;
    try {
      const auto model = geminiUpstreamModelFor(modelRaw);
      if (!model) throw GeminiError(400, "gemini_model_unknown", "Gemini subscription does not support model " + modelRaw + ".");
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
      const auto recording = tel::recordRequestUsage(usageContext, details);
      co_await recording.wait();
      auto task = runWithAdmission(prepared, *model, usageContext, effectiveOptions, requestSignal, admitted);
      turn = co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (admitted && shouldRecordGeminiUpstreamFailure(failure)) health::recordGeminiProviderHealth("upstream_error", 502);
      co_return failureResponse(failure, usageContext);
    }
    const auto usage = geminiUsageTokens(turn->usage);
    const double created = rt::Math::floor(rt::dateNow() / 1000);
    const std::string rawId = turn->conversationId.value_or("");
    const std::string responseId = normalizedId(rawId, "resp_");
    const std::string chatId = normalizedId(rawId, "chatcmpl-");
    const auto toolCall = parseToolCall(turn->response, rawRecord);
    const std::string callId = toolCall ? normalizedId(rawId + toolCall->name + toolCall->arguments, "call_") : std::string();
    if (usageContext && usageContext->responseTelemetry) usageContext->responseTelemetry->providerRequestId = turn->conversationId;
    const auto echo = responsesEcho(rawRecord);
    const auto completion = syntheticCompletion(chatId, modelRaw, created, turn->response, usage, toolCall, callId);
    rt::Headers headers{{"x-uos-upstream", "gemini"}};
    if (turn->conversationId) headers.set("x-uos-provider-request-id", *turn->conversationId);
    if (effectiveOptions.refreshQuota) {
      rt::spawn(admin::gemini_quota::refreshSnapshot(rt::path::resolve(rt::process::cwd(), ".data"),
                  admin::gemini_quota::GeminiQuotaOptions{.runner = effectiveOptions.runner, .force_refresh = true}, rt::dateNow()),
        rt::ignoreRejection);
    }
    chat::stream_translation::markChatSemanticOutput(usageContext);
    tel::recordTerminalUsage(usageContext, usage, true);
    tel::recordStreamTerminalType(usageContext, "response.completed");
    tel::recordStreamTerminal(usageContext);
    health::recordGeminiProviderHealth("success", 200, {}, turn->conversationId);
    auto body = payload::toDeepSeekResponsesPayload(completion, modelRaw, responseId, echo);
    if (!turn->deniedActions.empty()) body["uos_warnings"] = json::array{"Antigravity requested a tool that headless gateway inference cannot approve."};
    if (!streamRequested) co_return http::json(200, body, headers);
    auto translator = deepseek::responses_stream::createDeepSeekResponsesStreamTranslator(modelRaw, responseId, echo, created);
    auto events = translator.open();
    json::object delta{{"role", "assistant"}};
    const std::string finishReason = toolCall ? "tool_calls" : "stop";
    if (toolCall) {
      delta["tool_calls"] = json::array{json::object{{"index", rt::makeNumber(0.0)}, {"id", callId}, {"type", "function"},
        {"function", json::object{{"name", toolCall->name}, {"arguments", toolCall->arguments}}}}};
    } else {
      delta["content"] = turn->response;
    }
    json::object completionChunk{{"choices", json::array{json::object{{"index", rt::makeNumber(0.0)}, {"delta", std::move(delta)}, {"finish_reason", finishReason}}}}};
    if (usage) completionChunk["usage"] = chatUsageObject(*usage);
    auto pushed = translator.push(completionChunk);
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
