#include "uos/provider/lithos_models.hpp"
#include "uos/rt/memory.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
namespace uos::provider::lithos_models {
namespace {
  constexpr double LITHOS_MODELS_FETCH_TIMEOUT_MS = 10'000;
  constexpr double LITHOS_MODELS_FAILURE_BACKOFF_MS = 30'000;
  std::optional<LithosModelsSnapshot> lithosModelsCache;
  std::optional<rt::Promise<std::optional<LithosModelsSnapshot>>> lithosModelsFetchInFlight;
  double lithosModelsFetchGeneration = 0;
  double lithosModelsCacheGeneration = 0;
  double lithosModelsRetryAfterMs = 0;
  std::vector<LithosModel> listedLithosModels(const json::array& data) {
    std::vector<LithosModel> models;
    for (const auto& value : data) {
      if (!value.is_object()) continue;
      auto id = utils::getString(rt::get(value, "id"));
      if (!id) continue;
      *id = rt::trim(*id);
      if (id->empty() || std::any_of(models.begin(), models.end(), [&](const auto& row) { return row.id == *id; })) continue;
      auto created = rt::getNumber(value, "created").value_or(0);
      if (!rt::Number::isSafeInteger(created)) created = 0;
      models.push_back({*id, created});
    }
    return models;
  }
  rt::Task<std::optional<LithosModelsSnapshot>> requestModels(FetchLithosModelsOptions options, std::string apiKey, rt::Fetch fetcher, double generation) {
    std::vector<rt::AbortSignal> signals{rt::AbortSignal::timeout(LITHOS_MODELS_FETCH_TIMEOUT_MS)};
    if (options.signal) signals.push_back(*options.signal);
    const auto signal = rt::AbortSignal::any(signals);
    rt::Reason failure;
    try {
      const rt::Headers headers{{"Authorization", "Bearer " + apiKey}, {"Accept", "application/json"}};
      rt::RequestInit init{.headers = headers, .signal = signal};
      rt::Promise<rt::Response> pending(fetcher(std::string(LITHOS_MODELS_URL), init));
      const auto response = co_await pending.wait();
      if (!response.ok()) { lithosModelsRetryAfterMs = rt::dateNow() + LITHOS_MODELS_FAILURE_BACKOFF_MS; co_return lithosModelsCache; }
      const auto payload = co_await response.json();
      const auto* data = rt::getArray(payload, "data");
      if (!payload.is_object() || !data) { lithosModelsRetryAfterMs = rt::dateNow() + LITHOS_MODELS_FAILURE_BACKOFF_MS; co_return lithosModelsCache; }
      const LithosModelsSnapshot snapshot{listedLithosModels(*data), rt::dateNow()};
      if (generation > lithosModelsCacheGeneration) {
        lithosModelsCacheGeneration = generation;
        lithosModelsCache = snapshot;
        lithosModelsRetryAfterMs = 0;
        const auto& models = lithosModelsCache->models;
        std::size_t bytes = models.capacity() * sizeof(LithosModel);
        for (const auto& model : models) bytes += rt::memory::bytes(model.id);
        rt::memory::setCache(rt::memory::Cache::lithos_models, models.size(), bytes);
      }
      co_return lithosModelsCache;
    } catch (...) { failure = std::current_exception(); }
    if (signal.aborted() && options.signal && options.signal->aborted()) std::rethrow_exception(failure);
    lithosModelsRetryAfterMs = rt::dateNow() + LITHOS_MODELS_FAILURE_BACKOFF_MS;
    co_return lithosModelsCache;
  }
}
/**
 * Fetch the provider's current model list for alias resolution. The static
 * `LITHOS_MODEL_IDS` list remains the cold-start and failure fallback.
 */
rt::Task<std::optional<LithosModelsSnapshot>> fetchLithosModels() { return fetchLithosModels(FetchLithosModelsOptions{}); }
rt::Task<std::optional<LithosModelsSnapshot>> fetchLithosModels(FetchLithosModelsOptions options) {
  const bool force = options.force.value_or(false);
  if (!force && lithosModelsCache && rt::dateNow() - lithosModelsCache->updated_at_ms < LITHOS_MODELS_CACHE_TTL_MS) co_return lithosModelsCache;
  if (!force && rt::dateNow() < lithosModelsRetryAfterMs) co_return lithosModelsCache;
  if (options.cachedOnly.value_or(false)) co_return lithosModelsCache;
  const auto apiKey = lithos::readLithosApiKey();
  if (!apiKey) co_return lithosModelsCache;
  auto fetcher = options.fetcher ? options.fetcher : rt::globalFetch() == rt::defaultFetch() ? rt::defaultFetch() : rt::Fetch{};
  if (!fetcher) co_return lithosModelsCache;
  const bool coalesce = !force && !options.fetcher && !options.signal;
  if (coalesce && lithosModelsFetchInFlight) { auto result = co_await lithosModelsFetchInFlight->wait(); co_return result; }
  const double generation = ++lithosModelsFetchGeneration;
  auto task = requestModels(options, *apiKey, fetcher, generation);
  rt::Promise<std::optional<LithosModelsSnapshot>> request(std::move(task));
  if (!coalesce) { auto result = co_await request.wait(); co_return result; }
  lithosModelsFetchInFlight = request;
  const auto cleanup = rt::scopeExit([request] { if (lithosModelsFetchInFlight && *lithosModelsFetchInFlight == request) lithosModelsFetchInFlight.reset(); });
  auto result = co_await request.wait();
  co_return result;
}
std::optional<LithosModelsSnapshot> lithosModelsSnapshot() { return lithosModelsCache; }
void tag_invoke(const json::value_from_tag&, json::value& out, const LithosModel& value) { out = rt::record::Writer().required("id", value.id).required("created", value.created).done(); }
LithosModel tag_invoke(const json::value_to_tag<LithosModel>&, const json::value& value) { const rt::record::Reader in(value, "LithosModel"); return {in.required<std::string>("id"), in.required<double>("created")}; }
void tag_invoke(const json::value_from_tag&, json::value& out, const LithosModelsSnapshot& value) { out = rt::record::Writer().required("models", value.models).required("updated_at_ms", value.updated_at_ms).done(); }
LithosModelsSnapshot tag_invoke(const json::value_to_tag<LithosModelsSnapshot>&, const json::value& value) { const rt::record::Reader in(value, "LithosModelsSnapshot"); return {in.required<std::vector<LithosModel>>("models"), in.required<double>("updated_at_ms")}; }
}
