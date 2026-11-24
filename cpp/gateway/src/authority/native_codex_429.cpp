#include "uos/authority/native_codex_429.hpp"

#include "uos/authority/native_codex_capacity.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::authority::native_codex_429 {
  using namespace native_codex_contracts;
  using namespace native_codex_capacity;

  namespace {
    constexpr std::array<std::string_view, 7> IMF_FIXDATE_WEEKDAYS = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
    constexpr std::array<std::string_view, 12> IMF_FIXDATE_MONTHS = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

    template <std::size_t N> bool setHas(const std::array<std::string_view, N>& set, const std::string& value) {
      return std::find(set.begin(), set.end(), value) != set.end();
    }

    bool isImfFixdate(const std::string& value) {
      static const rt::Regex IMF_FIXDATE_PATTERN("^([A-Za-z]{3}), (\\d{2}) ([A-Za-z]{3}) (\\d{4}) (\\d{2}:\\d{2}:\\d{2}) GMT$", "");
      const std::optional<rt::RegexMatch> match = IMF_FIXDATE_PATTERN.exec(value);
      return match.has_value() && setHas(IMF_FIXDATE_WEEKDAYS, *(*match)[1]) && setHas(IMF_FIXDATE_MONTHS, *(*match)[3]);
    }
  }

  std::optional<RetryAfterDeadline> futureRetryAfterDeadline(const rt::Headers& headers, double now) {
    static const rt::Regex DIGITS("^\\d+$", "");
    const std::optional<std::string> raw = headers.get("retry-after");
    if (!raw) return std::nullopt;
    const std::string value = rt::trim(*raw);
    if (DIGITS.test(value)) {
      const double seconds = rt::stringToNumber(value);
      const double deltaMs = seconds * 1'000;
      const double deadline = now + deltaMs;
      return rt::Number::isSafeInteger(seconds) && rt::Number::isSafeInteger(deltaMs) && rt::Number::isSafeInteger(deadline) && deadline > now
               ? std::optional<RetryAfterDeadline>(RetryAfterDeadline{.deadlineMs = deadline, .isStable = false})
               : std::nullopt;
    }
    if (!isImfFixdate(value)) return std::nullopt;
    const double deadline = rt::Date::parse(value);
    return rt::Number::isSafeInteger(deadline) && deadline > now && rt::Date(deadline).toUTCString() == value
             ? std::optional<RetryAfterDeadline>(RetryAfterDeadline{.deadlineMs = deadline, .isStable = true})
             : std::nullopt;
  }

  namespace {
    enum class JsonObjectKeyScanResult { valid, invalid, duplicate };

    /**
     * JSON.parse intentionally accepts repeated object keys and keeps only the
     * last one. A quota decision cannot rely on that ambiguous interpretation, so
     * scan the complete JSON grammar first and reject any repeated key.
     */
    // The TS closures share `source` and `index`; here they are the members of one scan. Positions count
    // UTF-16 code units, as `source.length`, `source[index]` and `charCodeAt` do. In the TS, parseValue,
    // parseArray and parseObject recurse once per nesting level. This scan uses an explicit stack
    // and stops at the same maximum depth as the parser that checks the complete body.
    class DuplicateJsonObjectKeyScan {
    public:
      explicit DuplicateJsonObjectKeyScan(std::string_view text) : source(rt::toUtf16(text)) {}

      // The outermost `parseValue()`.
      JsonObjectKeyScanResult parseValue() {
        for (;;) {
          // parseValue: a value starts here.
          skipWhitespace();
          const int unit = unitAt(index);
          if (unit == u'{' || unit == u'[') {
            // parseObject / parseArray up to their loops.
            const bool isObject = unit == u'{';
            if (levels.size() >= rt::limits::kJsonDepth) throw rt::SyntaxError("Invalid JSON");
            index += 1;
            skipWhitespace();
            if (unitAt(index) == (isObject ? u'}' : u']')) {
              index += 1;
            } else {
              levels.push_back(Level{.isObject = isObject, .keys = {}});
              // `while (index < source.length)`, then the loop body up to its parseValue.
              if (index >= source.size()) return JsonObjectKeyScanResult::invalid;
              if (isObject) {
                const JsonObjectKeyScanResult name = parseMemberName();
                if (name != JsonObjectKeyScanResult::valid) return name;
              }
              continue;
            }
          } else {
            const JsonObjectKeyScanResult scalar = parseScalar(unit);
            if (scalar != JsonObjectKeyScanResult::valid) return scalar;
          }
          // The value is complete ("valid"): each open level continues its loop after its parseValue.
          for (;;) {
            if (levels.empty()) return JsonObjectKeyScanResult::valid;
            Level& level = levels.back();
            skipWhitespace();
            if (unitAt(index) == (level.isObject ? u'}' : u']')) {
              index += 1;
              levels.pop_back();
              continue;
            }
            if (unitAt(index) != u',') return JsonObjectKeyScanResult::invalid;
            index += 1;
            if (level.isObject) skipWhitespace();
            if (index >= source.size()) return JsonObjectKeyScanResult::invalid;
            if (level.isObject) {
              const JsonObjectKeyScanResult name = parseMemberName();
              if (name != JsonObjectKeyScanResult::valid) return name;
            }
            break;
          }
        }
      }

    private:
      // One open parseArray or parseObject call; `keys` is parseObject's `new Set<string>()`.
      struct Level {
        bool isObject;
        std::unordered_set<std::string> keys;
      };

      std::u16string source;
      std::size_t index = 0;
      std::vector<Level> levels{};

      // `source[index]`: -1 where JS reads undefined.
      int unitAt(std::size_t at) const { return at < source.size() ? static_cast<int>(source[at]) : -1; }

      // `/[\t\n\r ]/.test(source[index])` on one code unit.
      static bool isJsonWhitespace(char16_t unit) { return unit == u'\t' || unit == u'\n' || unit == u'\r' || unit == u' '; }

      void skipWhitespace() {
        while (index < source.size() && isJsonWhitespace(source[index])) index += 1;
      }

      std::optional<std::string> parseString() {
        if (unitAt(index) != u'"') return std::nullopt;
        const std::size_t start = index;
        index += 1;
        while (index < source.size()) {
          const char16_t code = source[index];
          if (code <= 0x1f) return std::nullopt;
          if (code == 0x5c) {
            index += 2;
            continue;
          }
          index += 1;
          if (code != 0x22) continue;
          try {
            const json::value value = rt::jsonParse(rt::fromUtf16(std::u16string_view(source).substr(start, index - start)));
            return value.is_string() ? std::optional<std::string>(std::string(value.get_string())) : std::nullopt;
          } catch (...) {
            return std::nullopt;
          }
        }
        return std::nullopt;
      }

      bool parseLiteral(std::u16string_view literal) {
        if (index > source.size() || !std::u16string_view(source).substr(index).starts_with(literal)) return false;
        index += literal.size();
        return true;
      }

      bool parseNumber() {
        static const rt::Regex NUMBER("^(?:-?(?:0|[1-9]\\d*)(?:\\.\\d+)?(?:[eE][+-]?\\d+)?)", "");
        // `NUMBER.exec(source.slice(index))`. The pattern has no lookaround and consumes only ASCII
        // characters from `-+.0-9eE`, so its match on the rest of the source is its match on the longest
        // run of those characters, which bounds the copy.
        std::string candidate;
        for (std::size_t at = index; at < source.size(); ++at) {
          const char16_t unit = source[at];
          if (!((unit >= u'0' && unit <= u'9') || unit == u'-' || unit == u'+' || unit == u'.' || unit == u'e' || unit == u'E')) break;
          candidate.push_back(static_cast<char>(unit));
        }
        const std::optional<rt::RegexMatch> match = NUMBER.exec(candidate);
        if (!match) return false;
        index += match->endIndex() - match->index;
        return true;
      }

      // parseValue's cases other than `{` and `[`.
      JsonObjectKeyScanResult parseScalar(int unit) {
        switch (unit) {
          case u'"':
            return !parseString() ? JsonObjectKeyScanResult::invalid : JsonObjectKeyScanResult::valid;
          case u't':
            return parseLiteral(u"true") ? JsonObjectKeyScanResult::valid : JsonObjectKeyScanResult::invalid;
          case u'f':
            return parseLiteral(u"false") ? JsonObjectKeyScanResult::valid : JsonObjectKeyScanResult::invalid;
          case u'n':
            return parseLiteral(u"null") ? JsonObjectKeyScanResult::valid : JsonObjectKeyScanResult::invalid;
          default:
            return parseNumber() ? JsonObjectKeyScanResult::valid : JsonObjectKeyScanResult::invalid;
        }
      }

      // parseObject's loop body before its parseValue: the key, the repeated-key check and the colon.
      JsonObjectKeyScanResult parseMemberName() {
        const std::optional<std::string> key = parseString();
        if (!key) return JsonObjectKeyScanResult::invalid;
        std::unordered_set<std::string>& keys = levels.back().keys;
        if (keys.contains(*key)) return JsonObjectKeyScanResult::duplicate;
        keys.insert(*key);
        skipWhitespace();
        if (unitAt(index) != u':') return JsonObjectKeyScanResult::invalid;
        index += 1;
        return JsonObjectKeyScanResult::valid;
      }
    };

    bool hasDuplicateJsonObjectKeys(std::string_view source) {
      DuplicateJsonObjectKeyScan scan(source);
      return scan.parseValue() == JsonObjectKeyScanResult::duplicate;
    }

    struct Codex429BodyClassification {
      bool usageLimitReached;
      std::optional<double> bodyResetAtMs;
    };

    /**
     * Classify a bounded 429 body. An incomplete read, a malformed byte sequence or
     * JSON.parse's last-key-wins behavior must never turn an ambiguous upstream
     * body into a durable reset signal, so all three mean "no signal".
     */
    Codex429BodyClassification classifyCodex429Body(std::span<const std::uint8_t> bytes, bool complete, double now) {
      const Codex429BodyClassification noSignal{.usageLimitReached = false, .bodyResetAtMs = std::nullopt};
      if (!complete) return noSignal;
      try {
        const std::string bodyText = rt::decodeUtf8(bytes, {.fatal = true});
        if (hasDuplicateJsonObjectKeys(bodyText)) return noSignal;
        const json::value body = rt::jsonParse(bodyText);
        const json::value* error = utils::isRecord(body) && utils::isRecord(rt::get(body, "error")) ? rt::get(body, "error") : nullptr;
        const bool usageLimitReached = utils::getString(rt::get(error, "type")) == "usage_limit_reached";
        const std::optional<double> resetsAtSeconds = rt::getNumber(rt::get(error, "resets_at"));
        if (!usageLimitReached || !resetsAtSeconds || !rt::Number::isSafeInteger(*resetsAtSeconds) || *resetsAtSeconds < 0) {
          // A valid UTF-8, unambiguous, fully parsed OpenAI error is required
          // before routing can persist a block.
          return Codex429BodyClassification{.usageLimitReached = usageLimitReached, .bodyResetAtMs = std::nullopt};
        }
        const double deadlineMs = *resetsAtSeconds * 1'000;
        return Codex429BodyClassification{.usageLimitReached = usageLimitReached,
                                          .bodyResetAtMs = rt::Number::isSafeInteger(deadlineMs) && deadlineMs > now ? std::optional<double>(deadlineMs) : std::nullopt};
      } catch (...) {
        // A valid UTF-8, unambiguous, fully parsed OpenAI error is required
        // before routing can persist a block.
        return noSignal;
      }
    }

    struct Codex429RetryDeadlines {
      std::optional<double> retryAtMs;
      std::optional<CodexQuotaBlockSource> quotaBlockSource;
      bool resetDeadlineIsStable;
      bool resetDeadlineConflict;
    };

    std::optional<CodexQuotaBlockSource> resolveQuotaBlockSource(std::optional<double> bodyResetAtMs, const std::optional<RetryAfterDeadline>& retryAfter) {
      if (bodyResetAtMs) return CodexQuotaBlockSource("body_resets_at");
      return !retryAfter ? std::nullopt : std::optional<CodexQuotaBlockSource>("header_retry_after");
    }

    std::optional<double> resolveResetDeadlineMs(std::optional<double> bodyResetAtMs, const std::optional<RetryAfterDeadline>& retryAfter, bool resetDeadlineConflict);

    Codex429RetryDeadlines resolveCodex429RetryDeadlines(std::optional<double> bodyResetAtMs, const std::optional<RetryAfterDeadline>& retryAfter) {
      const bool absoluteHeaderConflict = bodyResetAtMs && retryAfter && retryAfter->isStable && retryAfter->deadlineMs != *bodyResetAtMs;
      const bool relativeHeaderExtendsPastBody = bodyResetAtMs && retryAfter && !retryAfter->isStable && retryAfter->deadlineMs > *bodyResetAtMs;
      const bool resetDeadlineConflict = absoluteHeaderConflict || relativeHeaderExtendsPastBody;
      return Codex429RetryDeadlines{
        .retryAtMs = resolveResetDeadlineMs(bodyResetAtMs, retryAfter, resetDeadlineConflict),
        .quotaBlockSource = resolveQuotaBlockSource(bodyResetAtMs, retryAfter),
        .resetDeadlineIsStable = bodyResetAtMs ? !resetDeadlineConflict : retryAfter && retryAfter->isStable,
        .resetDeadlineConflict = resetDeadlineConflict,
      };
    }

    /** The OpenAI-compatible replacement body for a truncated or oversized 429. */
    rt::Response truncatedCodex429Response(const rt::Response& response, const rt::Headers& headers) {
      const json::value body = json::object{
        {"error", json::object{
                    {"message", "Codex returned an oversized or incomplete rate-limit response."},
                    {"type", "rate_limit_error"},
                    {"code", "codex_rate_limit_response_truncated"},
                    {"param", nullptr},
                  }},
      };
      rt::ResponseInit init;
      init.status = response.status();
      init.statusText = response.statusText();
      init.headers = headers;
      return rt::Response(rt::BodyInit(rt::jsonStringify(body)), std::move(init));
    }
  }

  rt::Task<Codex429Classification> classifyCodex429(rt::Response response, double now) {
    rt::Headers headers(response.headers());
    // Preserve a complete upstream body for the final response while refusing
    // to classify or forward an incomplete body.
    bounded_response_body::ReadBoundedResponseBodyOptions options;
    options.cancellationReason = json::value("Codex 429 classified");
    auto read = bounded_response_body::readBoundedResponseBody(response, std::move(options));
    bounded_response_body::BoundedResponseBody bounded = co_await std::move(read);
    const Codex429BodyClassification classified = classifyCodex429Body(bounded.bytes, bounded.complete, now);
    const Codex429RetryDeadlines deadlines = resolveCodex429RetryDeadlines(classified.bodyResetAtMs, futureRetryAfterDeadline(headers, now));
    if (!bounded.complete) {
      headers.set("Content-Type", "application/json");
      Codex429Classification truncated{
        .response = truncatedCodex429Response(response, headers),
        .usageLimitReached = false,
        .retryAtMs = deadlines.retryAtMs,
        .quotaBlockSource = deadlines.quotaBlockSource,
        .resetDeadlineIsStable = deadlines.resetDeadlineIsStable,
        .resetDeadlineConflict = deadlines.resetDeadlineConflict,
      };
      co_return truncated;
    }
    rt::ResponseInit init;
    init.status = response.status();
    init.statusText = response.statusText();
    init.headers = headers;
    Codex429Classification complete{
      .response = rt::Response(rt::BodyInit(std::move(bounded.bytes)), std::move(init)),
      .usageLimitReached = classified.usageLimitReached,
      .retryAtMs = deadlines.retryAtMs,
      .quotaBlockSource = deadlines.quotaBlockSource,
      .resetDeadlineIsStable = deadlines.resetDeadlineIsStable,
      .resetDeadlineConflict = deadlines.resetDeadlineConflict,
    };
    co_return complete;
  }

  namespace {
    /**
     * Absolute reset deadline for a 429: the body value when the body carries one,
     * the larger of the two when a conflicting Retry-After header is present, and
     * the relative Retry-After delta otherwise.
     */
    std::optional<double> resolveResetDeadlineMs(std::optional<double> bodyResetAtMs, const std::optional<RetryAfterDeadline>& retryAfter, bool resetDeadlineConflict) {
      if (!bodyResetAtMs) return retryAfter ? std::optional<double>(retryAfter->deadlineMs) : std::nullopt;
      if (resetDeadlineConflict && retryAfter) return rt::Math::max(*bodyResetAtMs, retryAfter->deadlineMs);
      return bodyResetAtMs;
    }
  }

  std::optional<CodexRoutingSlot> nonBlockingQuotaProbeSlot(const CodexAccountRoutingState& state, const CodexRoutingAccount& account) {
    if (!account.probeGeneration || !rt::truthy(account.probeToken)) return std::nullopt;
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account) || !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    const CodexRoutingSlot released = routingProbeCircuit(account) == "quota" ? releaseQuotaClassProbe(current, quotaClass(account.requestedModel), requestedModelView(account.requestedModel)) : current;
    CodexRoutingSlot next = released;
    next.account_id_hash = account.accountIdHash;
    next.upstream_timeout_blocked_until_ms =
      routingProbeCircuit(account) == "upstream_timeout" ? rt::Nullable<double>(nullptr) : current.upstream_timeout_blocked_until_ms;
    next.banked_reset_recovery_probe_pending =
      routingProbeCircuit(account) == "quota" ? released.banked_reset_recovery_probe_pending : current.banked_reset_recovery_probe_pending;
    next.probe_lease = std::nullopt;
    return next;
  }

  namespace {
    /** A quota lease that belongs to another account's claim must survive an ordinary request. */
    bool isForeignQuotaProbeLease(const CodexRoutingSlot& current, const CodexRoutingAccount& account) {
      const std::optional<CodexProbeLease>& lease = current.probe_lease;
      if (account.probeGeneration || !lease || lease->circuit != "quota") return false;
      if (lease->quota_class.isNullish()) return false;
      return !codexQuotaScopeApplies(*lease->quota_class, requestedModelView(account.requestedModel));
    }

    struct CodexQuotaClassResetIdentity {
      std::optional<double> observedResetAtMs;
      bool observedResetAtIsStable;
      bool generationAmbiguous;
    };

    /**
     * Resolve the reset identity a class block records. A stable absolute deadline
     * is not by itself a provider-proven new quota-window generation: once a stable
     * observation exists, a changed date *or any later relative delay* cannot prove
     * a new provider quota generation, so the first stable observation stays
     * lookup-only and fences claims until a successful half-open probe clears it.
     * Expiry and administrative rechecks are not proof that the provider advanced
     * the quota generation.
     */
    CodexQuotaClassResetIdentity resolveQuotaClassResetIdentity(const CodexRoutingSlot& current, const std::optional<CodexQuotaClassBlock>& priorClassBlock,
                                                                const Codex429Classification& parsed, double retryAtMs, double priorDeadline, bool hasClassBlocks,
                                                                bool boundedRecoveryProbe, double now) {
      const std::optional<double> priorObservedResetAtMs = priorClassBlock && priorClassBlock->observed_reset_at_ms
                                                             ? priorClassBlock->observed_reset_at_ms
                                                             : (!hasClassBlocks ? current.observed_reset_at_ms : std::nullopt);
      const bool priorObservedResetIsStable = priorClassBlock ? priorClassBlock->observed_reset_at_is_stable : (!hasClassBlocks && current.observed_reset_at_is_stable);
      const bool hasStableObservation = priorObservedResetAtMs.has_value() && priorObservedResetIsStable;
      // A carried ambiguity only protects against re-reading an old window while
      // that window could still describe the current block. Once its deadline has
      // passed (or no observation was ever recorded), a fresh live 429 with a
      // stable future deadline is the current window: refusing here would strand
      // the reset fences forever, because every later observation inherits the
      // same flag. The durable redemption ledger still makes each window
      // spend-once, so a superseded observation cannot double-spend.
      const bool priorWindowRuledOut = !priorObservedResetAtMs || *priorObservedResetAtMs <= now;
      const bool liveDeadlineSupersedes = !hasClassBlocks && priorWindowRuledOut && parsed.resetDeadlineIsStable && retryAtMs > now;
      const bool generationAmbiguous = boundedRecoveryProbe || (priorClassBlock && priorClassBlock->banked_reset_generation_ambiguous) ||
                                       (!hasClassBlocks && current.banked_reset_generation_ambiguous && !liveDeadlineSupersedes) || parsed.resetDeadlineConflict ||
                                       (hasStableObservation && (!parsed.resetDeadlineIsStable || retryAtMs != *priorObservedResetAtMs));
      const bool preserveStableObservation = hasStableObservation && generationAmbiguous;
      // The reset observation must describe the actual circuit deadline. A shorter
      // later Retry-After cannot overwrite the identity of an earlier, longer block
      // and thereby let a stale reset clear that longer circuit.
      const bool extendsPriorDeadline = retryAtMs >= priorDeadline;
      const std::optional<double> observedResetAtMs = !preserveStableObservation && extendsPriorDeadline ? std::optional<double>(retryAtMs) : priorObservedResetAtMs;
      const bool observedResetAtIsStable = !preserveStableObservation && extendsPriorDeadline ? parsed.resetDeadlineIsStable : priorObservedResetIsStable;
      return CodexQuotaClassResetIdentity{.observedResetAtMs = observedResetAtMs, .observedResetAtIsStable = observedResetAtIsStable, .generationAmbiguous = generationAmbiguous};
    }

    /**
     * Build the class-scoped quota circuit for a 429 that proves exhaustion. A
     * verified reset can take a short time to propagate to the inference endpoint,
     * so a failed recovery probe must not turn that transient 429 into the old,
     * week-long circuit: it stays fenced and retries a bounded probe after the
     * normal half-open lease interval, and the verified redemption record prevents
     * this path from spending another reset for the same quota episode.
     */
    CodexRoutingSlot buildQuotaBlockedSlot(const CodexRoutingSlot& current, const CodexRoutingAccount& account, const Codex429Classification& parsed,
                                           const CodexQuotaBlockSource& quotaBlockSource, double retryAtMs, double now, bool recoveryProbe, bool foreignQuotaProbe) {
      const CodexQuotaClass blockedQuotaClass = quotaClass(account.requestedModel);
      const CodexQuotaBlocksByClass quotaBlocksBeforeUpdate = quotaBlocksIncludingLegacy(current);
      CodexRoutingSlot classAwareCurrent = current;
      classAwareCurrent.quota_blocks_by_class = quotaBlocksBeforeUpdate;
      const std::optional<CodexQuotaClassBlock> priorClassBlock = quotaBlockForClass(classAwareCurrent, blockedQuotaClass);
      const double priorDeadline = priorClassBlock ? priorClassBlock->blocked_until_ms : 0;
      const bool hasClassBlocks = std::any_of(quotaBlocksBeforeUpdate.begin(), quotaBlocksBeforeUpdate.end(),
                                               [](const auto& entry) { return !isCodexCapacityModelScope(entry.first); });
      const double priorTimeout = current.upstream_timeout_blocked_until_ms.valueOr(0);
      const bool boundedRecoveryProbe = recoveryProbe || (current.banked_reset_recovery_probe_pending && account.probeGeneration.has_value());
      const double deadline = boundedRecoveryProbe ? now + CODEX_HALF_OPEN_LEASE_MS : rt::Math::max(priorDeadline, retryAtMs);
      const CodexQuotaClassResetIdentity identity =
        resolveQuotaClassResetIdentity(current, priorClassBlock, parsed, retryAtMs, priorDeadline, hasClassBlocks, boundedRecoveryProbe, now);
      const CodexQuotaClassBlock blockedClassBlock{
        .blocked_until_ms = deadline,
        .source = quotaBlockSource,
        .legacy_fallback = false,
        .quota_signal_observed_at_ms = now,
        .observed_reset_at_ms = identity.observedResetAtMs,
        .observed_reset_at_is_stable = identity.observedResetAtIsStable,
        .banked_reset_generation_ambiguous = identity.generationAmbiguous,
        .banked_reset_recovery_probe_pending = (priorClassBlock && priorClassBlock->banked_reset_recovery_probe_pending) || recoveryProbe,
      };
      CodexQuotaBlocksByClass quotaBlocksByClass = quotaBlocksBeforeUpdate;
      quotaBlocksByClass.set(blockedQuotaClass, blockedClassBlock);
      // The block for this class was written into the map directly above, so the
      // reducer's seed entry is present; a missing one is a broken invariant.
      const CodexQuotaClassBlock* seed = quotaBlocksByClass.get(blockedQuotaClass);
      CodexQuotaClassBlock latestClassBlock = seed != nullptr ? *seed : blockedClassBlock;
      std::vector<std::string> quotaBlockedClasses;
      bool generationAmbiguous = false;
      bool recoveryProbePending = false;
      for (const auto& [key, block] : quotaBlocksByClass) {
        if (isCodexCapacityModelScope(key)) continue;
        if (block.blocked_until_ms > latestClassBlock.blocked_until_ms) latestClassBlock = block;
        quotaBlockedClasses.push_back(key);
        generationAmbiguous = generationAmbiguous || block.banked_reset_generation_ambiguous;
        recoveryProbePending = recoveryProbePending || block.banked_reset_recovery_probe_pending;
      }
      const std::optional<double> primaryUsedPercent = parseFinitePercent(parsed.response.headers().get("x-codex-primary-used-percent"));
      const std::optional<double> secondaryUsedPercent = parseFinitePercent(parsed.response.headers().get("x-codex-secondary-used-percent"));
      CodexRoutingSlot next = current;
      next.account_id_hash = account.accountIdHash;
      next.quota_blocked_until_ms = latestClassBlock.blocked_until_ms;
      next.quota_block_source = latestClassBlock.source;
      next.quota_blocked_classes = std::move(quotaBlockedClasses);
      next.quota_blocks_by_class = std::move(quotaBlocksByClass);
      next.upstream_timeout_blocked_until_ms = priorTimeout > now ? rt::Nullable<double>(priorTimeout) : rt::Nullable<double>(nullptr);
      next.quota_signal_observed_at_ms = now;
      next.primary_used_percent = primaryUsedPercent ? primaryUsedPercent : current.primary_used_percent;
      next.secondary_used_percent = secondaryUsedPercent ? secondaryUsedPercent : current.secondary_used_percent;
      next.observed_reset_at_ms = latestClassBlock.observed_reset_at_ms;
      next.observed_reset_at_is_stable = latestClassBlock.observed_reset_at_is_stable;
      next.banked_reset_generation_ambiguous = generationAmbiguous;
      next.banked_reset_recovery_probe_pending = recoveryProbePending;
      next.generation = foreignQuotaProbe ? current.generation : current.generation + 1;
      next.probe_lease = foreignQuotaProbe ? current.probe_lease : std::nullopt;
      return next;
    }
  }

  std::optional<CodexRoutingSlot> quotaBlockedSlot(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, const Codex429Classification& parsed,
                                                   double now, bool recoveryProbe) {
    const std::optional<double> retryAtMs = parsed.retryAtMs;
    const std::optional<CodexQuotaBlockSource> quotaBlockSource = parsed.quotaBlockSource;
    if (!retryAtMs || !quotaBlockSource) return std::nullopt;
    const CodexRoutingSlot current = slotFor(state, account);
    if (!slotMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    const bool foreignQuotaProbe = isForeignQuotaProbeLease(current, account);
    if (!account.probeGeneration && current.probe_lease && !foreignQuotaProbe) return std::nullopt;
    if (account.probeGeneration && !probeLeaseMatchesCodexRoutingAccount(current, account)) return std::nullopt;
    return buildQuotaBlockedSlot(current, account, parsed, *quotaBlockSource, *retryAtMs, now, recoveryProbe, foreignQuotaProbe);
  }

  std::optional<CodexAccountRoutingState> quotaBlockTransition(const CodexAccountRoutingState& state, const CodexRoutingAccount& account,
                                                               const Codex429Classification& parsed, double now, bool recoveryProbe, double updatedAtMs) {
    const std::optional<CodexRoutingSlot> slot = quotaBlockedSlot(state, account, parsed, now, recoveryProbe);
    if (!slot) return std::nullopt;
    return withSlot(state, account.slot, *slot, updatedAtMs);
  }

  std::optional<CodexAccountRoutingState> releaseNonBlockingQuotaProbe(const CodexAccountRoutingState& state, const CodexRoutingAccount& account, double updatedAtMs) {
    const std::optional<CodexRoutingSlot> slot = nonBlockingQuotaProbeSlot(state, account);
    if (!slot) return std::nullopt;
    return withSlot(state, account.slot, *slot, updatedAtMs);
  }
}
