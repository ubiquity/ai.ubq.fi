#include "uos/provider/capacity_analytics.hpp"

#include "uos/provider/capacity_parse.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <utility>

namespace uos::provider::capacity_analytics {
  using capacity_contract::ProviderCapacityCodexSource;
  using capacity_contract::ProviderCapacityHistoryPoint;

  namespace {
    /** A stored point is about 1.4 KB; one row may not approach the KV value limit. */
    constexpr double MAX_REMOTE_CAPACITY_OBSERVATION_BYTES = 16 * 1024;

    /** Rows per KV transaction: one commit per batch instead of one write per row, far inside the KV mutation limits. */
    constexpr std::size_t REMOTE_CAPACITY_WRITE_BATCH = 64;

    // Same-bucket transition points (exhausted, then refilled) differ only by sample time.
    rt::KvKey remoteCapacityObservationKey(const std::string& host, const ProviderCapacityHistoryPoint& point) {
      return rt::KvKey{REMOTE_CAPACITY_OBSERVATION_PREFIX, host, point.bucket_start_at_ms, point.sampled_at_ms};
    }

    /** A point whose bucket starts on a fifteen-minute boundary and whose sample falls inside that bucket. */
    bool isBucketedSample(const ProviderCapacityHistoryPoint& point) {
      return rt::mod(point.bucket_start_at_ms, capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS) == 0 &&
             point.sampled_at_ms >= point.bucket_start_at_ms &&
             point.sampled_at_ms < point.bucket_start_at_ms + capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    }

    struct RetentionWindow {
      double cutoffMs;
      double newestBucketMs;
    };

    RetentionWindow retentionWindow(double nowMs) {
      return RetentionWindow{
        .cutoffMs = rt::Math::max(0.0, nowMs - capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS),
        .newestBucketMs = nowMs + capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS,
      };
    }

    // `left.sampled_at_ms - right.sampled_at_ms || left.bucket_start_at_ms - right.bucket_start_at_ms`
    double bySample(const ProviderCapacityHistoryPoint& left, const ProviderCapacityHistoryPoint& right) {
      const double sampled = left.sampled_at_ms - right.sampled_at_ms;
      return rt::truthy(sampled) ? sampled : left.bucket_start_at_ms - right.bucket_start_at_ms;
    }

    // `point.sources.find((source) => source.source === "codex" && source.slot === slot)`; nullptr is undefined.
    const ProviderCapacityCodexSource* codexSlot(const ProviderCapacityHistoryPoint& point, double slot) {
      if (std::get<0>(point.sources).slot == slot) return &std::get<0>(point.sources);
      if (std::get<1>(point.sources).slot == slot) return &std::get<1>(point.sources);
      return nullptr;
    }

    constexpr std::string_view NOT_CONFIGURED_SLOT = "not_configured";

    /** The account a slot reports: its credential cohort, the unconfigured marker, or null when unknown. */
    std::optional<std::string> slotAccount(const ProviderCapacityCodexSource* source) {
      if (!source) return std::nullopt;
      return source->failure_kind == std::optional<std::string>("not_configured") ? std::optional<std::string>(NOT_CONFIGURED_SLOT) : source->account_cohort_id;
    }

    /** Every configured Codex slot is available with a known cohort, and at least one slot is configured. */
    bool isAvailableSample(const ProviderCapacityHistoryPoint& point) {
      int configured = 0;
      for (const double slot : {1.0, 2.0}) {
        const ProviderCapacityCodexSource* source = codexSlot(point, slot);
        if (!source) return false;
        if (source->failure_kind == std::optional<std::string>("not_configured")) continue;
        if (source->state != "available" || !source->account_cohort_id) return false;
        configured += 1;
      }
      return configured > 0;
    }

    bool sameSlotAccounts(const ProviderCapacityHistoryPoint& left, const ProviderCapacityHistoryPoint& right) {
      for (const double slot : {1.0, 2.0}) {
        const std::optional<std::string> account = slotAccount(codexSlot(left, slot));
        if (!account || account != slotAccount(codexSlot(right, slot))) return false;
      }
      return true;
    }

    /** True when the closest local sample before or after `point` reports the same slot accounts. */
    bool matchesLocalNeighbour(const std::vector<ProviderCapacityHistoryPoint>& ordered, const ProviderCapacityHistoryPoint& point) {
      std::size_t low = 0;
      std::size_t high = ordered.size();
      while (low < high) {
        const std::size_t middle = (low + high) >> 1;
        if (ordered[middle].sampled_at_ms < point.sampled_at_ms) low = middle + 1;
        else high = middle;
      }
      const ProviderCapacityHistoryPoint* before = low > 0 ? &ordered[low - 1] : nullptr;
      const ProviderCapacityHistoryPoint* after = low < ordered.size() ? &ordered[low] : nullptr;
      return (before && sameSlotAccounts(*before, point)) || (after && sameSlotAccounts(*after, point));
    }
  }

  bool isRemoteCapacityHostId(std::string_view value) {
    // `/^[a-z0-9][a-z0-9-]{0,31}$/`
    if (value.empty() || value.size() > 32) return false;
    const auto lowerOrDigit = [](char character) { return (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9'); };
    if (!lowerOrDigit(value.front())) return false;
    for (std::size_t index = 1; index < value.size(); ++index) {
      if (!lowerOrDigit(value[index]) && value[index] != '-') return false;
    }
    return true;
  }

  bool isRemoteCapacityHostId(const json::value& value) { return value.is_string() && isRemoteCapacityHostId(std::string_view(value.get_string())); }

  bool isRemoteCapacityHostId(const json::value* value) { return value != nullptr && isRemoteCapacityHostId(*value); }

  std::optional<std::vector<ProviderCapacityHistoryPoint>> parseRemoteCapacityObservations(const json::value& value) {
    const json::array* candidates = value.if_array();
    if (!candidates || candidates->empty() || static_cast<double>(candidates->size()) > MAX_REMOTE_CAPACITY_OBSERVATIONS) return std::nullopt;
    std::vector<ProviderCapacityHistoryPoint> points;
    for (const json::value& candidate : *candidates) {
      std::optional<ProviderCapacityHistoryPoint> point = capacity_parse::readStoredHistoryPoint(rt::KvValue(candidate));
      if (!point || !isBucketedSample(*point) ||
          static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(json::value_from(*point)))) > MAX_REMOTE_CAPACITY_OBSERVATION_BYTES) {
        return std::nullopt;
      }
      points.push_back(std::move(*point));
    }
    return points;
  }

  std::optional<std::vector<ProviderCapacityHistoryPoint>> parseRemoteCapacityObservations(const json::value* value) {
    if (!value) return std::nullopt;
    return parseRemoteCapacityObservations(*value);
  }

  rt::Task<double> storeRemoteCapacityObservations(std::shared_ptr<rt::Kv> kv, std::string host, std::vector<ProviderCapacityHistoryPoint> points, double nowMs) {
    const RetentionWindow window = retentionWindow(nowMs);
    rt::OrderedMap<std::string, ProviderCapacityHistoryPoint> rows;
    for (const ProviderCapacityHistoryPoint& point : points) {
      if (point.bucket_start_at_ms < window.cutoffMs || point.bucket_start_at_ms > window.newestBucketMs) continue;
      rows.set(rt::numberToString(point.bucket_start_at_ms) + ":" + rt::numberToString(point.sampled_at_ms), point);
    }
    const std::vector<ProviderCapacityHistoryPoint> accepted = rt::arrayFrom(rows.values());
    for (std::size_t start = 0; start < accepted.size(); start += REMOTE_CAPACITY_WRITE_BATCH) {
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      const std::size_t end = std::min(accepted.size(), start + REMOTE_CAPACITY_WRITE_BATCH);
      for (std::size_t index = start; index < end; ++index) {
        const rt::KvSetOptions options{.expireIn = capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS};
        operation->set(remoteCapacityObservationKey(host, accepted[index]), rt::KvValue(json::value_from(accepted[index])), options);
      }
      auto committing = operation->commit();
      const rt::KvCommitResult committed = co_await std::move(committing);
      if (!committed.ok) throw rt::JsError("Remote capacity observation batch was not committed");
    }
    co_return static_cast<double>(accepted.size());
  }

  rt::Task<std::vector<RemoteCapacityObservation>> readRemoteCapacityObservations(std::shared_ptr<rt::Kv> kv, double nowMs) {
    const RetentionWindow window = retentionWindow(nowMs);
    std::vector<RemoteCapacityObservation> observations;
    bool failed = false;
    try {
      const rt::KvListSelector selector{.prefix = REMOTE_CAPACITY_OBSERVATION_PREFIX};
      std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
      while (true) {
        std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        // Rows written before the sample time joined the key end at the bucket; both shapes start with the host.
        const rt::KvKeyPart* host = rt::element(entry->key, static_cast<double>(REMOTE_CAPACITY_OBSERVATION_PREFIX.size()));
        std::optional<ProviderCapacityHistoryPoint> point = capacity_parse::readStoredHistoryPoint(entry->value);
        if (!host || !host->isString() || !isRemoteCapacityHostId(std::string_view(host->string())) || !point || !isBucketedSample(*point)) continue;
        if (point->bucket_start_at_ms < window.cutoffMs || point->bucket_start_at_ms > window.newestBucketMs) continue;
        observations.push_back(RemoteCapacityObservation{.host = host->string(), .point = std::move(*point)});
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return std::vector<RemoteCapacityObservation>{};
    rt::sort(observations, [](const RemoteCapacityObservation& left, const RemoteCapacityObservation& right) {
      const double order = bySample(left.point, right.point);
      return rt::truthy(order) ? order : static_cast<double>(rt::localeCompare(left.host, right.host));
    });
    co_return observations;
  }

  std::vector<ProviderCapacityHistoryPoint> mergeCapacityHistory(const std::vector<ProviderCapacityHistoryPoint>& local, const std::vector<RemoteCapacityObservation>& remote) {
    std::vector<ProviderCapacityHistoryPoint> ordered = rt::toSorted(local, bySample);
    rt::OrderedSet<double> localBuckets;
    for (const ProviderCapacityHistoryPoint& point : ordered) localBuckets.add(point.bucket_start_at_ms);
    rt::OrderedMap<double, rt::OrderedMap<std::string, rt::OrderedMap<double, ProviderCapacityHistoryPoint>>> gapFills;
    for (const auto& [host, point] : remote) {
      if (localBuckets.has(point.bucket_start_at_ms) || !isAvailableSample(point) || !matchesLocalNeighbour(ordered, point)) continue;
      const auto* existingHosts = gapFills.get(point.bucket_start_at_ms);
      rt::OrderedMap<std::string, rt::OrderedMap<double, ProviderCapacityHistoryPoint>> hosts = existingHosts ? *existingHosts : decltype(hosts){};
      const auto* existingSamples = hosts.get(host);
      rt::OrderedMap<double, ProviderCapacityHistoryPoint> samples = existingSamples ? *existingSamples : decltype(samples){};
      samples.set(point.sampled_at_ms, point);
      hosts.set(host, std::move(samples));
      gapFills.set(point.bucket_start_at_ms, std::move(hosts));
    }
    std::vector<ProviderCapacityHistoryPoint> merged = ordered;
    for (const auto& hosts : gapFills.values()) {
      std::vector<std::string> names = rt::arrayFrom(hosts.keys());
      rt::sort(names, [](const std::string& left, const std::string& right) { return static_cast<double>(rt::localeCompare(left, right)); });
      if (names.empty()) continue;
      if (const auto* samples = hosts.get(names[0])) {
        for (const auto& sample : samples->values()) merged.push_back(sample);
      }
    }
    rt::sort(merged, bySample);
    return merged;
  }
}
