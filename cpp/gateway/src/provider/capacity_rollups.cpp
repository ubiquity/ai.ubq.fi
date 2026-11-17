#include "uos/provider/capacity_rollups.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"

#include <string_view>
#include <utility>

namespace uos::provider::capacity_rollups {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `isRecord` (src/utils.ts: `typeof value === "object" && value !== null`, arrays included)
    // over a stored value.
    bool isRecord(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }

    bool isSafeTimestamp(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0; }
    bool isSampleCount(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0; }
    bool isUsedPercent(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isFinite(value.asNumber()) && value.asNumber() >= 0 && value.asNumber() <= 100;
    }
    bool isOptionalPercent(const rt::KvValue& value) { return value.isNull() || isUsedPercent(value); }
    bool isOptionalTimestamp(const rt::KvValue& value) { return value.isNull() || isSafeTimestamp(value); }
    bool isOptionalWindowSeconds(const rt::KvValue& value) {
      return value.isNull() || (value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0);
    }

    std::optional<ProviderCapacityRollupState> capacityState(const rt::KvValue& value) {
      if (!value.isString()) return std::nullopt;
      const std::string& text = value.asString();
      return text == "available" || text == "stale" || text == "unavailable" ? std::optional<ProviderCapacityRollupState>(text) : std::nullopt;
    }

    // `value === n` for a number literal.
    bool isNumberLiteral(const rt::KvValue& value, double literal) { return value.isNumber() && value.asNumber() == literal; }

    bool isRollupWindow(const rt::KvValue& value) {
      if (!isRecord(value)) return false;
      return isSampleCount(value["samples"]) && isOptionalWindowSeconds(value["limit_window_seconds"]) && isOptionalPercent(value["min_used_percent"]) &&
             isOptionalPercent(value["max_used_percent"]) && isOptionalPercent(value["last_used_percent"]) && isOptionalTimestamp(value["last_reset_at_ms"]);
    }

    bool isRollupSlot(const rt::KvValue& value) {
      if (!isRecord(value) || (!isNumberLiteral(value["slot"], 1) && !isNumberLiteral(value["slot"], 2)) || capacityState(value["last_state"]) == std::nullopt) return false;
      return isSampleCount(value["available_sample_count"]) && isRollupWindow(value["primary"]) && isRollupWindow(value["secondary"]);
    }

    // The typed record of a value the guards accepted (every member read is present and typed).
    std::optional<double> storedNumberOrNull(const rt::KvValue& value) {
      if (value.isNull()) return std::nullopt;
      return value.asNumber();
    }

    ProviderCapacityRollupWindow storedWindow(const rt::KvValue& value) {
      return {
        .samples = value["samples"].asNumber(),
        .limit_window_seconds = storedNumberOrNull(value["limit_window_seconds"]),
        .min_used_percent = storedNumberOrNull(value["min_used_percent"]),
        .max_used_percent = storedNumberOrNull(value["max_used_percent"]),
        .last_used_percent = storedNumberOrNull(value["last_used_percent"]),
        .last_reset_at_ms = storedNumberOrNull(value["last_reset_at_ms"]),
      };
    }

    ProviderCapacityRollupSlot storedSlot(const rt::KvValue& value) {
      return {
        .slot = value["slot"].asNumber(),
        .available_sample_count = value["available_sample_count"].asNumber(),
        .last_state = value["last_state"].asString(),
        .primary = storedWindow(value["primary"]),
        .secondary = storedWindow(value["secondary"]),
      };
    }

    // A `[T, T]` tuple member: an array of exactly two objects.
    template <class T> std::array<T, 2> readPair(const Reader& in, const json::value& value, std::string_view key) {
      std::vector<T> items = in.required<std::vector<T>>(key);
      if (items.size() != 2) in.fail(key, rt::get(value, key), "[object, object]");
      return {std::move(items[0]), std::move(items[1])};
    }

    template <class T> json::array writePair(const std::array<T, 2>& items) {
      json::array out;
      for (const T& item : items) out.push_back(json::value_from(item));
      return out;
    }

    ProviderCapacityRollupWindow emptyWindowSummary() {
      return {
        .samples = 0,
        .limit_window_seconds = std::nullopt,
        .min_used_percent = std::nullopt,
        .max_used_percent = std::nullopt,
        .last_used_percent = std::nullopt,
        .last_reset_at_ms = std::nullopt,
      };
    }

    std::optional<double> minObservedPercent(std::optional<double> existing, std::optional<double> next) {
      if (next == std::nullopt) return existing;
      if (existing == std::nullopt) return next;
      return rt::Math::min(*existing, *next);
    }

    std::optional<double> maxObservedPercent(std::optional<double> existing, std::optional<double> next) {
      if (next == std::nullopt) return existing;
      if (existing == std::nullopt) return next;
      return rt::Math::max(*existing, *next);
    }

    ProviderCapacityRollupWindow mergeRollupWindow(const ProviderCapacityRollupWindow& existing, const std::optional<ProviderCapacityRollupWindowInput>& input) {
      if (!input) return existing;
      return {
        .samples = existing.samples + 1,
        .limit_window_seconds = input->limit_window_seconds ? input->limit_window_seconds : existing.limit_window_seconds,
        .min_used_percent = minObservedPercent(existing.min_used_percent, input->used_percent),
        .max_used_percent = maxObservedPercent(existing.max_used_percent, input->used_percent),
        .last_used_percent = input->used_percent ? input->used_percent : existing.last_used_percent,
        .last_reset_at_ms = input->reset_at_ms ? input->reset_at_ms : existing.last_reset_at_ms,
      };
    }

    ProviderCapacityRollupSlot emptyRollupSlot(double slot) {
      return {
        .slot = slot,
        .available_sample_count = 0,
        .last_state = "unavailable",
        .primary = emptyWindowSummary(),
        .secondary = emptyWindowSummary(),
      };
    }

    ProviderCapacityRollupSlot mergeRollupSlot(const ProviderCapacityRollupSlot& existing, const ProviderCapacityRollupSlotInput& input) {
      return {
        .slot = input.slot,
        .available_sample_count = existing.available_sample_count + (input.state == "available" ? 1 : 0),
        .last_state = input.state,
        .primary = mergeRollupWindow(existing.primary, input.primary),
        .secondary = mergeRollupWindow(existing.secondary, input.secondary),
      };
    }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupWindow& value) {
    out = Writer()
            .required("samples", value.samples)
            .orNull("limit_window_seconds", value.limit_window_seconds)
            .orNull("min_used_percent", value.min_used_percent)
            .orNull("max_used_percent", value.max_used_percent)
            .orNull("last_used_percent", value.last_used_percent)
            .orNull("last_reset_at_ms", value.last_reset_at_ms)
            .done();
  }

  ProviderCapacityRollupWindow tag_invoke(const json::value_to_tag<ProviderCapacityRollupWindow>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupWindow");
    return {
      .samples = in.required<double>("samples"),
      .limit_window_seconds = in.orNull<double>("limit_window_seconds"),
      .min_used_percent = in.orNull<double>("min_used_percent"),
      .max_used_percent = in.orNull<double>("max_used_percent"),
      .last_used_percent = in.orNull<double>("last_used_percent"),
      .last_reset_at_ms = in.orNull<double>("last_reset_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupSlot& value) {
    out = Writer()
            .required("slot", value.slot)
            .required("available_sample_count", value.available_sample_count)
            .required("last_state", value.last_state)
            .required("primary", value.primary)
            .required("secondary", value.secondary)
            .done();
  }

  ProviderCapacityRollupSlot tag_invoke(const json::value_to_tag<ProviderCapacityRollupSlot>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupSlot");
    return {
      .slot = in.required<double>("slot"),
      .available_sample_count = in.required<double>("available_sample_count"),
      .last_state = in.required<std::string>("last_state"),
      .primary = in.required<ProviderCapacityRollupWindow>("primary"),
      .secondary = in.required<ProviderCapacityRollupWindow>("secondary"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupPoint& value) {
    out = Writer()
            .required("v", value.v)
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("sample_count", value.sample_count)
            .required("first_sampled_at_ms", value.first_sampled_at_ms)
            .required("last_sampled_at_ms", value.last_sampled_at_ms)
            .required("slots", writePair(value.slots))
            .done();
  }

  ProviderCapacityRollupPoint tag_invoke(const json::value_to_tag<ProviderCapacityRollupPoint>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupPoint");
    return {
      .v = in.required<double>("v"),
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .sample_count = in.required<double>("sample_count"),
      .first_sampled_at_ms = in.required<double>("first_sampled_at_ms"),
      .last_sampled_at_ms = in.required<double>("last_sampled_at_ms"),
      .slots = readPair<ProviderCapacityRollupSlot>(in, value, "slots"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupWindowInput& value) {
    out = Writer()
            .orNull("limit_window_seconds", value.limit_window_seconds)
            .orNull("used_percent", value.used_percent)
            .orNull("reset_at_ms", value.reset_at_ms)
            .done();
  }

  ProviderCapacityRollupWindowInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupWindowInput>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupWindowInput");
    return {
      .limit_window_seconds = in.orNull<double>("limit_window_seconds"),
      .used_percent = in.orNull<double>("used_percent"),
      .reset_at_ms = in.orNull<double>("reset_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupSlotInput& value) {
    out = Writer()
            .required("slot", value.slot)
            .required("state", value.state)
            .orNull("primary", value.primary)
            .orNull("secondary", value.secondary)
            .done();
  }

  ProviderCapacityRollupSlotInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupSlotInput>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupSlotInput");
    return {
      .slot = in.required<double>("slot"),
      .state = in.required<std::string>("state"),
      .primary = in.orNull<ProviderCapacityRollupWindowInput>("primary"),
      .secondary = in.orNull<ProviderCapacityRollupWindowInput>("secondary"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupInput& value) {
    out = Writer()
            .required("bucket_start_at_ms", value.bucket_start_at_ms)
            .required("sampled_at_ms", value.sampled_at_ms)
            .required("slots", writePair(value.slots))
            .done();
  }

  ProviderCapacityRollupInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupInput>&, const json::value& value) {
    const Reader in(value, "ProviderCapacityRollupInput");
    return {
      .bucket_start_at_ms = in.required<double>("bucket_start_at_ms"),
      .sampled_at_ms = in.required<double>("sampled_at_ms"),
      .slots = readPair<ProviderCapacityRollupSlotInput>(in, value, "slots"),
    };
  }

  bool isProviderCapacityRollupPoint(const rt::KvValue& value) {
    if (!isRecord(value) || !isNumberLiteral(value["v"], 1)) return false;
    if (!isSafeTimestamp(value["bucket_start_at_ms"]) || rt::mod(value["bucket_start_at_ms"].asNumber(), PROVIDER_CAPACITY_ROLLUP_BUCKET_MS) != 0) return false;
    if (!isSampleCount(value["sample_count"]) || !isSafeTimestamp(value["first_sampled_at_ms"]) || !isSafeTimestamp(value["last_sampled_at_ms"])) return false;
    if (value["first_sampled_at_ms"].asNumber() > value["last_sampled_at_ms"].asNumber()) return false;
    const rt::KvArray* slots = value["slots"].isArray() ? &value["slots"].asArray() : nullptr;
    if (!slots || slots->size() != 2) return false;
    const rt::KvValue* slotOne = isRollupSlot(value["slots"][0]) ? &value["slots"][0] : nullptr;
    const rt::KvValue* slotTwo = isRollupSlot(value["slots"][1]) ? &value["slots"][1] : nullptr;
    if (!slotOne || !slotTwo || !isNumberLiteral((*slotOne)["slot"], 1) || !isNumberLiteral((*slotTwo)["slot"], 2)) return false;
    return (*slotOne)["available_sample_count"].asNumber() <= value["sample_count"].asNumber() &&
           (*slotTwo)["available_sample_count"].asNumber() <= value["sample_count"].asNumber();
  }

  std::optional<ProviderCapacityRollupPoint> providerCapacityRollupPointOf(const rt::KvValue& value) {
    if (!isProviderCapacityRollupPoint(value)) return std::nullopt;
    return ProviderCapacityRollupPoint{
      .v = 1,
      .bucket_start_at_ms = value["bucket_start_at_ms"].asNumber(),
      .sample_count = value["sample_count"].asNumber(),
      .first_sampled_at_ms = value["first_sampled_at_ms"].asNumber(),
      .last_sampled_at_ms = value["last_sampled_at_ms"].asNumber(),
      .slots = {storedSlot(value["slots"][0]), storedSlot(value["slots"][1])},
    };
  }

  double providerCapacityRollupBucketStartAtMs(double observedAtMs) {
    return rt::Math::floor(rt::Math::max(0.0, observedAtMs) / PROVIDER_CAPACITY_ROLLUP_BUCKET_MS) * PROVIDER_CAPACITY_ROLLUP_BUCKET_MS;
  }

  rt::KvKey providerCapacityRollupKey(double bucketStartAtMs) { return rt::KvKey{PROVIDER_CAPACITY_ROLLUP_PREFIX, bucketStartAtMs}; }

  ProviderCapacityRollupPoint mergeProviderCapacityRollup(const std::optional<ProviderCapacityRollupPoint>& existing, const ProviderCapacityRollupInput& input) {
    const ProviderCapacityRollupPoint* current = existing && existing->bucket_start_at_ms == input.bucket_start_at_ms ? &*existing : nullptr;
    const auto mergeSlot = [current](const ProviderCapacityRollupSlotInput& inputSlot) -> ProviderCapacityRollupSlot {
      const ProviderCapacityRollupSlot* found = nullptr;
      if (current) {
        for (const ProviderCapacityRollupSlot& candidate : current->slots) {
          if (candidate.slot == inputSlot.slot) {
            found = &candidate;
            break;
          }
        }
      }
      const ProviderCapacityRollupSlot existingSlot = found ? *found : emptyRollupSlot(inputSlot.slot);
      return mergeRollupSlot(existingSlot, inputSlot);
    };
    return {
      .v = 1,
      .bucket_start_at_ms = input.bucket_start_at_ms,
      .sample_count = (current ? current->sample_count : 0) + 1,
      .first_sampled_at_ms = current ? rt::Math::min(current->first_sampled_at_ms, input.sampled_at_ms) : input.sampled_at_ms,
      .last_sampled_at_ms = current ? rt::Math::max(current->last_sampled_at_ms, input.sampled_at_ms) : input.sampled_at_ms,
      .slots = {mergeSlot(input.slots[0]), mergeSlot(input.slots[1])},
    };
  }

  rt::Task<std::vector<ProviderCapacityRollupPoint>> listProviderCapacityRollups(std::shared_ptr<rt::Kv> kv, ListProviderCapacityRollupsOptions options) {
    const double limit =
        rt::Math::max(1.0, rt::Math::min(PROVIDER_CAPACITY_ROLLUP_READ_LIMIT, rt::Math::trunc(options.limit.value_or(PROVIDER_CAPACITY_ROLLUP_READ_LIMIT))));
    const double sinceMs = rt::Math::max(0.0, rt::Math::trunc(options.sinceMs));
    const double nowMs = rt::Math::max(sinceMs, rt::Math::trunc(options.nowMs));
    const double firstBucketStartAtMs = providerCapacityRollupBucketStartAtMs(sinceMs);
    const double lastBucketStartAtMs = providerCapacityRollupBucketStartAtMs(nowMs);
    const rt::KvKey start{PROVIDER_CAPACITY_ROLLUP_PREFIX, firstBucketStartAtMs};
    const rt::KvKey end{PROVIDER_CAPACITY_ROLLUP_PREFIX, lastBucketStartAtMs + PROVIDER_CAPACITY_ROLLUP_BUCKET_MS};
    std::vector<ProviderCapacityRollupPoint> points;
    // Deno KV rejects selectors that combine a prefix with both range bounds.
    rt::KvListSelector selector;
    selector.start = start;
    selector.end = end;
    rt::KvListOptions listOptions;
    listOptions.limit = limit;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, listOptions);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      std::optional<ProviderCapacityRollupPoint> rollup = providerCapacityRollupPointOf(entry->value);
      if (!rollup || rollup->bucket_start_at_ms < firstBucketStartAtMs || rollup->bucket_start_at_ms > lastBucketStartAtMs) continue;
      points.push_back(std::move(*rollup));
    }
    rt::sort(points, [](const ProviderCapacityRollupPoint& left, const ProviderCapacityRollupPoint& right) { return left.bucket_start_at_ms - right.bucket_start_at_ms; });
    co_return points;
  }
}
