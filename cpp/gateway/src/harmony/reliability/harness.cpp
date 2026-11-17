#include "uos/harmony/reliability/harness.hpp"

#include "uos/harmony/reliability/feedback.hpp"
#include "uos/harmony/reliability/loops.hpp"
#include "uos/harmony/tools/router.hpp"
#include "uos/harmony/tools/schemas.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <utility>

namespace uos::harmony::reliability::harness {
  namespace {
    using adapter::BuiltHarmonyRequest;
    using conversation::Conversation;
    using tools::result::ToolResult;
    using types::NormalizedAssistantResponse;
    using types::ToolCall;

    bool isTransientHttpStatus(double status) { return status == 408 || status == 425 || status == 429 || status >= 500; }

    double tailTurnsForBudget(context::ContextBudgetKind budget) {
      switch (budget) {
        case context::ContextBudgetKind::short_: return 2;
        case context::ContextBudgetKind::medium: return 4;
        case context::ContextBudgetKind::large: return 8;
      }
      throw std::logic_error("unreachable context budget");
    }

    /** Mutable state of one harness run: resolved options, counters, transcript. */
    struct HarnessRun {
      std::vector<types::ToolDefinition> tools;
      retry::RetryPolicy retryPolicy;
      verify::VerificationPolicy verificationPolicy;
      context::ContextBudgetKind budget;
      std::string mode;
      double maxTurns;
      double maxToolCalls;
      double invalidCallStreakLimit;
      double loopThreshold;
      double maxGuardRejections;
      double maxCompletionTokens;
      loops::LoopDetector detector{};
      verify::VerificationTracker tracker;
      retry::RetryLedger retryLedger;
      std::vector<HarnessEvent> events{};
      std::vector<state::FinalObservation> finals{};
      std::vector<verify::FinalAttempt> finalAttemptLog{};
      Conversation conversation;
      state::StructuredTaskState state;
      double seq = 0;
      double modelCalls = 0;
      double requestCounter = 0;
      double invalidStreak = 0;
      double guardRejections = 0;
      double finalAttempts = 0;
      double emittedToolCalls = 0;
      bool loopGuardEmitted = false;
    };

    using HarnessAbortReason = std::string;
    struct ModelTransportAttempt {
      std::string kind;
      std::optional<NormalizedAssistantResponse> normalized{};
    };

    /** Resolves every option default and seeds the transcript and counters. */
    HarnessRun createHarnessRun(const HarnessOptions& opts) {
      auto verificationPolicy = opts.verificationPolicy.value_or(verify::DEFAULT_VERIFICATION_POLICY);
      if (!verificationPolicy.verificationCommand) verificationPolicy.verificationCommand = opts.verificationCommand;
      auto conversation = conversation::createConversation();
      if (!opts.systemPrompt.empty()) conversation = conversation::appendTurn(conversation, types::ConversationTurnSystem{opts.systemPrompt});
      conversation = conversation::appendUser(conversation, opts.userPrompt);
      const auto retryPolicy = opts.retryPolicy.value_or(retry::DEFAULT_RETRY_POLICY);
      return {
        .tools = opts.tools.value_or(tools::schemas::toolDefinitions()),
        .retryPolicy = retryPolicy,
        .verificationPolicy = verificationPolicy,
        .budget = opts.transcriptBudget.value_or(context::ContextBudgetKind::medium),
        .mode = opts.contextMode.value_or("structured"),
        .maxTurns = opts.maxTurns.value_or(40),
        .maxToolCalls = opts.maxToolCalls.value_or(60),
        .invalidCallStreakLimit = opts.invalidCallStreakLimit.value_or(4),
        .loopThreshold = opts.loopThreshold.value_or(3),
        .maxGuardRejections = opts.maxGuardRejections.value_or(6),
        .maxCompletionTokens = opts.maxCompletionTokens.value_or(512),
        .tracker = verify::VerificationTracker(verificationPolicy),
        .retryLedger = retry::RetryLedger(retryPolicy),
        .conversation = std::move(conversation),
        .state = state::emptyTaskState(),
      };
    }

    /** Records one event in the run and forwards it to the caller's sink. */
    void emitHarnessEvent(HarnessRun& run, const HarnessOptions& opts, const HarnessEvent& event) {
      run.events.push_back(event);
      if (opts.emit) opts.emit(event);
    }

    /** Deterministic backoff sleep (a zero delay resolves without a timer). */
    rt::Task<void> sleepMs(double ms) {
      if (ms > 0) return rt::sleep(ms);
      return rt::Promise<void>::resolve().wait();
    }

    // The sentinel uses the rejection channel only inside this private race, so both
    // inputs retain Promise.race's turns without a value-mapping reaction.
    struct RunCancelled : std::exception {};

    /** The race observes abandoned provider promises, including late rejections. */
    template <class T> rt::Task<std::optional<T>> awaitWithRunCancellation(rt::Promise<T> promise, std::optional<rt::AbortSignal> signal) {
      if (!signal) {
        auto value = co_await promise.wait();
        co_return std::optional<T>(std::move(value));
      }
      if (signal->aborted()) co_return std::nullopt;
      auto cancellation = rt::withResolvers<T>();
      rt::EventListener onAbort([reject = cancellation.reject] { reject(rt::makeReason(RunCancelled{})); });
      signal->addEventListener("abort", onAbort, {.once = true});
      auto cleanup = rt::scopeExit([signal, onAbort] { signal->removeEventListener("abort", onAbort); });
      auto race = rt::race(promise, cancellation.promise);
      std::optional<T> value;
      try {
        value = co_await race.wait();
      } catch (const RunCancelled&) {
        // The whole-run signal won; the provider promise remains observed.
      }
      co_return value;
    }

    failure::ReliabilityClassification classifyRun(const HarnessRun& run, const std::optional<std::string>& abortedReason) {
      return failure::classifyReliability({
        .state = run.state,
        .invalidCallStreak = run.invalidStreak,
        .loopStreak = run.state.semanticLoopStreak,
        .guardRejections = run.guardRejections,
        .finalAccepted = std::any_of(run.finals.begin(), run.finals.end(), [](const auto& final) { return final.accepted; }),
        .abortedReason = rt::orNull(abortedReason),
      });
    }

    HarnessOutcome abortRun(const HarnessRun& run, const HarnessAbortReason& reason) {
      return {
        .phase = reason == "signal" ? "aborted" : "failed",
        .finalContent = std::nullopt,
        .conversation = run.conversation,
        .state = run.state,
        .classification = classifyRun(run, reason),
        .events = run.events,
        .modelCalls = run.modelCalls,
        .abortedReason = reason,
      };
    }

    HarnessOutcome completeRun(const HarnessRun& run, const std::string& content) {
      return {
        .phase = "completed",
        .finalContent = content,
        .conversation = run.conversation,
        .state = run.state,
        .classification = classifyRun(run, std::nullopt),
        .events = run.events,
        .modelCalls = run.modelCalls,
        .abortedReason = std::nullopt,
      };
    }

    /** The model-facing conversation of the next request (full or structured). */
    Conversation requestConversationFor(const HarnessRun& run, const HarnessOptions& opts) {
      if (run.mode == "full") return context::compactTranscript(run.conversation, {.budget = run.budget}).conversation;
      const auto text = context::renderStructuredContext(run.state, run.conversation, {.maxTailTurns = tailTurnsForBudget(run.budget)});
      std::vector<types::ConversationTurn> head;
      for (const auto& turn : run.conversation.turns) {
        if (std::holds_alternative<types::ConversationTurnSystem>(turn) || std::holds_alternative<types::ConversationTurnDeveloper>(turn)) head.push_back(turn);
      }
      head.push_back(types::ConversationTurnUser{rt::concat(rt::concat(opts.userPrompt, "\n\n"), text)});
      return conversation::createConversation(std::move(head));
    }

    types::AssistantTurn assistantTurn(const ToolCall& call, const std::vector<std::string>& analysis);

    /** Appends the assistant tool-call turn and its recorded tool result. */
    void appendToolPair(HarnessRun& run, const ToolCall& call, const ToolResult& result, const std::vector<std::string>& analysis) {
      run.conversation = conversation::appendTurn(run.conversation, assistantTurn(call, analysis));
      const context::ParsedToolResult parsed{
        .ok = result.ok,
        .output = rt::orUndefined(result.output),
        .error = rt::orUndefined(result.error),
        .error_code = rt::orUndefined(result.error_code),
      };
      run.conversation = conversation::appendTurn(run.conversation, types::ToolResultTurn{
        .toolCallId = call.id,
        .name = call.name,
        .content = context::serializeToolResultContent(parsed),
      });
    }

    std::optional<BuiltHarmonyRequest> buildHarnessRequest(const HarnessRun& run, const HarnessOptions& opts) {
      try {
        return adapter::buildCerebrasHarmonyRequest({
          .style = "generic",
          .turns = requestConversationFor(run, opts).turns,
          .reasoningEffort = opts.reasoningEffort.value_or("low"),
          .tools = run.tools,
          .maxCompletionTokens = run.maxCompletionTokens,
        });
      } catch (...) {
        return std::nullopt;
      }
    }

    rt::Task<void> cancelBody(rt::Response response) {
      if (auto body = response.body()) {
        auto cancellation = body->cancelPromise();
        auto observed = cancellation.catch_([](rt::Reason) {});
        co_await observed.wait();
      } else {
        co_await rt::microtask();
      }
    }

    rt::Promise<json::value> jsonOrNull(rt::Response response) {
      rt::Promise<json::value> pending(response.json());
      return pending.catch_([](rt::Reason) { return json::value(nullptr); });
    }

    /** One transport attempt, classified for the bounded retry loop. */
    rt::Task<ModelTransportAttempt> attemptModelTransport(HarnessRun& run, const HarnessOptions& opts, const BuiltHarmonyRequest& built) {
      run.requestCounter += 1;
      const double requestId = run.requestCounter;
      emitHarnessEvent(run, opts, HarnessEventModelRequest{requestId, run.mode, built, context::estimateRequestTokens(built.body)});
      std::optional<rt::Response> response;
      bool failed = false;
      try {
        adapter::HarmonyTransportRequestOptions requestOptions{.signal = opts.signal};
        auto dispatch = opts.transport(built.body, requestOptions);
        rt::Promise<rt::Response> dispatched(std::move(dispatch));
        auto cancellation = awaitWithRunCancellation(dispatched, opts.signal);
        rt::Promise<std::optional<rt::Response>> waiting(std::move(cancellation));
        response = co_await waiting.wait();
      } catch (...) {
        failed = true;
      }
      if (failed) {
        if (opts.signal && opts.signal->aborted()) co_return ModelTransportAttempt{.kind = "aborted"};
        co_await sleepMs(run.retryPolicy.backoffMs);
        co_return ModelTransportAttempt{.kind = "retry"};
      }
      if (!response) co_return ModelTransportAttempt{.kind = "aborted"};
      if (opts.signal && opts.signal->aborted()) {
        co_await cancelBody(*response);
        co_return ModelTransportAttempt{.kind = "aborted"};
      }
      if (!response->ok()) {
        co_await cancelBody(*response);
        if (!isTransientHttpStatus(response->status())) co_return ModelTransportAttempt{.kind = "stop"};
        co_await sleepMs(run.retryPolicy.backoffMs);
        co_return ModelTransportAttempt{.kind = "retry"};
      }
      auto bodyPending = jsonOrNull(*response);
      auto cancellation = awaitWithRunCancellation(bodyPending, opts.signal);
      rt::Promise<std::optional<json::value>> waiting(std::move(cancellation));
      auto body = co_await waiting.wait();
      if (!body) co_return ModelTransportAttempt{.kind = "aborted"};
      if (body->is_null()) {
        co_await sleepMs(run.retryPolicy.backoffMs);
        co_return ModelTransportAttempt{.kind = "retry"};
      }
      auto result = adapter::normalizeHarmonyChatCompletion(*body);
      if (std::holds_alternative<adapter::HarmonyNormalizationError>(result)) {
        co_await sleepMs(run.retryPolicy.backoffMs);
        co_return ModelTransportAttempt{.kind = "retry"};
      }
      auto normalized = std::get<NormalizedAssistantResponse>(std::move(result));
      double chars = static_cast<double>(rt::length(normalized.content.value_or("")));
      for (const auto& call : normalized.toolCalls) chars += static_cast<double>(rt::length(call.arguments));
      emitHarnessEvent(run, opts, HarnessEventModelResponse{requestId, normalized, rt::Math::max(1.0, rt::Math::ceil(chars / 4))});
      co_return ModelTransportAttempt{.kind = "response", .normalized = std::move(normalized)};
    }

    /** Transport with deterministic retry (transient failures only). */
    rt::Task<std::optional<NormalizedAssistantResponse>> requestModelResponse(HarnessRun& run, const HarnessOptions& opts, const BuiltHarmonyRequest& built) {
      for (double attempt = 0; attempt <= run.retryPolicy.maxRetriesPerCall; attempt++) {
        if (opts.signal && opts.signal->aborted()) co_return std::nullopt;
        rt::Promise<ModelTransportAttempt> pending(attemptModelTransport(run, opts, built));
        auto outcome = co_await pending.wait();
        if (outcome.kind == "aborted") co_return std::nullopt;
        if (outcome.kind == "response") co_return outcome.normalized;
        if (outcome.kind == "stop") break;
      }
      co_return std::nullopt;
    }

    ToolResult blockedDuplicateResult(const ToolCall& call, const json::object& args, const loops::DuplicateFlag& duplicate,
                                      const std::optional<std::string>& previousCode, const std::string& identity) {
      const std::string blockedCode = duplicate == "repeat_after_success" ? "duplicate_call" : "repeated_failure";
      const std::string message = blockedCode == "duplicate_call"
        ? rt::concat("duplicate of the previous call " + call.name + "(" + rt::jsonStringify(args), "); do not repeat it — read the existing result or take a different action")
        : retry::renderRepeatedFailureFeedback(previousCode, identity);
      return {.ok = false, .error = message, .error_code = blockedCode};
    }

    ToolResult recordBlockedDuplicate(HarnessRun& run, const HarnessOptions& opts, const std::string& id, const ToolCall& call,
                                      const json::object& args, const std::vector<std::string>& analysis, const loops::DuplicateFlag& duplicate,
                                      const std::optional<std::string>& previousCode, const std::string& identity) {
      // Deterministic guard: never re-execute an identical call.
      const auto result = blockedDuplicateResult(call, args, duplicate, previousCode, identity);
      emitHarnessEvent(run, opts, HarnessEventToolCall{.id = id, .tool = call.name, .arguments = args, .valid = true, .repeated = duplicate});
      emitHarnessEvent(run, opts, HarnessEventToolResult{.id = id, .result = result});
      appendToolPair(run, call, result, analysis);
      return result;
    }

    /** Executes one validated call through the canonical tool router. */
    rt::Task<ToolResult> executeToolCall(HarnessRun& run, const HarnessOptions& opts, const std::string& id, const ToolCall& call,
                                        const json::object& args, const std::vector<std::string>& analysis,
                                        const std::optional<loops::DuplicateFlag>& duplicate, const std::optional<retry::RetryDecision>& retryDecision) {
      if (retryDecision && retryDecision->retry && retryDecision->delayMs > 0) co_await sleepMs(retryDecision->delayMs);
      emitHarnessEvent(run, opts, HarnessEventToolCall{.id = id, .tool = call.name, .arguments = args, .valid = true, .repeated = rt::orNull(duplicate)});
      const double started = rt::dateNow();
      ToolResult result;
      try {
        tools::router::RunToolOptions runOptions{.signal = opts.signal};
        auto task = tools::router::runTool(opts.backends, call.name, json::value(args), runOptions);
        rt::Promise<ToolResult> pending(std::move(task));
        result = co_await pending.wait();
      } catch (...) {
        const auto error = std::current_exception();
        result = tools::result::toolFailure("internal", rt::isError(error) ? rt::errorMessage(error).value_or("") : rt::reasonToString(error));
      }
      emitHarnessEvent(run, opts, HarnessEventToolResult{.id = id, .result = result, .durationMs = rt::dateNow() - started});
      appendToolPair(run, call, result, analysis);
      co_return result;
    }

    loops::ResultLike resultLike(const ToolResult& result) {
      return {.ok = result.ok, .error_code = rt::orUndefined(result.error_code), .error = rt::orUndefined(result.error), .output = rt::orUndefined(result.output)};
    }

    std::optional<HarnessOutcome> recordInvalidToolCall(HarnessRun& run, const HarnessOptions& opts, const std::string& id,
                                                       const ToolCall& call, const feedback::DetailedValidationResult& validation,
                                                       const std::vector<std::string>& analysis) {
      // Deterministic feedback; the call is never executed.
      const auto result = tools::result::toolFailure("invalid_args", feedback::renderValidationFeedback(call.name, validation));
      const auto view = resultLike(result);
      const auto flags = run.detector.observe(call.name, validation.arguments, view);
      emitHarnessEvent(run, opts, HarnessEventToolCall{
        .id = id, .tool = call.name, .arguments = validation.arguments, .valid = false, .invalidReason = feedback::invalidCallLabel(validation),
      });
      emitHarnessEvent(run, opts, HarnessEventToolResult{.id = id, .result = result});
      appendToolPair(run, call, result, analysis);
      run.invalidStreak += 1;
      if (run.invalidStreak >= run.invalidCallStreakLimit) return abortRun(run, "invalid_argument_loop");
      run.state = state::reduceToolObservation(run.state,
        {.seq = run.seq, .tool = call.name, .args = validation.arguments, .valid = false, .result = view},
        {.duplicate = std::nullopt, .semanticLoop = flags.semanticLoop, .verification = std::nullopt});
      return std::nullopt;
    }

    json::value parseArguments(const std::string& argumentsText);

    /** Processes one model tool call; a non-null result terminates the run. */
    rt::Task<std::optional<HarnessOutcome>> processToolCall(HarnessRun& run, const HarnessOptions& opts, const ToolCall& call, const std::vector<std::string>& analysis) {
      const std::string id = "t" + rt::numberToString(run.seq);
      const auto validation = feedback::validateToolArgumentsDetailed(call.name, parseArguments(call.arguments));
      if (!validation.valid) co_return recordInvalidToolCall(run, opts, id, call, validation, analysis);
      const auto identity = loops::callIdentity(call.name, validation.arguments);
      const auto priorAttempts = run.retryLedger.priorAttempts(identity);
      const auto entry = run.retryLedger.entry(identity);
      const auto previousCode = entry ? entry->lastCode : std::nullopt;
      const auto duplicate = run.detector.checkDuplicate(call.name, validation.arguments);
      const auto retryDecision = duplicate && priorAttempts > 0 ? std::optional<retry::RetryDecision>(retry::decideRetry(run.retryPolicy, previousCode, priorAttempts)) : std::nullopt;
      ToolResult result;
      if (duplicate && !(retryDecision && retryDecision->retry)) {
        result = recordBlockedDuplicate(run, opts, id, call, validation.arguments, analysis, *duplicate, previousCode, identity);
      } else {
        rt::Promise<ToolResult> pending(executeToolCall(run, opts, id, call, validation.arguments, analysis, duplicate, retryDecision));
        result = co_await pending.wait();
      }
      run.retryLedger.observe(identity, result, priorAttempts);
      const auto view = resultLike(result);
      const auto verification = run.tracker.observe(call.name, validation.arguments, view);
      const auto flags = run.detector.observe(call.name, validation.arguments, view);
      run.state = state::reduceToolObservation(run.state,
        {.seq = run.seq, .tool = call.name, .args = validation.arguments, .valid = true, .result = view},
        {.duplicate = flags.duplicate ? flags.duplicate : duplicate, .semanticLoop = flags.semanticLoop, .verification = verification});
      run.invalidStreak = 0;
      if (flags.semanticLoop && flags.streak >= run.loopThreshold && !run.loopGuardEmitted) {
        run.loopGuardEmitted = true;
        emitHarnessEvent(run, opts, HarnessEventGuard{"loop", loops::renderLoopFeedback(flags), 0, run.state.phase});
      }
      if (!flags.semanticLoop) run.loopGuardEmitted = false;
      co_return std::nullopt;
    }

    /** Runs every tool call of one model response (sequentially, in order). */
    rt::Task<std::optional<HarnessOutcome>> runToolCalls(HarnessRun& run, const HarnessOptions& opts, const NormalizedAssistantResponse& normalized) {
      // Tool calls (parallel calls are processed sequentially in order).
      for (const auto& call : normalized.toolCalls) {
        if (run.emittedToolCalls >= run.maxToolCalls) co_return abortRun(run, "tool_call_limit");
        run.emittedToolCalls += 1;
        run.seq += 1;
        rt::Promise<std::optional<HarnessOutcome>> pending(processToolCall(run, opts, call, normalized.analysis));
        auto outcome = co_await pending.wait();
        if (outcome) co_return outcome;
      }
      co_return std::nullopt;
    }

    /** Handles a final answer attempt: guard decision, transcript and outcome. */
    std::optional<HarnessOutcome> runFinalAttempt(HarnessRun& run, const HarnessOptions& opts, const NormalizedAssistantResponse& normalized) {
      const std::string content = normalized.content.value_or("");
      if (rt::trim(content).empty()) return abortRun(run, "no_model_output");
      run.finalAttempts += 1;
      const auto decision = verify::guardFinal({
        .finalContent = content,
        .lastActionSeq = run.state.lastActionSeq,
        .previousFinals = run.finalAttemptLog,
        .semanticLoopStreak = run.state.semanticLoopStreak,
        .planUpdated = run.state.plan.seq.has_value(),
        .writes = static_cast<double>(run.state.writes.size()),
        .tracker = run.tracker,
        .policy = run.verificationPolicy,
      });
      run.finalAttemptLog.push_back(decision.attempt);
      const state::FinalObservation final{content, decision.allowed, run.seq + 1};
      run.finals.push_back(final);
      run.conversation = conversation::appendTurn(run.conversation, types::AssistantTurn{
        .content = content, .analysis = normalized.analysis, .toolCalls = {}, .finishReason = normalized.finishReason,
      });
      run.state = state::reduceFinalAttempt(run.state, final);
      if (decision.allowed) {
        emitHarnessEvent(run, opts, HarnessEventFinal{content, true, run.finalAttempts});
        return completeRun(run, content);
      }
      emitHarnessEvent(run, opts, HarnessEventFinal{content, false, run.finalAttempts});
      run.guardRejections += 1;
      // A rejected final always carries at least one blocking requirement.
      const auto& first = decision.requirements.front();
      emitHarnessEvent(run, opts, HarnessEventGuard{
        decision.falseCompletion ? "false_completion" : first.kind,
        verify::renderGuardRequirements(decision.requirements), run.finalAttempts, run.state.phase,
      });
      run.conversation = conversation::appendUser(run.conversation, std::string(verify::GUARD_PREFIX) + ": " + verify::renderGuardRequirements(decision.requirements));
      if (decision.falseCompletion && decision.attempt.repetitions >= run.verificationPolicy.maxRepeatedFinals) return abortRun(run, "false_completion");
      if (run.finalAttempts >= run.verificationPolicy.maxFinalAttempts) return abortRun(run, "guard_exhausted");
      if (run.guardRejections >= run.maxGuardRejections) return abortRun(run, "guard_exhausted");
      return std::nullopt;
    }

    /** Pre-turn guards: the abort signal, then the hard tool-call budget. */
    std::optional<HarnessAbortReason> turnAbortReason(const HarnessRun& run, const HarnessOptions& opts) {
      if (opts.signal && opts.signal->aborted()) return "signal";
      if (run.emittedToolCalls >= run.maxToolCalls) return "tool_call_limit";
      return std::nullopt;
    }

    /** One full model turn: request, transport, then final answer or tool calls. */
    rt::Task<std::optional<HarnessOutcome>> runHarnessTurn(HarnessRun& run, const HarnessOptions& opts) {
      const auto abortReason = turnAbortReason(run, opts);
      if (abortReason) co_return abortRun(run, *abortReason);
      const auto built = buildHarnessRequest(run, opts);
      if (!built) co_return abortRun(run, "invalid_config");
      rt::Promise<std::optional<NormalizedAssistantResponse>> pending(requestModelResponse(run, opts, *built));
      const auto normalized = co_await pending.wait();
      if (!normalized) co_return abortRun(run, opts.signal && opts.signal->aborted() ? "signal" : "transport_failed");
      run.modelCalls += 1;
      run.state.modelCalls = run.modelCalls;
      if (normalized->toolCalls.empty()) co_return runFinalAttempt(run, opts, *normalized);
      rt::Promise<std::optional<HarnessOutcome>> toolCalls(runToolCalls(run, opts, *normalized));
      auto outcome = co_await toolCalls.wait();
      co_return outcome;
    }

    json::value parseArguments(const std::string& argumentsText) {
      try {
        auto value = rt::jsonParse(argumentsText);
        if (value.is_object()) return value;
      } catch (...) {
        // fall through to the invalid-arguments path
      }
      return nullptr;
    }

    types::AssistantTurn assistantTurn(const ToolCall& call, const std::vector<std::string>& analysis) {
      return {.content = std::nullopt, .analysis = analysis, .toolCalls = {call}, .finishReason = "tool_calls"};
    }
  }

  std::string renderCanonicalPolicy(const CanonicalPolicyOptions& opts) {
    const std::vector<std::string> lines{
      "You are a deterministic agent running inside the canonical reliability harness.",
      "Rules:",
      "- Validate the exact tool arguments before calling a tool. Invalid calls are not executed and you must correct them.",
      "- Never repeat a tool call with identical arguments: repeats are detected and blocked. After a failure, change the arguments or the approach.",
      "- After every edit, verify it (read the file back or run a check command) before answering.",
      "- Never claim completion while a verification is pending or a command/editing failure is unresolved.",
      "- Available tools: " + rt::join(opts.tools, ", ") + ".",
      "- Context budget tier: " + std::string(context::toString(opts.budget)) + ".",
      "- Answer only when everything is verified.",
    };
    return rt::join(lines, "\n");
  }

  rt::Task<HarnessOutcome> runReliabilityHarness(HarnessOptions opts) {
    auto run = createHarnessRun(opts);
    for (double turn = 0; turn < run.maxTurns; turn++) {
      rt::Promise<std::optional<HarnessOutcome>> pending(runHarnessTurn(run, opts));
      auto outcome = co_await pending.wait();
      if (outcome) co_return *outcome;
    }
    co_return abortRun(run, "turn_limit");
  }
}
