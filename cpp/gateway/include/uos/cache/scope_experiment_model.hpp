#pragma once
// Port of src/cache/scope-experiment-model.ts.
#include "uos/models/codex_models.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::cache::scope_experiment_model {
  inline constexpr auto PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER = models::codex_models::CODEX_CHATGPT_PROMPT_CACHE_PROVIDER;
  inline constexpr std::string_view CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER = "chatgpt_codex";
  // v5 is the durable experiment-definition fence; every key includes the fixed profile and exact target.
  inline const rt::KvKey PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX{"uos_ai", "prompt_cache_scope_experiment", "v5"};
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES = 3;
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE = 10;
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS = 120'000;
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_SESSION_MS = 15 * 60'000;
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLE_DEADLINE_MS = 100'000;
  inline constexpr double PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLE_DEADLINE_MS = 7'000;
  inline constexpr std::array<std::string_view, 10> CACHE_SCOPE_STEP_NAMES{
    "slot_1_warm", "slot_1_repeat", "slot_2_first", "slot_2_repeat", "slot_1_after_slot_2", "slot_1_after_refresh",
    "slot_1_post_refresh_repeat", "slot_1_conversation_changed", "slot_1_conversation_changed_repeat", "slot_1_original_conversation_recheck"};
  inline constexpr std::array<double, 10> CACHE_SCOPE_EXPECTED_SLOTS{1, 1, 2, 2, 1, 1, 1, 1, 1, 1};
  inline const rt::OrderedSet<double> CACHE_SCOPE_DISCRIMINATOR_INDEXES{2, 5, 7};
  using CacheScopeStepName = std::string;
  using CacheSignal = std::string;
  inline const std::array<std::optional<CacheSignal>, 10> CACHE_SCOPE_EXPECTED_SIGNALS{
    "write", "read", std::nullopt, "read", "read", std::nullopt, "read", std::nullopt, "read", "read"};
  using InconclusiveReason = std::string;

  struct PromptCacheScopeExperimentTelemetryBaselineTestOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::optional<std::string> release{};
  };
  struct ConcreteScopeObservation {
    std::string probe_profile;
    std::string account_slots;
    std::string token_refresh;
    std::string conversation_id;
    std::string effective_model;
    friend bool operator==(const ConcreteScopeObservation&, const ConcreteScopeObservation&) = default;
  };
  struct NormalizedUsage {
    double input_tokens;
    double cached_tokens;
    double cache_write_tokens;
    double output_tokens;
    double total_tokens;
    friend bool operator==(const NormalizedUsage&, const NormalizedUsage&) = default;
  };
  struct PromptCacheScopeSample {
    CacheScopeStepName step;
    double slot;
    // Only the provider's five-counter tuple is retained, keeping durable evidence redacted.
    NormalizedUsage raw_usage;
    NormalizedUsage usage;
    double elapsed_ms;
    friend bool operator==(const PromptCacheScopeSample&, const PromptCacheScopeSample&) = default;
  };
  struct CycleEvidence {
    double cycle;
    std::vector<PromptCacheScopeSample> samples;
    std::optional<ConcreteScopeObservation> classification{};
    std::optional<InconclusiveReason> inconclusive_reason{};
    friend bool operator==(const CycleEvidence&, const CycleEvidence&) = default;
  };
  struct PromptCacheScopeTargetBinding {
    std::string id;
    std::string provider;
    std::string telemetry_provider;
    std::string topology_kind;
    std::string model;
    std::string probe_profile;
    std::string capability_fingerprint;
    std::string inventory_fingerprint;
    std::string catalog_versionstamp;
    std::string runtime_versionstamp;
    std::string auth_pool_versionstamp;
    std::string auth_pool_identity_fingerprint;
    std::optional<std::string> catalog_client_version;
    friend bool operator==(const PromptCacheScopeTargetBinding&, const PromptCacheScopeTargetBinding&) = default;
  };
  struct PromptCacheScopeExperimentTelemetryBaseline { PromptCacheScopeTargetBinding target; };
  struct StoredEvidence {
    double v;
    PromptCacheScopeTargetBinding target;
    std::string outcome;
    double started_at_ms;
    double verified_at_ms;
    std::vector<CycleEvidence> cycles;
    std::optional<InconclusiveReason> inconclusive_reason{};
    friend bool operator==(const StoredEvidence&, const StoredEvidence&) = default;
  };
  struct ExperimentState {
    double v;
    PromptCacheScopeTargetBinding target;
    std::string campaign_owner;
    double started_at_ms;
    double expires_at_ms;
    std::string auth_pool_versionstamp;
    double next_cycle;
    std::vector<ConcreteScopeObservation> classifications;
    std::optional<ConcreteScopeObservation> pending_scope{};
    friend bool operator==(const ExperimentState&, const ExperimentState&) = default;
  };
  struct ExperimentLease { std::string owner; double lease_until_ms; };
  struct CampaignLease { std::string owner; std::string target_id; std::string inventory_fingerprint; double lease_until_ms; };
  struct PromptCacheScopeExperimentResult {
    std::string provider;
    std::string telemetry_provider;
    std::string target_id;
    std::string model;
    std::string status;
    double completed_cycles;
    double verified_at_ms;
    std::optional<ConcreteScopeObservation> scope{};
    std::optional<InconclusiveReason> inconclusive_reason{};
  };
  class PromptCacheScopeExperimentBusyError : public rt::JsError {
  public:
    PromptCacheScopeExperimentBusyError()
      : rt::JsError("A prompt-cache scope experiment campaign is already running for the active provider.", "PromptCacheScopeExperimentBusyError") {}
  };
  class PromptCacheScopeExperimentUnavailableError : public rt::JsError {
  public:
    explicit PromptCacheScopeExperimentUnavailableError(std::string message_)
      : rt::JsError(std::move(message_), "PromptCacheScopeExperimentUnavailableError") {}
  };
  class PromptCacheScopeExperimentFailedError : public rt::JsError {
  public:
    explicit PromptCacheScopeExperimentFailedError(std::string message_)
      : rt::JsError(std::move(message_), "PromptCacheScopeExperimentFailedError") {}
  };
  struct ReadSampleResultSample {
    static constexpr std::string_view status = "sample";
    struct Sample { double slot; NormalizedUsage raw_usage; NormalizedUsage usage; double elapsed_ms; } sample;
    CacheSignal signal;
  };
  struct ReadSampleResultInconclusive {
    static constexpr std::string_view status = "inconclusive";
    InconclusiveReason reason;
  };
  using ReadSampleResult = std::variant<ReadSampleResultSample, ReadSampleResultInconclusive>;
  struct RuntimeBinding { std::string versionstamp; std::string default_model; };

  bool isSafeNonNegativeInteger(const json::value* value);
  bool isSafeNonNegativeInteger(const json::value& value);
  bool isSafeNonNegativeInteger(double value);
  rt::KvKey stateKey(const PromptCacheScopeTargetBinding& target);
  rt::KvKey evidenceKey(const PromptCacheScopeTargetBinding& target);
  rt::KvKey cycleLeaseKey(const PromptCacheScopeTargetBinding& target);
  rt::KvKey campaignLeaseKey(const PromptCacheScopeTargetBinding& target);
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const json::value* value);
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const json::value& value);
  std::optional<PromptCacheScopeTargetBinding> parseTargetBinding(const rt::KvValue& value);
  bool sameLiteralField(std::string_view left, std::string_view right);
  bool sameTargetDefinition(const PromptCacheScopeTargetBinding& left, const PromptCacheScopeTargetBinding& right);
  std::optional<ConcreteScopeObservation> sharedObservation(const std::vector<ConcreteScopeObservation>& observations);
  std::optional<ExperimentState> parseState(const json::value* value);
  std::optional<ExperimentState> parseState(const json::value& value);
  std::optional<ExperimentState> parseState(const rt::KvValue& value);
  bool ownsLease(const json::value* value, std::string_view owner);
  bool ownsLease(const json::value& value, std::string_view owner);
  bool ownsLease(const rt::KvValue& value, std::string_view owner);
  bool ownsCampaignLease(const json::value* value, const ExperimentState& state);
  bool ownsCampaignLease(const json::value& value, const ExperimentState& state);
  bool ownsCampaignLease(const rt::KvValue& value, const ExperimentState& state);
  json::object buildExperimentRequest(const std::string& model, const std::string& cycleId, const std::string& cacheKey);
  bool hasMixedPrefixScaleCacheSignals(const NormalizedUsage& usage);
  bool matchesCycleReusableCounter(const NormalizedUsage& usage, double reusableTokens);
  rt::Task<ReadSampleResult> readPromptCacheScopeExperimentCompletedUsage(rt::Response response, double expectedSlot, std::string expectedModel,
                                                                       double startedAtMs, rt::AbortSignal signal);
  std::optional<ConcreteScopeObservation> classifyCycle(const std::string& model, const std::vector<PromptCacheScopeSample>& samples,
                                                       const std::vector<CacheSignal>& signals);
  std::optional<StoredEvidence> parseEvidence(const json::value* value);
  std::optional<StoredEvidence> parseEvidence(const json::value& value);
  std::optional<StoredEvidence> parseEvidence(const rt::KvValue& value);
  bool activeStateEvidenceIsConsistent(const ExperimentState& state, const std::optional<StoredEvidence>& evidence);
  void throwIfAborted(const rt::AbortSignal& signal);

  #define UOS_SCOPE_RECORD(Type) \
    void tag_invoke(const json::value_from_tag&, json::value& out, const Type& value); \
    Type tag_invoke(const json::value_to_tag<Type>&, const json::value& value)
  UOS_SCOPE_RECORD(ConcreteScopeObservation);
  UOS_SCOPE_RECORD(NormalizedUsage);
  UOS_SCOPE_RECORD(PromptCacheScopeSample);
  UOS_SCOPE_RECORD(CycleEvidence);
  UOS_SCOPE_RECORD(PromptCacheScopeTargetBinding);
  UOS_SCOPE_RECORD(StoredEvidence);
  UOS_SCOPE_RECORD(ExperimentState);
  UOS_SCOPE_RECORD(ExperimentLease);
  UOS_SCOPE_RECORD(CampaignLease);
  UOS_SCOPE_RECORD(PromptCacheScopeExperimentResult);
  UOS_SCOPE_RECORD(RuntimeBinding);
  #undef UOS_SCOPE_RECORD
  void tag_invoke(const json::value_from_tag&, json::value& out, const ReadSampleResult& value);
}
