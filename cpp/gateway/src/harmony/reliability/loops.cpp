#include "uos/harmony/reliability/loops.hpp"

#include "uos/harmony/reliability/hash.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cmath>
#include <cstddef>

namespace uos::harmony::reliability::loops {
  namespace {
    // Array.prototype.slice's relative index: ToIntegerOrInfinity, negative
    // values count from the end, clamped to [0, length].
    std::size_t sliceIndex(double relative, std::size_t length) {
      const double size = static_cast<double>(length);
      double index = std::isnan(relative) ? 0.0 : std::trunc(relative);
      if (index < 0) index = std::max(size + index, 0.0);
      else index = std::min(index, size);
      return static_cast<std::size_t>(index);
    }

    // `items.slice(start, end).map((e) => e[0])`
    std::vector<std::string> sliceIdentities(const std::vector<std::pair<std::string, std::string>>& items, double start, std::optional<double> end) {
      const std::size_t from = sliceIndex(start, items.size());
      const std::size_t to = end ? sliceIndex(*end, items.size()) : items.size();
      std::vector<std::string> out;
      for (std::size_t index = from; index < to; ++index) out.push_back(items[index].first);
      return out;
    }

    const std::string NUL(1, '\0');

    using rt::record::Reader;
    using rt::record::Writer;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ResultLike& value) {
    out = Writer().required("ok", value.ok).nullable("error_code", value.error_code).nullable("error", value.error).nullable("output", value.output).done();
  }

  ResultLike tag_invoke(const json::value_to_tag<ResultLike>&, const json::value& value) {
    const Reader in(value, "ResultLike");
    return ResultLike{
      .ok = in.required<bool>("ok"),
      .error_code = in.nullable<std::string>("error_code"),
      .error = in.nullable<std::string>("error"),
      .output = in.nullable<std::string>("output"),
    };
  }

  json::value canonicalize(const json::value& value) {
    if (value.is_array()) {
      json::array out;
      for (const json::value& item : value.get_array()) out.push_back(canonicalize(item));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      std::vector<std::string> keys = rt::objectKeys(value.get_object());
      rt::sort(keys, [](const std::string& a, const std::string& b) { return rt::localeCompare(a, b); });
      for (const std::string& key : keys) rt::objectSet(out, key, canonicalize(*rt::get(value.get_object(), key)));
      return out;
    }
    return value;
  }

  std::optional<json::value> canonicalize(const json::value* value) {
    if (value == nullptr) return std::nullopt;
    return canonicalize(*value);
  }

  std::string canonicalArgs(const json::object& args) { return rt::jsonStringify(canonicalize(json::value(args))); }

  std::string callIdentity(const std::string& tool, const json::object& args) { return tool + NUL + canonicalArgs(args); }

  std::string effectDigest(const ResultLike& result) {
    if (result.ok) return "ok:" + hash::digestShort(result.output.valueOr(""));
    return "fail:" + result.error_code.valueOr("none") + ":" + hash::digestShort(result.error.valueOr(""));
  }

  std::string effectSignature(const std::string& tool, const json::object& args, const ResultLike& result) {
    return callIdentity(tool, args) + NUL + effectDigest(result);
  }

  LoopDetector::LoopDetector(LoopDetectorOptions opts)
    : window_(opts.window.value_or(DEFAULT_LOOP_WINDOW)),
      patternLength_(opts.patternLength.value_or(DEFAULT_LOOP_PATTERN_LENGTH)),
      effectThreshold_(opts.effectRepeatThreshold.value_or(DEFAULT_EFFECT_REPEAT_THRESHOLD)) {}

  std::optional<DuplicateFlag> LoopDetector::checkDuplicate(const std::string& tool, const json::object& args) const {
    const std::string identity = callIdentity(tool, args);
    if (!last_ || last_->identity != identity) return std::nullopt;
    return last_->ok ? "repeat_after_success" : "exact_adjacent";
  }

  LoopFlags LoopDetector::observe(const std::string& tool, const json::object& args, const ResultLike& result) {
    const std::string identity = callIdentity(tool, args);
    const std::string effect = effectSignature(tool, args, result);
    identities_.emplace_back(identity, effect);
    while (static_cast<double>(identities_.size()) > window_) {
      if (identities_.empty()) {
        throw rt::JsError("loop detector: cannot evict a call identity from an empty history (window must not be negative)");
      }
      const std::pair<std::string, std::string> evicted = identities_.front();
      identities_.erase(identities_.begin());
      const auto found = effects_.find(evicted.second);
      effects_[evicted.second] = rt::Math::max(0.0, (found == effects_.end() ? 1.0 : found->second) - 1);
    }
    const auto counted = effects_.find(effect);
    effects_[effect] = (counted == effects_.end() ? 0.0 : counted->second) + 1;

    // Pattern recurrence: the last `patternLength` calls repeat the
    // preceding `patternLength` calls exactly.
    bool patternLoop = false;
    if (static_cast<double>(identities_.size()) >= 2 * patternLength_) {
      const std::vector<std::string> tail = sliceIdentities(identities_, -patternLength_, std::nullopt);
      const std::vector<std::string> prev = sliceIdentities(identities_, -2 * patternLength_, -patternLength_);
      patternLoop = true;
      for (std::size_t i = 0; i < tail.size(); ++i) {
        if (i >= prev.size() || tail[i] != prev[i]) {
          patternLoop = false;
          break;
        }
      }
    }
    // Effect repetition: the same effect signature repeats within the window.
    const auto repeated = effects_.find(effect);
    const bool effectLoop = (repeated == effects_.end() ? 0.0 : repeated->second) >= effectThreshold_;

    const bool semanticLoop = patternLoop || effectLoop;
    streak_ = semanticLoop ? streak_ + 1 : 0;
    // Adjacency of this call to the immediately previous one; reported
    // independently of the loop verdict (retry.ts decides on allowances).
    std::optional<DuplicateFlag> duplicate;
    if (last_ && last_->identity == identity) {
      duplicate = last_->ok ? "repeat_after_success" : "exact_adjacent";
    }
    // Pattern recurrence outranks effect repetition when both fire.
    std::optional<std::string> loopKind;
    if (patternLoop) loopKind = "pattern_recurrence";
    else if (effectLoop) loopKind = "effect_repeat";

    LoopFlags flags{duplicate, semanticLoop, loopKind, streak_};
    last_ = Last{identity, result.ok};
    flags_[identity] = flags;
    return flags;
  }

  std::optional<LoopFlags> LoopDetector::flagsFor(const std::string& identity) const {
    const auto found = flags_.find(identity);
    if (found == flags_.end()) return std::nullopt;
    return found->second;
  }

  void LoopDetector::reset() {
    last_ = std::nullopt;
    streak_ = 0;
  }

  std::string renderLoopFeedback(const LoopFlags& flags) {
    if (!flags.semanticLoop) return "";
    const std::string kind = flags.loopKind == "pattern_recurrence" ? "the same call sequence keeps repeating" : "the same action produces the same result";
    return "semantic loop detected (" + kind + "); take a materially different action or verify the result — repeating this action will not change the outcome";
  }
}
