#include "uos/lib/jev_compaction/jev_codex_smoke.hpp"
#include "uos/lib/jev_compaction/codex_items.hpp"
#include "uos/lib/jev_compaction/types.hpp"
#include "uos/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/handler/admission.hpp"
#include "uos/handler/index.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/jev_compaction/compaction.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/storage/kv.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <utility>

namespace uos::lib::jev_compaction::jev_codex_smoke {
  namespace {
    inline constexpr std::string_view CODEX_BIN = "codex";
    inline constexpr double DEADLINE_MS = 240'000;
    inline constexpr double STEP_TIMEOUT_MS = 30'000;
    inline const std::string GATEWAY_TOKEN = "u_" + std::string(64, 'd');
    inline constexpr std::string_view COMPACTION_PROMPT_MARKER = "CONTEXT CHECKPOINT COMPACTION";
    inline constexpr std::string_view DUMMY_DEEPSEEK_KEY = "jev-codex-smoke-dummy-deepseek-key";
    inline constexpr std::string_view JEV_API_ORIGIN = "https://api.typesafe.ai/";
    inline constexpr std::string_view GAMMA_MARKER = "GAMMA_REQUIRED_FACT_KEEP_VERBATIM";
    inline constexpr std::string_view ALPHA_MARKER = "ALPHA_OBSOLETE_PROBE";
    inline constexpr std::string_view BETA_HEAD = "BETA_OBSOLETE_SCAN_HEAD";
    inline constexpr std::string_view BETA_TAIL = "BETA_OBSOLETE_TAIL_MUST_BE_TRUNCATED";
    inline const std::string BETA_FILLER = rt::repeat("beta obsolete filler text ", 120);
    inline constexpr std::string_view GOAL_TEXT = "Goal: keep the Gamma release fact verbatim; the obsolete Alpha build probe and the obsolete Beta metrics line are no longer needed.";
    inline constexpr std::string_view POST_TURN = "post-compaction probe";
    inline constexpr std::string_view FILLER_ONE = "filler turn one";
    inline constexpr std::string_view FILLER_TWO = "filler turn two";

    std::string octalEscapes(const std::string& text) {
      std::string result;
      for (const auto& character : rt::codePoints(text)) {
        auto point = character;
        std::string octal;
        do { octal.insert(octal.begin(), static_cast<char>('0' + point % 8)); point /= 8; } while (point);
        result += "\\" + rt::padStart(octal, 3, "0");
      }
      return result;
    }
    inline const std::string ALPHA_COMMAND = "printf '%s' '" + std::string(ALPHA_MARKER) + ": superseded build probe; obsolete and safe to delete'";
    inline const std::string BETA_COMMAND = "printf '" + std::string(BETA_HEAD) + ": superseded metrics; " + BETA_FILLER + octalEscapes(std::string(BETA_TAIL)) + "'";
    inline const std::string GAMMA_COMMAND = "printf '%s' '" + std::string(GAMMA_MARKER) + ": required release fact; keep verbatim'";
    void require_(bool condition, const std::string& message) { if (!condition) throw rt::JsError(message); }
    rt::Task<void> delay(double ms) { return rt::sleep(ms); }
    double remaining(double startedAt) { return DEADLINE_MS - (rt::dateNow() - startedAt); }
    std::string safeExcerpt(const std::string& value, double limit = 400) { return rt::slice(rt::replaceAll(value, "\n", " "), 0, limit); }
    json::object record(const json::value* value) { return value && value->is_object() ? value->as_object() : json::object{}; }
    json::object record(const json::value& value) { return record(&value); }
    std::string adoptedSummary(const std::string& body) {
      const auto start = rt::indexOf(body, codex_items::SUMMARY_MARKER);
      if (start < 0) return "";
      const auto end = rt::indexOf(body, POST_TURN, start);
      return end < 0 ? rt::slice(body, start) : rt::slice(body, start, end);
    }
    bool openRouterKeyPresent() {
      try { const auto key = rt::env::get("OPENROUTER_API_KEY"); return key && !key->empty(); }
      catch (...) { return false; }
    }
    struct UpstreamRequest { std::string method; std::string path; std::string body; };
    struct ChatTool { std::string name; json::object parameters; };
    struct ToolCallSpec { std::string name; std::string arguments; };
    std::optional<ChatTool> chatToolFrom(const json::value& value) {
      const auto tool = record(value);
      const auto fn = record(rt::get(tool, "function"));
      const auto name = rt::getString(fn, "name").value_or("");
      if (name.empty()) return std::nullopt;
      const auto parameters = record(rt::get(fn, "parameters"));
      return parameters.empty() ? std::nullopt : std::optional(ChatTool{std::string(name), parameters});
    }
    std::optional<ChatTool> pickShellTool(const json::array& tools) {
      std::vector<ChatTool> usable;
      for (const auto& tool : tools) {
        const auto parsed = chatToolFrom(tool);
        if (!parsed) continue;
        const auto properties = record(rt::get(parsed->parameters, "properties"));
        if (properties.contains("cmd") || properties.contains("command")) usable.push_back(*parsed);
      }
      const std::vector<std::string> preferred{"exec_command", "shell", "local_shell", "unified_exec", "container.exec"};
      for (const auto& name : preferred) for (const auto& tool : usable) if (tool.name == name) return tool;
      return usable.empty() ? std::nullopt : std::optional(usable.front());
    }
    std::optional<json::object> buildCallArguments(const ChatTool& tool, const std::string& command) {
      const auto properties = record(rt::get(tool.parameters, "properties"));
      json::object args;
      if (properties.contains("cmd")) {
        args.emplace("cmd", command);
        if (properties.contains("login")) args.emplace("login", false);
        if (properties.contains("yield_time_ms")) args.emplace("yield_time_ms", 10'000);
      } else if (properties.contains("command")) {
        const auto argSchema = record(rt::get(properties, "command"));
        if (rt::getString(argSchema, "type") == "string") args.emplace("command", command);
        else args.emplace("command", json::array{"bash", "-lc", command});
      } else return std::nullopt;
      if (properties.contains("timeout_ms")) args.emplace("timeout_ms", 10'000);
      return args;
    }
    std::string structureOf(const std::string& body) {
      json::object parsed;
      try { parsed = record(rt::jsonParse(body)); } catch (...) {}
      const auto* source = rt::get(parsed, "messages");
      const auto messages = source && source->is_array() ? source->as_array() : json::array{};
      std::set<std::string> roles;
      std::size_t assistantToolCalls = 0;
      std::size_t toolResults = 0;
      for (const auto& raw : messages) {
        const auto message = record(raw);
        const std::string role(rt::getString(message, "role").value_or("unknown"));
        roles.insert(role);
        const auto* calls = rt::get(message, "tool_calls");
        if (role == "assistant" && calls && calls->is_array()) assistantToolCalls += calls->as_array().size();
        if (role == "tool") ++toolResults;
      }
      std::vector<std::string> sortedRoles(roles.begin(), roles.end());
      std::sort(sortedRoles.begin(), sortedRoles.end(), [](const auto& a, const auto& b) { return rt::compare(a, b) < 0; });
      return "messages=" + rt::numberToString(static_cast<double>(messages.size())) + " roles=" + rt::join(sortedRoles, "|") +
        " assistant_tool_calls=" + rt::numberToString(static_cast<double>(assistantToolCalls)) + " tool_results=" + rt::numberToString(static_cast<double>(toolResults));
    }
    inline constexpr std::string_view CHAT_ID = "chatcmpl-jev-codex-smoke";
    inline constexpr std::array<std::string_view, 3> ALL_PROBES{ALPHA_MARKER, BETA_HEAD, GAMMA_MARKER};
    std::set<std::string> executedProbes(const json::value* messages) {
      std::set<std::string> found;
      if (!messages || !messages->is_array()) return found;
      for (const auto& raw : messages->as_array()) {
        const auto message = record(raw);
        const auto role = rt::getString(message, "role").value_or("");
        const auto* calls = rt::get(message, "tool_calls");
        if (role == "assistant" && calls && calls->is_array()) for (const auto& call : calls->as_array()) {
          const auto item = record(call);
          const auto fn = record(rt::get(item, "function"));
          const auto args = rt::getString(fn, "arguments");
          if (!args) continue;
          for (const auto probe : ALL_PROBES) if (args->find(probe) != std::string_view::npos) found.emplace(probe);
        }
        const auto content = rt::getString(message, "content");
        if (role == "tool" && content) for (const auto probe : ALL_PROBES) if (content->find(probe) != std::string_view::npos) found.emplace(probe);
      }
      return found;
    }
    std::string chatChunk(const json::object& delta, const std::optional<std::string>& finishReason, const std::optional<json::object>& usage = std::nullopt) {
      json::object chunk{{"id", CHAT_ID}, {"object", "chat.completion.chunk"}, {"created", 1'780'000'010}, {"model", "deepseek-flash"},
        {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", finishReason ? json::value(*finishReason) : json::value(nullptr)}}}}};
      if (usage) chunk.emplace("usage", *usage);
      return "data: " + rt::jsonStringify(chunk) + "\n\n";
    }
    class FakeChatUpstream : public std::enable_shared_from_this<FakeChatUpstream> {
      std::string script = "chain";
      bool chainServed = false;
      std::optional<runtime::http::HttpServer> server;
      std::optional<ChatTool> shellTool;
    public:
      std::vector<UpstreamRequest> requests;
      std::vector<std::string> toolNames;
      std::optional<double> reportedPromptTokens;
      bool reportedPromptTokensOnce = false;
      std::string url;
      const std::string& mode() const { return script; }
      void mode(std::string next) { script = std::move(next); chainServed = false; }
      static rt::Task<std::shared_ptr<FakeChatUpstream>> start() {
        auto fake = std::make_shared<FakeChatUpstream>();
        runtime::http::ServeHttpOptions options{.hostname = "127.0.0.1", .port = 0};
        runtime::http::ServeHandler handle = [weak = std::weak_ptr(fake)](rt::Request request, runtime::http::ServeHandlerInfo) {
          if (auto owner = weak.lock()) {
            return rt::async([owner = std::move(owner), request = std::move(request)] { return owner->handle(request); });
          }
          return rt::fulfilledTask(rt::Response("Fixture closed", {.status = 503}));
        };
        fake->server = runtime::http::serveHttp(options, handle);
        const auto addr = co_await fake->server->ready.wait();
        fake->url = "http://127.0.0.1:" + rt::numberToString(addr.port);
        co_return fake;
      }
      rt::Task<void> close() { if (server) { auto pending = server->shutdown(); co_await pending.wait(); } else co_await rt::microtask(); }
      std::vector<UpstreamRequest> compactionLeaks() const { return bodiesMatching(std::string(COMPACTION_PROMPT_MARKER)); }
      std::vector<UpstreamRequest> bodiesMatching(const std::string& needle) const {
        std::vector<UpstreamRequest> found;
        for (const auto& entry : requests) if (entry.body.find(needle) != std::string::npos) found.push_back(entry);
        return found;
      }
      std::string lastStructure() const { return requests.empty() ? "no-requests" : structureOf(requests.back().body); }
    private:
      rt::Task<rt::Response> handle(rt::Request request) {
        const auto url_ = rt::URL(request.url());
        std::string body;
        if (request.method() != "GET" && request.method() != "HEAD") {
          rt::Promise<std::string> pending(request.text());
          body = co_await pending.wait();
        }
        requests.push_back({request.method(), url_.pathname() + url_.search(), body});
        if (url_.pathname() == "/healthz") co_return rt::Response("fake chat upstream ready\n");
        json::object parsed;
        try { parsed = record(rt::jsonParse(body)); } catch (...) {}
        const auto* source = rt::get(parsed, "tools");
        const auto tools = source && source->is_array() ? source->as_array() : json::array{};
        toolNames.clear();
        for (const auto& tool : tools) if (auto parsedTool = chatToolFrom(tool)) toolNames.push_back(parsedTool->name);
        shellTool = pickShellTool(tools);
        const auto next = nextCall(executedProbes(rt::get(parsed, "messages")));
        std::vector<std::string> frames;
        if (next) {
          frames.push_back(chatChunk({{"role", "assistant"}, {"content", "Normal turn narration before " + next->name + "."}}, std::nullopt));
          auto uuid = rt::replaceAll(rt::crypto::randomUUID(), "-", "");
          const json::object delta{{"tool_calls", json::array{json::object{{"index", 0}, {"id", "call_" + rt::slice(uuid, 0, 12)}, {"type", "function"},
            {"function", json::object{{"name", next->name}, {"arguments", next->arguments}}}}}}};
          frames.push_back(chatChunk(delta, std::nullopt));
          frames.push_back(chatChunk({}, "tool_calls", usage(body, 24)));
        } else {
          const auto reply = mode() == "chain" ? "NORMAL_REPLY_MARKER_CHAIN_DONE" : "NORMAL_REPLY_MARKER_THREAD_TWO";
          frames.push_back(chatChunk({{"role", "assistant"}, {"content", reply}}, std::nullopt));
          frames.push_back(chatChunk({}, "stop", usage(body, 12)));
        }
        frames.push_back("data: [DONE]\n\n");
        rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"content-type", "text/event-stream"}, {"cache-control", "no-store"}}};
        co_return rt::Response(rt::join(frames, ""), init);
      }
      json::object usage(const std::string& body, double completionTokens) {
        const auto promptTokens = reportedPromptTokens.value_or(rt::Math::max(1.0, rt::Math::ceil(static_cast<double>(rt::length(body)) / 4)));
        if (reportedPromptTokensOnce) { reportedPromptTokens = 80; reportedPromptTokensOnce = false; }
        return {{"prompt_tokens", promptTokens}, {"completion_tokens", completionTokens}, {"total_tokens", promptTokens + completionTokens}};
      }
      std::optional<ToolCallSpec> nextCall(const std::set<std::string>& executed) {
        if (!shellTool || chainServed) return std::nullopt;
        std::vector<std::pair<std::string, std::string>> steps;
        if (script == "chain") steps = {{std::string(ALPHA_MARKER), ALPHA_COMMAND}, {std::string(BETA_HEAD), BETA_COMMAND}, {std::string(GAMMA_MARKER), GAMMA_COMMAND}};
        else steps = {{std::string(ALPHA_MARKER), ALPHA_COMMAND}};
        for (const auto& step : steps) if (!executed.contains(step.first)) {
          const auto args = buildCallArguments(*shellTool, step.second);
          if (args) return ToolCallSpec{shellTool->name, rt::jsonStringify(*args)};
        }
        chainServed = true;
        return std::nullopt;
      }
    };

    json::object answersFor(const types::JevState& state, const types::JevQuestions& questions) {
      std::map<std::string, std::string> actions;
      const auto stateRecord = record(state);
      const auto* history = rt::get(stateRecord, "history");
      if (history && history->is_array()) for (const auto& entry : history->as_array()) {
        const auto entryRecord = record(entry);
        const auto* calls = rt::get(entryRecord, "tool_calls");
        if (!calls || !calls->is_array()) continue;
        for (const auto& call : calls->as_array()) {
          const std::string text = call.is_string() ? std::string(call.as_string()) : rt::jsonStringify(call);
          std::string id;
          if (call.is_string()) id = text.substr(0, text.find(' '));
          else {
            const auto item = record(call);
            const auto* value = rt::get(item, "id");
            id = value && !value->is_null() ? rt::toString(*value) : "";
          }
          if (id.empty()) continue;
          if (text.find(ALPHA_MARKER) != std::string::npos) actions[id] = "drop_call";
          else if (text.find(BETA_HEAD) != std::string::npos) actions[id] = "drop_result";
          else if (text.find(GAMMA_MARKER) != std::string::npos) actions[id] = "keep";
        }
      }
      json::object answers;
      for (const auto& name : questions.keys()) {
        const auto id = name.starts_with("call_") ? name.substr(5) : name.starts_with("result_") ? name.substr(7) : name;
        const auto found = actions.find(id);
        const auto action = found == actions.end() ? "keep" : found->second;
        double noul = 0.95;
        if (action == "drop_call") noul = 0.1;
        else if (action == "drop_result") noul = name.starts_with("result_") ? 0.05 : 0.9;
        answers.emplace(name, json::object{{"noul", noul}});
      }
      return answers;
    }
    class FakeJev final : public types::JevAsker {
    public:
      std::string mode = "decide";
      double calls = 0;
      double questions = 0;
      rt::Task<types::JevResponse> ask(types::JevState state, types::JevQuestions questions_) override {
        calls += 1;
        questions += static_cast<double>(questions_.size());
        if (mode == "fail") return rt::Promise<types::JevResponse>::reject(rt::makeReason(rt::JsError("synthetic Jev outage"))).wait();
        return rt::fulfilledTask(types::JevResponse{{"model", "fake-jev"}, {"answers", answersFor(state, questions_)}});
      }
    };
    using RpcMessage = json::object;
    struct PendingWaiter {
      std::function<void(std::optional<json::value>)> resolve;
      std::function<void(rt::Reason)> reject;
    };
    class AppServer : public std::enable_shared_from_this<AppServer> {
      double nextId = 1;
      rt::OrderedMap<double, PendingWaiter> pending;
      rt::process::Subprocess child;
      std::vector<rt::Promise<void>> pumps;
      explicit AppServer(rt::process::Subprocess process) : child(std::move(process)) {}
    public:
      std::vector<RpcMessage> notifications;
      std::vector<std::string> serverRequests;
      std::string stderr;
      bool exited = false;
      static rt::Task<std::shared_ptr<AppServer>> start(std::string cwd, std::vector<std::pair<std::string, std::string>> env) {
        rt::process::SpawnOptions options{.cwd = cwd, .env = env, .stdin = rt::process::Stdio::pipe, .stdout = rt::process::Stdio::pipe, .stderr = rt::process::Stdio::pipe};
        const auto child = rt::process::spawn({std::string(CODEX_BIN), "app-server"}, options);
        auto server = std::shared_ptr<AppServer>(new AppServer(child));
        server->pumps.emplace_back(server->pumpStdout());
        server->pumps.emplace_back(server->pumpStderr());
        rt::Reason failure;
        try {
          const json::object params{{"clientInfo", json::object{{"name", "ai-ubq-fi-jev-smoke"}, {"title", "jev-smoke"}, {"version", "0.1.0"}}}};
          rt::Promise<std::optional<json::value>> initialized(server->request("initialize", params));
          co_await initialized.wait();
          server->notify("initialized", {});
        } catch (...) { failure = std::current_exception(); }
        if (failure) {
          rt::Promise<void> stopped(server->stop());
          co_await stopped.wait();
          std::rethrow_exception(failure);
        }
        co_return server;
      }
    private:
      rt::Task<void> pumpStdout() {
        auto reader = child.stdout().getReader();
        rt::TextDecoder decoder;
        std::string buffer;
        try {
          for (;;) {
            const auto next = co_await reader.read();
            if (next.done) break;
            buffer += decoder.decode(*next.value, {.stream = true});
            auto index = buffer.find('\n');
            while (index != std::string::npos) {
              const auto line = rt::trim(buffer.substr(0, index));
              buffer = buffer.substr(index + 1);
              if (!line.empty()) handleLine(line);
              index = buffer.find('\n');
            }
          }
        } catch (...) {}
        exited = true;
        for (const auto& waiter : pending.values()) waiter.reject(rt::makeReason(rt::JsError("codex app-server exited")));
        pending.clear();
      }
      rt::Task<void> pumpStderr() {
        auto reader = child.stderr().getReader();
        rt::TextDecoder decoder;
        try {
          for (;;) {
            const auto next = co_await reader.read();
            if (next.done) break;
            stderr = rt::slice(stderr + decoder.decode(*next.value, {.stream = true}), -4000);
          }
        } catch (...) {}
      }
      void handleLine(const std::string& line) {
        json::value parsed;
        try { parsed = rt::jsonParse(line); } catch (...) { return; }
        if (parsed.is_null()) throw rt::TypeError("null is not an object (evaluating 'message.id')");
        if (!parsed.is_object()) return;
        auto message = parsed.as_object();
        const auto* id = rt::get(message, "id");
        const auto* method = rt::get(message, "method");
        if (id && rt::truthy(method)) {
          serverRequests.push_back(rt::toString(*method));
          write(json::object{{"jsonrpc", "2.0"}, {"id", *id}, {"result", json::object{}}});
          return;
        }
        if (id) {
          const auto number = rt::toNumber(*id);
          const auto* current = pending.get(number);
          if (!current) return;
          const auto waiter = *current;
          pending.delete_(number);
          const auto* error = rt::get(message, "error");
          if (rt::truthy(error)) waiter.reject(rt::makeReason(rt::JsError(rt::slice(rt::jsonStringify(*error), 0, 300))));
          else { const auto* result = rt::get(message, "result"); waiter.resolve(result ? std::optional(*result) : std::nullopt); }
          return;
        }
        if (rt::truthy(method)) notifications.push_back(message);
      }
      void write(const json::value& payload) {
        child.stdin().write(rt::jsonStringify(payload) + "\n");
        (void)child.stdin().flush();
      }
    public:
      void notify(const std::string& method, const json::object& params) { write(json::object{{"jsonrpc", "2.0"}, {"method", method}, {"params", params}}); }
      rt::Task<std::optional<json::value>> request(std::string method, json::object params) {
        const auto id = nextId++;
        const auto resolvers = rt::withResolvers<std::optional<json::value>>();
        pending.set(id, PendingWaiter{resolvers.resolve, resolvers.reject});
        write(json::object{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}});
        const auto timeout = rt::newPromise<std::optional<json::value>>([method](auto, auto reject) {
          rt::setTimeout([method, reject] { reject(rt::makeReason(rt::JsError("timed out waiting for " + method + " response"))); }, STEP_TIMEOUT_MS);
        });
        std::vector<rt::Promise<std::optional<json::value>>> racers{resolvers.promise, timeout};
        const auto result = co_await rt::race(std::move(racers)).wait();
        co_return result;
      }
      std::size_t cursor() const { return notifications.size(); }
      rt::Task<RpcMessage> waitFor(std::function<bool(const RpcMessage&)> predicate, std::string label, double timeoutMs = STEP_TIMEOUT_MS, std::size_t from = 0) {
        const auto end = rt::dateNow() + timeoutMs;
        while (rt::dateNow() < end) {
          for (std::size_t i = std::min(from, notifications.size()); i < notifications.size(); ++i) if (predicate(notifications[i])) co_return notifications[i];
          if (exited) throw rt::JsError("codex app-server exited while waiting for " + label);
          rt::Promise<void> delayed(delay(50));
          co_await delayed.wait();
        }
        throw rt::JsError("timed out (" + rt::numberToString(timeoutMs) + "ms) waiting for " + label);
      }
      bool sawContextCompaction(std::size_t from) const {
        for (std::size_t i = std::min(from, notifications.size()); i < notifications.size(); ++i) {
          const auto& message = notifications[i];
          const auto params = record(rt::get(message, "params"));
          const auto item = record(rt::get(params, "item"));
          if (rt::getString(message, "method") == "item/completed" && rt::getString(item, "type") == "contextCompaction") return true;
        }
        return false;
      }
      rt::Task<void> stop() {
        try { child.kill(std::string_view("SIGTERM")); } catch (...) {}
        const auto childExited = child.exited().then([](int) { return true; });
        const auto gracePeriod = rt::Promise<void>(delay(2000)).then([] { return false; });
        std::vector<rt::Promise<bool>> racers{childExited, gracePeriod};
        const auto observedExit = co_await rt::race(std::move(racers)).wait();
        if (!observedExit) { try { child.kill(std::string_view("SIGKILL")); } catch (...) {} }
        auto finalExit = child.exited();
        co_await finalExit.wait();
        const auto settled = co_await rt::allSettled(pumps).wait();
        (void)settled;
        exited = true;
      }
    };
    struct CompactionOutcome { bool ok; std::string detail; };
    struct ThreadHandle {
      std::string id;
      std::function<rt::Task<std::vector<RpcMessage>>(std::string)> runTurn;
      std::function<rt::Task<CompactionOutcome>()> compact;
    };
    std::string threadIdOf(const json::value* result) {
      const auto value = record(result);
      const auto thread = record(rt::get(value, "thread"));
      const json::value* id = rt::get(thread, "id");
      if (!id || id->is_null()) id = rt::get(value, "threadId");
      if (!id || id->is_null()) id = rt::get(value, "id");
      if (!result) throw rt::TypeError("undefined is not an object (evaluating 'JSON.stringify(result).slice')");
      require_(id && id->is_string() && !id->as_string().empty(), "thread/start returned no thread id: " + rt::slice(rt::jsonStringify(*result), 0, 200));
      return std::string(id->as_string());
    }
    std::string statusOf(const RpcMessage& message) {
      const auto params = record(rt::get(message, "params"));
      const auto turn = record(rt::get(params, "turn"));
      const auto* status = rt::get(turn, "status");
      if (!status || status->is_null()) status = rt::get(params, "status");
      return !status || status->is_null() ? "unknown" : rt::toString(*status);
    }
    std::optional<std::string> turnIdOf(const RpcMessage& message) {
      const auto params = record(rt::get(message, "params"));
      const auto turn = record(rt::get(params, "turn"));
      auto* id = rt::get(params, "turnId");
      if (!id || id->is_null()) id = rt::get(turn, "id");
      return id && id->is_string() && !id->as_string().empty() ? std::optional(std::string(id->as_string())) : std::nullopt;
    }
    rt::Task<std::vector<RpcMessage>> runTurn(std::shared_ptr<AppServer> rpc, std::string id, std::string text) {
      const auto from = rpc->cursor();
      const json::object params{{"threadId", id}, {"input", json::array{json::object{{"type", "text"}, {"text", text}}}}};
      rt::Promise<std::optional<json::value>> started(rpc->request("turn/start", params));
      co_await started.wait();
      std::function<bool(const RpcMessage&)> predicate = [id](const RpcMessage& message) {
        const auto params_ = record(rt::get(message, "params"));
        return rt::getString(message, "method") == "turn/completed" && rt::getString(params_, "threadId") == id;
      };
      rt::Promise<RpcMessage> completedPending(rpc->waitFor(predicate, "turn/completed for " + rt::slice(text, 0, 40), STEP_TIMEOUT_MS, from));
      const auto completed = co_await completedPending.wait();
      const auto status = statusOf(completed);
      require_(status == "completed", "turn \"" + rt::slice(text, 0, 40) + "\" ended with status " + status);
      co_return std::vector<RpcMessage>(rpc->notifications.begin() + static_cast<std::ptrdiff_t>(std::min(from, rpc->notifications.size())), rpc->notifications.end());
    }
    rt::Task<CompactionOutcome> compact(std::shared_ptr<AppServer> rpc, std::string id) {
      const auto from = rpc->cursor();
      std::optional<std::string> rejected;
      try {
        const json::object params{{"threadId", id}};
        rt::Promise<std::optional<json::value>> started(rpc->request("thread/compact/start", params));
        co_await started.wait();
      } catch (...) { rejected = "compact/start rejected: " + safeExcerpt(rt::reasonToString(std::current_exception()), 200); }
      if (rejected) co_return CompactionOutcome{false, *rejected};
      const auto end = rt::dateNow() + STEP_TIMEOUT_MS;
      std::optional<std::string> compactTurnId;
      bool compacted = false;
      std::optional<std::string> failure;
      while (rt::dateNow() < end) {
        for (std::size_t i = std::min(from, rpc->notifications.size()); i < rpc->notifications.size(); ++i) {
          const auto& message = rpc->notifications[i];
          const auto params = record(rt::get(message, "params"));
          const bool forThread = rt::getString(params, "threadId") == id;
          const auto eventTurnId = turnIdOf(message);
          if (forThread && eventTurnId && !compactTurnId) compactTurnId = eventTurnId;
          const auto method = rt::getString(message, "method").value_or("");
          const auto item = record(rt::get(params, "item"));
          if (forThread && method == "item/completed" && rt::getString(item, "type") == "contextCompaction") compacted = true;
          if (method == "error" && !failure) failure = std::string(method) + ": " + safeExcerpt(rt::jsonStringify(params), 200);
          if (!forThread || method != "turn/completed") continue;
          if (compactTurnId && eventTurnId != compactTurnId) continue;
          const auto status = statusOf(message);
          if (status == "completed" && compacted) co_return CompactionOutcome{true, "contextCompaction + turn/completed (" + status + ")"};
          const auto detail = failure.value_or(compacted ? "turn/completed (" + status + ") after the contextCompaction item" : "turn/completed (" + status + ") without a contextCompaction item");
          co_return CompactionOutcome{false, detail};
        }
        rt::Promise<void> delayed(delay(50));
        co_await delayed.wait();
      }
      rt::OrderedSet<std::string> methods;
      for (std::size_t i = std::min(from, rpc->notifications.size()); i < rpc->notifications.size(); ++i) {
        const auto* method = rt::get(rpc->notifications[i], "method");
        methods.add(method && !method->is_null() ? rt::toString(*method) : "");
      }
      std::vector<std::string> names;
      for (const auto& name : methods) names.push_back(name);
      const auto seen = rt::join(names, ", ");
      co_return CompactionOutcome{false, failure.value_or("timed out waiting for the compaction outcome; methods=" + (seen.empty() ? std::string("none") : seen))};
    }
    rt::Task<ThreadHandle> openThread(std::shared_ptr<AppServer> rpc, std::string cwd) {
      const json::object params{{"cwd", cwd}, {"approvalPolicy", "never"}, {"sandbox", "read-only"}};
      rt::Promise<std::optional<json::value>> pending(rpc->request("thread/start", params));
      const auto result = co_await pending.wait();
      const auto id = threadIdOf(result ? &*result : nullptr);
      ThreadHandle thread{id, [rpc, id](std::string text) { return runTurn(rpc, id, std::move(text)); }, [rpc, id] { return compact(rpc, id); }};
      co_return thread;
    }
    std::string configToml(double port, const std::string& workDir, bool autoCompact) {
      const auto contextWindow = autoCompact ? 20'000 : 120'000;
      const auto compactLimit = autoCompact ? 12'000 : 100'000;
      return "# Generated by lib/jev_compaction/jev-codex-smoke.ts: isolated real-client acceptance config.\n"
        "model = \"deepseek-flash\"\nmodel_provider = \"jevtest\"\napproval_policy = \"never\"\nsandbox_mode = \"read-only\"\n"
        "model_context_window = " + rt::numberToString(contextWindow) + "\nmodel_auto_compact_token_limit = " + rt::numberToString(compactLimit) + "\n\n"
        "[model_providers.jevtest]\nname = \"jevtest\"\nbase_url = \"http://127.0.0.1:" + rt::numberToString(port) + "/v1\"\n"
        "wire_api = \"responses\"\nenv_key = \"OPENAI_API_KEY\"\nrequires_openai_auth = false\nstream_max_retries = 0\nrequest_max_retries = 0\n\n"
        "[projects.\"" + workDir + "\"]\ntrust_level = \"trusted\"\n";
    }
    struct GatewayHandle { std::string url; double port; std::function<rt::Task<void>()> stop; };
    rt::Task<void> stopGateway(std::shared_ptr<rt::Kv> store, runtime::http::HttpServer server) {
      kv::setKvForTest(nullptr);
      handler::admission::setInferenceAdmissionControllerForTest(std::nullopt);
      auto stopped = server.shutdown();
      co_await stopped.wait();
      store->close();
    }
    rt::Task<GatewayHandle> startGateway() {
      rt::Promise<std::shared_ptr<rt::Kv>> opened(storage::kv::openKv(":memory:"));
      auto store = co_await opened.wait();
      const std::string keyId = "jev-codex-smoke-key";
      rt::Promise<std::string> hashed(utils::sha256Base64Url(GATEWAY_TOKEN));
      const auto tokenHash = co_await hashed.wait();
      const auto now = rt::dateNow();
      const auto windowMs = 60 * 60'000;
      const json::object commonPolicy{{"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 500}, {"usage_requests", 0},
        {"usage_reset_at_ms", now + windowMs}, {"window_ms", windowMs}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false},
        {"paid_fallback_limit_microcredits", api_keys::PAID_FALLBACK_NO_LIMIT}, {"paid_fallback_spent_microcredits", 0},
        {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}};
      json::object keyRecord{{"id", keyId}, {"name", "jev codex smoke key"}, {"prefix", rt::slice(GATEWAY_TOKEN, 0, 10)}, {"hash", tokenHash}, {"created_at_ms", now}};
      for (const auto& entry : commonPolicy) keyRecord.emplace(entry.key(), entry.value());
      keyRecord.emplace("paid_fallback_model_ids", json::array{});
      keyRecord.emplace("paid_fallback_quota_per_credit", 0);
      keyRecord.emplace("paid_fallback_max_exposure_microcredits", json::object{});
      keyRecord.emplace("paid_fallback_pricing_checked_at_ms", now);
      json::object hashRecord{{"id", keyId}};
      for (const auto& entry : commonPolicy) hashRecord.emplace(entry.key(), entry.value());
      rt::KvKey idKey{"ubq_ai", "api_keys", "id", keyId};
      rt::KvKey hashKey{"ubq_ai", "api_keys", "hash", tokenHash};
      auto setId = store->set(idKey, rt::KvValue(keyRecord));
      co_await std::move(setId);
      co_await rt::microtask();
      auto setHash = store->set(hashKey, rt::KvValue(hashRecord));
      co_await std::move(setHash);
      co_await rt::microtask();
      require_(api_key_policy::apiKeyPolicyFromHashRecord(tokenHash, json::value(hashRecord), now).has_value(), "the seeded key must resolve to a live policy");
      kv::setKvForTest(store);
      api_key_policy::resetApiKeyPolicyCacheForTest();
      handler::admission::setInferenceAdmissionControllerForTest(inference_admission::createInferenceAdmissionController({8, 8, 500}));
      co_await rt::microtask();
      co_await rt::microtask();
      runtime::http::ServeHttpOptions options{.hostname = "127.0.0.1", .port = 0};
      const auto server = runtime::http::serveHttp(options, handler::serve_handler::createServeHandler());
      const auto address = co_await server.ready.wait();
      GatewayHandle result{"http://127.0.0.1:" + rt::numberToString(address.port), address.port, [store, server] { return stopGateway(store, server); }};
      co_return result;
    }
    struct Report {
      std::string status = "FAIL";
      std::string failure_class = "not-started";
      std::string mode;
      bool manual_compaction_ok = false;
      double adopted_summary_chars = 0;
      bool summary_has_stats_trailer = false;
      bool summary_has_goal = false;
      bool summary_kept_gamma = false;
      bool summary_alpha_provenance_kept = false;
      bool summary_dropped_beta_tail = false;
      bool codex_installed_summary = false;
      bool auto_compaction_observed = false;
      bool auto_summary_adopted = false;
      std::optional<bool> failure_path_history_preserved;
      double ordinary_turns = 0;
      double upstream_requests = 0;
      double compaction_leaks = 0;
      double fake_jev_calls = 0;
      double fake_jev_questions = 0;
      double jev_api_requests = 0;
      double jev_api_requests_manual = 0;
      double jev_api_requests_auto = 0;
      double total_ms = 0;
    };
    json::object toJson(const Report& report) {
      return {{"status", report.status}, {"failure_class", report.failure_class}, {"mode", report.mode}, {"manual_compaction_ok", report.manual_compaction_ok},
        {"adopted_summary_chars", report.adopted_summary_chars}, {"summary_has_stats_trailer", report.summary_has_stats_trailer},
        {"summary_has_goal", report.summary_has_goal}, {"summary_kept_gamma", report.summary_kept_gamma},
        {"summary_alpha_provenance_kept", report.summary_alpha_provenance_kept}, {"summary_dropped_beta_tail", report.summary_dropped_beta_tail},
        {"codex_installed_summary", report.codex_installed_summary}, {"auto_compaction_observed", report.auto_compaction_observed},
        {"auto_summary_adopted", report.auto_summary_adopted},
        {"failure_path_history_preserved", report.failure_path_history_preserved ? json::value(*report.failure_path_history_preserved) : json::value(nullptr)},
        {"ordinary_turns", report.ordinary_turns}, {"upstream_requests", report.upstream_requests}, {"compaction_leaks", report.compaction_leaks},
        {"fake_jev_calls", report.fake_jev_calls}, {"fake_jev_questions", report.fake_jev_questions}, {"jev_api_requests", report.jev_api_requests},
        {"jev_api_requests_manual", report.jev_api_requests_manual}, {"jev_api_requests_auto", report.jev_api_requests_auto}, {"total_ms", report.total_ms}};
    }
    rt::Task<void> main(std::shared_ptr<Report> report, double startedAt) {
      require_(BETA_COMMAND.find(BETA_TAIL) == std::string::npos, "the beta command must not embed the dropped tail literal");
      require_(BETA_COMMAND.find(BETA_HEAD) != std::string::npos, "the beta command must still name the call for the Jev decision");
      const auto tmp = rt::fs::promises::mkdtemp(rt::path::join(rt::os::tmpdir(), "jev-codex-smoke-"));
      co_await rt::microtask();
      const auto manualHome = tmp + "/codex-home-manual";
      const auto autoHome = tmp + "/codex-home-auto";
      const auto workDir = tmp + "/work";
      rt::fs::promises::mkdir(manualHome, {.recursive = true});
      co_await rt::microtask();
      rt::fs::promises::mkdir(autoHome, {.recursive = true});
      co_await rt::microtask();
      rt::fs::promises::mkdir(workDir, {.recursive = true});
      co_await rt::microtask();
      const auto fakeJev = std::make_shared<FakeJev>();
      rt::Promise<std::shared_ptr<FakeChatUpstream>> upstreamPending(FakeChatUpstream::start());
      const auto upstream = co_await upstreamPending.wait();
      const auto inheritedDeepSeekKey = rt::env::get("DEEPSEEK_API_KEY");
      rt::env::set("DEEPSEEK_API_KEY", std::string(DUMMY_DEEPSEEK_KEY));
      const auto jevApiRequests = std::make_shared<double>(0);
      const auto originalFetch = rt::globalFetch();
      rt::Fetch fixtureFetch([upstream, jevApiRequests, originalFetch](rt::RequestInfo input, rt::RequestInit init) {
        std::string url;
        if (const auto* string = std::get_if<std::string>(&input)) url = *string;
        else if (const auto* parsed = std::get_if<rt::URL>(&input)) url = parsed->toString();
        else url = std::get<rt::Request>(input).url();
        if (url.starts_with(JEV_API_ORIGIN)) *jevApiRequests += 1;
        if (url == deepseek::index::DEEPSEEK_CHAT_COMPLETIONS_URL) return originalFetch(upstream->url + "/chat/completions", init);
        return originalFetch(input, init);
      });
      rt::setGlobalFetch(fixtureFetch);
      rt::Promise<GatewayHandle> gatewayPending(startGateway());
      const auto gateway = co_await gatewayPending.wait();
      if (report->mode == "fake") ::uos::jev_compaction::compaction::setJevCompactionAskerForTest(fakeJev);
      std::vector<std::pair<std::string, std::string>> childEnv{
        {"CODEX_HOME", manualHome}, {"HOME", manualHome}, {"PATH", rt::env::get("PATH").value_or("/usr/bin:/bin")}, {"TMPDIR", tmp},
        {"TERM", "dumb"}, {"LANG", "C.UTF-8"}, {"NO_COLOR", "1"}, {"RUST_LOG", "error"}, {"NO_PROXY", "127.0.0.1,localhost"}, {"OPENAI_API_KEY", GATEWAY_TOKEN}};
      std::shared_ptr<AppServer> rpc;
      std::shared_ptr<AppServer> autoRpc;
      rt::Reason failure;
      try {
        rt::fs::promises::writeFile(manualHome + "/config.toml", configToml(gateway.port, workDir, false));
        co_await rt::microtask();
        rt::Promise<std::shared_ptr<AppServer>> rpcPending(AppServer::start(workDir, childEnv));
        rpc = co_await rpcPending.wait();
        require_(rpc->serverRequests.empty(), "unexpected server requests during startup: " + rt::join(rpc->serverRequests, ", "));
        rt::console::log("PASS app-server: initialize handshake accepted");
        rt::Promise<ThreadHandle> threadPending(openThread(rpc, workDir));
        const auto thread = co_await threadPending.wait();
        rt::Promise<std::vector<RpcMessage>> chainPending(thread.runTurn(std::string(GOAL_TEXT)));
        const auto chain = co_await chainPending.wait();
        report->ordinary_turns += 1;
        std::vector<std::string> streamed;
        for (const auto& message : chain) if (rt::getString(message, "method") == "item/completed") {
          const auto params = record(rt::get(message, "params"));
          streamed.push_back(rt::jsonStringify(params));
        }
        const auto streamedText = rt::join(streamed, " ");
        require_(streamedText.find("NORMAL_REPLY_MARKER_CHAIN_DONE") != std::string::npos, "normal assistant content did not stream through the gateway unchanged");
        const auto chainStructure = upstream->lastStructure();
        require_(chainStructure.find("assistant_tool_calls=3") != std::string::npos && chainStructure.find("tool_results=3") != std::string::npos,
          "the fixture chain did not execute three tool calls and results: " + chainStructure + " (tools=" + rt::join(upstream->toolNames, ",") + ")");
        require_(fakeJev->calls == 0 || report->mode == "live", "an ordinary turn invoked Jev");
        require_(*jevApiRequests == 0, "an ordinary turn made a real Jev API request");
        std::vector<std::string> firstTools(upstream->toolNames.begin(), upstream->toolNames.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(4, upstream->toolNames.size())));
        const auto advertised = rt::join(firstTools, ", ");
        rt::console::log("PASS ordinary turns: tool-call chain executed (tools advertised: " + (advertised.empty() ? std::string("none") : advertised) + ")");
        rt::Promise<std::vector<RpcMessage>> fillerOne(thread.runTurn(std::string(FILLER_ONE)));
        co_await fillerOne.wait();
        rt::Promise<std::vector<RpcMessage>> fillerTwo(thread.runTurn(std::string(FILLER_TWO)));
        co_await fillerTwo.wait();
        report->ordinary_turns += 2;
        require_(upstream->compactionLeaks().empty(), "a compaction request leaked to the model upstream before compaction ran");
        const auto preCompactionStructure = upstream->lastStructure();
        require_(preCompactionStructure.find("assistant_tool_calls=3") != std::string::npos, "the fixture chain did not execute all three calls before compaction: " + preCompactionStructure);
        require_(preCompactionStructure.find("tool_results=3") != std::string::npos, "the fixture chain did not return all three tool results before compaction: " + preCompactionStructure);
        const auto jevBefore = fakeJev->calls;
        const auto jevApiBeforeManual = *jevApiRequests;
        rt::Promise<CompactionOutcome> outcomePending(thread.compact());
        const auto outcome = co_await outcomePending.wait();
        report->manual_compaction_ok = outcome.ok;
        require_(outcome.ok, "manual compaction did not succeed: " + outcome.detail + " [upstream " + upstream->lastStructure() + "]");
        if (report->mode == "fake") {
          const auto jevCalls = fakeJev->calls - jevBefore;
          require_(jevCalls == 1, "expected exactly one Jev request for the compaction, saw " + rt::numberToString(jevCalls));
          require_(*jevApiRequests == jevApiBeforeManual, "the fake-asker compaction made a real Jev API request");
        } else require_(*jevApiRequests > jevApiBeforeManual, "the live manual compaction made no Jev API request");
        report->jev_api_requests_manual = *jevApiRequests - jevApiBeforeManual;
        const auto jevApiAfterManual = *jevApiRequests;
        require_(upstream->compactionLeaks().empty(), "the manual compaction request was forwarded to the model upstream");
        rt::console::log("PASS manual compaction: terminal reached (" + safeExcerpt(outcome.detail, 60) + "), Jev API requests=" + rt::numberToString(report->jev_api_requests_manual) + ", fake calls=" + rt::numberToString(fakeJev->calls));
        rt::Promise<std::vector<RpcMessage>> afterPending(thread.runTurn(std::string(POST_TURN)));
        const auto after = co_await afterPending.wait();
        report->ordinary_turns += 1;
        const auto postRequests = upstream->bodiesMatching(std::string(POST_TURN));
        require_(!postRequests.empty(), "the post-compaction turn never reached the model upstream");
        const auto& postRequest = postRequests.front();
        require_(postRequest.body.find(codex_items::SUMMARY_MARKER) != std::string::npos, "the next request does not contain the Jev summary marker");
        report->codex_installed_summary = postRequest.body.find("Another language model started to solve this problem") != std::string::npos;
        require_(report->codex_installed_summary, "Codex did not install the summary with its own SUMMARY_PREFIX");
        const auto summary = adoptedSummary(postRequest.body);
        report->adopted_summary_chars = static_cast<double>(rt::length(summary));
        require_(rt::length(summary) > 500, "the adopted summary window is implausibly small (" + rt::numberToString(report->adopted_summary_chars) + " chars)");
        report->summary_has_stats_trailer = summary.find("[fast-jev-compaction stats]") != std::string::npos;
        require_(report->summary_has_stats_trailer, "the adopted summary is missing its structural trailer");
        report->summary_has_goal = summary.find(GOAL_TEXT) != std::string::npos;
        require_(report->summary_has_goal, "the real user goal did not survive compaction");
        report->summary_kept_gamma = summary.find(GAMMA_MARKER) != std::string::npos;
        require_(report->summary_kept_gamma, "the retained Gamma marker was lost from the compacted memory");
        report->summary_alpha_provenance_kept = summary.find(ALPHA_MARKER) != std::string::npos;
        require_(report->summary_alpha_provenance_kept, "the dropped Alpha call/result kept no bounded provenance in the adopted summary");
        report->summary_dropped_beta_tail = summary.find(BETA_TAIL) == std::string::npos;
        require_(report->summary_dropped_beta_tail, "a dropped result tail survived compaction");
        if (report->mode == "fake") {
          require_(summary.find("results_dropped=1 ") != std::string::npos && summary.find("calls_dropped=1 ") != std::string::npos, "the adopted summary does not report the fixture decisions");
          require_(summary.find("drop_result") != std::string::npos, "the kept call was not rendered with its drop_result notice");
          require_(summary.find("fast-jev-compaction truncated") != std::string::npos, "the truncated result notice is missing from the compacted memory");
        }
        require_(!after.empty(), "the post-compaction turn produced no notifications");
        require_(*jevApiRequests == jevApiAfterManual, "an ordinary turn after compaction made a real Jev API request");
        rt::console::log("PASS adoption: next request carries the Jev summary (" + rt::numberToString(report->adopted_summary_chars) + " chars), keeps Gamma, drops Alpha and the Beta tail");
        if (report->mode == "fake") {
          fakeJev->mode = "fail";
          upstream->mode("alpha-only");
          rt::Promise<ThreadHandle> failingPending(openThread(rpc, workDir));
          const auto failing = co_await failingPending.wait();
          const std::vector<std::string> turns{"failure-path fixture history", "failure-path filler one", "failure-path filler two", "failure-path filler three"};
          for (const auto& turn : turns) { rt::Promise<std::vector<RpcMessage>> run(failing.runTurn(turn)); co_await run.wait(); }
          report->ordinary_turns += 4;
          const auto jevApiBeforeFailure = *jevApiRequests;
          rt::Promise<CompactionOutcome> failedPending(failing.compact());
          const auto failed = co_await failedPending.wait();
          require_(!failed.ok, "compaction reported success although Jev was failing");
          require_(*jevApiRequests == jevApiBeforeFailure, "the fake-asker failure path made a real Jev API request");
          require_(upstream->compactionLeaks().empty(), "the failing compaction was forwarded to the model upstream");
          rt::Promise<std::vector<RpcMessage>> survivorPending(failing.runTurn("post-failure probe"));
          const auto survivor = co_await survivorPending.wait();
          const auto failureRequests = upstream->bodiesMatching("post-failure probe");
          require_(!failureRequests.empty(), "the post-failure turn never reached the model upstream");
          const auto& failureRequest = failureRequests.back();
          const auto failureStructure = structureOf(failureRequest.body);
          report->failure_path_history_preserved = failureStructure.find("tool_results=1") != std::string::npos && failureRequest.body.find(codex_items::SUMMARY_MARKER) == std::string::npos;
          require_(report->failure_path_history_preserved.value_or(false), "original tool output was lost, or a summary was installed, after a failed compaction: " + failureStructure);
          require_(!survivor.empty(), "the post-failure turn produced no notifications");
          fakeJev->mode = "decide";
          upstream->mode("chain");
          rt::console::log("PASS failure: compaction failed (" + safeExcerpt(failed.detail, 80) + "), original history still present");
        }
        upstream->mode("chain");
        upstream->reportedPromptTokens = 80;
        auto autoEnv = childEnv;
        for (auto& [key, value] : autoEnv) if (key == "CODEX_HOME" || key == "HOME") value = autoHome;
        rt::fs::promises::writeFile(autoHome + "/config.toml", configToml(gateway.port, workDir, true));
        co_await rt::microtask();
        rt::Promise<std::shared_ptr<AppServer>> autoRpcPending(AppServer::start(workDir, autoEnv));
        autoRpc = co_await autoRpcPending.wait();
        rt::Promise<ThreadHandle> autoThreadPending(openThread(autoRpc, workDir));
        const auto autoThread = co_await autoThreadPending.wait();
        const auto autoJevBefore = fakeJev->calls;
        const auto autoJevQuestionsBefore = fakeJev->questions;
        const auto jevApiBeforeAuto = *jevApiRequests;
        bool autoObserved = false;
        for (int round = 1; round <= 6 && !autoObserved; ++round) {
          const auto from = autoRpc->cursor();
          const auto turnText = "auto threshold turn " + rt::numberToString(round) + ": " + std::string(FILLER_ONE);
          rt::Promise<std::vector<RpcMessage>> run(autoThread.runTurn(turnText));
          co_await run.wait();
          report->ordinary_turns += 1;
          autoObserved = autoRpc->sawContextCompaction(from) || (report->mode == "fake" && fakeJev->calls > autoJevBefore);
          if (!autoObserved && round == 1) { upstream->reportedPromptTokens = 13'000; upstream->reportedPromptTokensOnce = true; }
        }
        report->auto_compaction_observed = autoObserved;
        require_(autoObserved, "the official client never auto-compacted at the isolated small threshold");
        if (report->mode == "fake") {
          require_(fakeJev->questions > autoJevQuestionsBefore, "the auto compaction reached Jev without actionable history");
          require_(*jevApiRequests == jevApiBeforeAuto, "the fake-asker auto compaction made a real Jev API request");
        } else require_(*jevApiRequests > jevApiBeforeAuto, "the live auto compaction made no Jev API request");
        report->jev_api_requests_auto = *jevApiRequests - jevApiBeforeAuto;
        upstream->reportedPromptTokens = 80;
        const auto jevApiAfterAuto = *jevApiRequests;
        rt::Promise<std::vector<RpcMessage>> autoAfterPending(autoThread.runTurn("post-auto-compaction probe"));
        const auto autoAfter = co_await autoAfterPending.wait();
        report->ordinary_turns += 1;
        const auto autoRequests = upstream->bodiesMatching("post-auto-compaction probe");
        require_(!autoRequests.empty(), "the post-auto-compaction turn never reached the model upstream");
        report->auto_summary_adopted = std::any_of(autoRequests.begin(), autoRequests.end(), [](const auto& entry) { return entry.body.find(codex_items::SUMMARY_MARKER) != std::string::npos; });
        require_(report->auto_summary_adopted, "the auto-compaction summary was not adopted by the next request");
        require_(!autoAfter.empty(), "the post-auto-compaction turn produced no notifications");
        require_(*jevApiRequests == jevApiAfterAuto, "an ordinary turn after auto compaction made a real Jev API request");
        rt::console::log("PASS auto compaction: isolated threshold reached, summary adopted, Jev API requests=" + rt::numberToString(report->jev_api_requests_auto));
        report->compaction_leaks = static_cast<double>(upstream->compactionLeaks().size());
        require_(report->compaction_leaks == 0, "a compaction request leaked to the model upstream");
        report->upstream_requests = static_cast<double>(upstream->requests.size());
        report->fake_jev_calls = fakeJev->calls;
        report->fake_jev_questions = fakeJev->questions;
        report->jev_api_requests = *jevApiRequests;
        report->status = "PASS";
        report->failure_class = "none";
      } catch (...) { failure = std::current_exception(); }
      report->total_ms = rt::Math::round(rt::dateNow() - startedAt);
      if (autoRpc) { rt::Promise<void> stopped(autoRpc->stop()); co_await stopped.wait(); } else co_await rt::microtask();
      if (rpc) { rt::Promise<void> stopped(rpc->stop()); co_await stopped.wait(); } else co_await rt::microtask();
      ::uos::jev_compaction::compaction::setJevCompactionAskerForTest(nullptr);
      rt::setGlobalFetch(originalFetch);
      if (inheritedDeepSeekKey) rt::env::set("DEEPSEEK_API_KEY", *inheritedDeepSeekKey);
      else rt::env::remove("DEEPSEEK_API_KEY");
      rt::Promise<void> gatewayStopped(gateway.stop());
      co_await gatewayStopped.wait();
      rt::Promise<void> upstreamStopped(upstream->close());
      co_await upstreamStopped.wait();
      try { rt::fs::promises::rm(tmp, {.recursive = true}); co_await rt::microtask(); } catch (...) {}
      if (failure) std::rethrow_exception(failure);
    }
  }

  rt::Task<void> importMetaMain() {
    const auto startedAt = rt::dateNow();
    auto report = std::make_shared<Report>();
    report->mode = openRouterKeyPresent() ? "live" : "fake";
    require_(remaining(startedAt) > 20'000, "the smoke test ran out of its budget before starting");
    try {
      rt::Promise<void> pending(main(report, startedAt));
      co_await pending.wait();
    } catch (...) {
      const auto error = std::current_exception();
      report->status = "FAIL";
      report->failure_class = rt::isError(error) ? rt::errorName(error).value_or("Error") + ": " + rt::slice(rt::errorMessage(error).value_or(""), 0, 240) : "unknown";
    }
    rt::console::log(rt::jsonStringify(toJson(*report)));
    rt::process::exit(report->status == "PASS" ? 0 : 1);
  }
}
