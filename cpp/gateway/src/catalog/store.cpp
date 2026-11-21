#include "uos/catalog/store.hpp"

#include "uos/models/codex_models.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/recent_model_context.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <utility>

namespace uos::catalog::store {
  namespace {
    using types::CodexCatalogMetadata;
    using types::LoadedCodexCatalog;
    using types::RefreshLease;

    constexpr double NaN = std::numeric_limits<double>::quiet_NaN();

    rt::OrderedMap<std::string, Catalog>& catalogMemo() {
      static rt::OrderedMap<std::string, Catalog> memo;
      return memo;
    }

    std::string catalogMemoKey(const CodexCatalogMetadata& metadata) {
      return metadata.client_version + ":" + metadata.auth_generation + ":" + metadata.body_generation;
    }

    void deleteCatalogMemoVersion(const std::string& version) {
      auto& memo = catalogMemo();
      for (const auto& [key, catalog] : memo) {
        if (catalog->metadata.client_version == version) memo.delete_(key);
      }
    }

    void memoizeCatalog(Catalog catalog) {
      auto& memo = catalogMemo();
      const std::string key = catalogMemoKey(catalog->metadata);
      // Map insertion order is the LRU order. Reinsert hits and replacements so
      // the first entry is always the least recently used catalog.
      memo.delete_(key);
      if (catalog->body.size() > types::CODEX_CATALOG_MEMO_BODY_BYTES) return;
      memo.set(key, std::move(catalog));
      std::size_t bodyBytes = 0;
      for (const auto& entry : memo.values()) bodyBytes += entry->body.size();
      while (static_cast<double>(memo.size()) > types::CODEX_CATALOG_MAX_VERSIONS || bodyBytes > types::CODEX_CATALOG_MEMO_BODY_BYTES) {
        const auto oldest = memo.begin();
        bodyBytes -= oldest->second->body.size();
        memo.delete_(oldest->first);
      }
    }

    rt::KvKey metadataKey(const std::string& version) { return rt::KvKey{types::CODEX_CATALOG_PREFIX, version}; }
    rt::KvKey chunkKey(const std::string& version, const std::string& generation, double index) {
      return rt::KvKey{types::CODEX_CATALOG_CHUNK_PREFIX, version, generation, index};
    }
    rt::KvKey sourceChunkKey(const std::string& version, const std::string& generation, double index) {
      return rt::KvKey{types::CODEX_CATALOG_SOURCE_CHUNK_PREFIX, version, generation, index};
    }
    rt::KvKey leaseKey(const std::string& version) { return rt::KvKey{types::CODEX_CATALOG_LEASE_PREFIX, version}; }

    rt::Task<void> deleteCatalogChunks(rt::Kv& kv, std::string version, std::string generation, double chunkCount) {
      for (double index = 0; index < chunkCount; index += 1) {
        rt::KvKey key = chunkKey(version, generation, index);
        auto deleting = kv.delete_(std::move(key));
        co_await std::move(deleting);
      }
    }

    rt::Task<void> deleteCatalogSourceChunks(rt::Kv& kv, std::string version, std::string generation, std::optional<double> chunkCount) {
      if (!chunkCount) co_return;
      for (double index = 0; index < *chunkCount; index += 1) {
        rt::KvKey key = sourceChunkKey(version, generation, index);
        auto deleting = kv.delete_(std::move(key));
        co_await std::move(deleting);
      }
    }

    rt::Task<void> deleteCatalogBodies(rt::Kv& kv, CodexCatalogMetadata metadata) {
      auto chunks = deleteCatalogChunks(kv, metadata.client_version, metadata.body_generation, metadata.chunk_count);
      co_await std::move(chunks);
      auto sourceChunks = deleteCatalogSourceChunks(kv, metadata.client_version, metadata.body_generation, metadata.source_chunk_count);
      co_await std::move(sourceChunks);
    }

    std::optional<CodexCatalogMetadata> catalogMetadata(const rt::KvValue& value);

    rt::Task<void> pruneCatalogVersions(rt::Kv& kv, std::string currentVersion) {
      struct StoredCatalog {
        rt::KvEntry<> entry;
        CodexCatalogMetadata metadata;
      };
      std::vector<StoredCatalog> catalogs;
      rt::KvListSelector selector;
      selector.prefix = types::CODEX_CATALOG_PREFIX;
      std::unique_ptr<rt::KvListIterator> iterator = kv.list(selector);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (std::optional<CodexCatalogMetadata> metadata = catalogMetadata(entry->value)) catalogs.push_back(StoredCatalog{*entry, std::move(*metadata)});
      }
      if (static_cast<double>(catalogs.size()) <= types::CODEX_CATALOG_MAX_VERSIONS) co_return;

      rt::sort(catalogs, [](const StoredCatalog& left, const StoredCatalog& right) { return left.metadata.fetched_at_ms - right.metadata.fetched_at_ms; });
      double remaining = static_cast<double>(catalogs.size());
      for (const StoredCatalog& catalog : catalogs) {
        if (remaining <= types::CODEX_CATALOG_MAX_VERSIONS) break;
        if (catalog.metadata.client_version == currentVersion) continue;
        std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(catalog.entry).delete_(catalog.entry.key);
        auto committing = operation->commit();
        const rt::KvCommitResult deleted = co_await std::move(committing);
        if (!deleted.ok) continue;
        deleteCatalogMemoVersion(catalog.metadata.client_version);
        auto deleting = deleteCatalogBodies(kv, catalog.metadata);
        co_await std::move(deleting);
        remaining -= 1;
      }
    }

    rt::Task<std::vector<std::uint8_t>> gzip(std::string_view body) { return rt::gzip::gzip(rt::encodeUtf8(body)); }

    rt::Task<std::string> gunzip(std::vector<std::uint8_t> bytes) {
      auto inflating = rt::gzip::gunzip(std::move(bytes));
      const std::vector<std::uint8_t> inflated = co_await std::move(inflating);
      co_return rt::decodeUtf8(inflated);
    }

    constexpr std::array<std::string_view, 4> MODEL_ID_KEYS{"slug", "id", "model", "name"};

    // getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name)
    std::optional<std::string> modelId(const json::object& model) {
      for (const std::string_view key : MODEL_ID_KEYS) {
        if (std::optional<std::string> id = utils::getString(rt::get(model, key))) return id;
      }
      return std::nullopt;
    }

    // The same on any record (an array has none of these members).
    std::optional<std::string> modelId(const json::value& model) {
      if (const json::object* object = model.if_object()) return modelId(*object);
      return std::nullopt;
    }

    // `value === expected` for a stored value and a string.
    bool equalsString(const rt::KvValue& value, std::string_view expected) { return value.isString() && value.asString() == expected; }

    std::optional<double> numberMember(const rt::KvObject& object, std::string_view key) {
      const rt::KvValue* member = object.find(key);
      if (member == nullptr || !member->isNumber()) return std::nullopt;
      return member->asNumber();
    }

    bool stringMember(const rt::KvObject& object, std::string_view key) {
      const rt::KvValue* member = object.find(key);
      return member != nullptr && member->isString();
    }

    bool hasValidSourceMetadata(const rt::KvObject& value) {
      static constexpr std::array<std::string_view, 4> fields{"source_chunk_count", "source_compressed_bytes", "source_body_bytes", "source_sha256"};
      if (!std::any_of(fields.begin(), fields.end(), [&](std::string_view field) { return value.contains(field); })) return true;
      const std::optional<double> sourceChunkCount = numberMember(value, "source_chunk_count");
      return sourceChunkCount && rt::Number::isSafeInteger(*sourceChunkCount) && *sourceChunkCount > 0 && numberMember(value, "source_compressed_bytes") &&
             numberMember(value, "source_body_bytes") && stringMember(value, "source_sha256");
    }

    // `isRecord(value)` admits every object, but only a plain object has these members.
    bool isCatalogMetadata(const rt::KvValue& value) {
      if (!value.isObject()) return false;
      const rt::KvObject& object = value.asObject();
      const rt::KvValue& etag = object.at("etag");
      const std::optional<double> fetchedAtMs = numberMember(object, "fetched_at_ms");
      const std::optional<double> chunkCount = numberMember(object, "chunk_count");
      return stringMember(object, "client_version") && stringMember(object, "auth_generation") && stringMember(object, "body_generation") &&
             stringMember(object, "content_type") && (etag.isNull() || etag.isString()) && fetchedAtMs && rt::Number::isFinite(*fetchedAtMs) && chunkCount &&
             rt::Number::isSafeInteger(*chunkCount) && *chunkCount > 0 && numberMember(object, "compressed_bytes") && numberMember(object, "body_bytes") &&
             stringMember(object, "sha256") && hasValidSourceMetadata(object);
    }

    // The metadata a stored value holds when isCatalogMetadata accepts it.
    std::optional<CodexCatalogMetadata> catalogMetadata(const rt::KvValue& value) {
      if (!isCatalogMetadata(value)) return std::nullopt;
      const rt::KvObject& object = value.asObject();
      const rt::KvValue& etag = object.at("etag");
      CodexCatalogMetadata metadata{
        .client_version = object.at("client_version").asString(),
        .auth_generation = object.at("auth_generation").asString(),
        .body_generation = object.at("body_generation").asString(),
        .content_type = object.at("content_type").asString(),
        .etag = etag.isNull() ? std::nullopt : std::optional<std::string>(etag.asString()),
        .fetched_at_ms = object.at("fetched_at_ms").asNumber(),
        .chunk_count = object.at("chunk_count").asNumber(),
        .compressed_bytes = object.at("compressed_bytes").asNumber(),
        .body_bytes = object.at("body_bytes").asNumber(),
        .sha256 = object.at("sha256").asString(),
      };
      if (object.contains("source_chunk_count")) {
        metadata.source_chunk_count = object.at("source_chunk_count").asNumber();
        metadata.source_compressed_bytes = object.at("source_compressed_bytes").asNumber();
        metadata.source_body_bytes = object.at("source_body_bytes").asNumber();
        metadata.source_sha256 = object.at("source_sha256").asString();
      }
      return metadata;
    }

    // `value instanceof Uint8Array`: its bytes.
    std::optional<std::span<const std::uint8_t>> uint8ArrayBytes(const rt::KvValue& value) {
      if (value.isBytes()) return std::span<const std::uint8_t>(value.asBytes());
      if (const auto* view = std::get_if<rt::KvArrayBufferView>(&value.variant()); view != nullptr && view->kind == rt::KvViewKind::Uint8Array) {
        return std::span<const std::uint8_t>(view->buffer).subspan(static_cast<std::size_t>(view->byteOffset), static_cast<std::size_t>(view->byteLength));
      }
      return std::nullopt;
    }

    rt::Task<std::optional<std::string>> readCatalogBody(rt::Kv& kv, std::function<rt::KvKey(double)> keyForChunk, double chunkCount, double compressedBytes,
                                                         double bodyBytes, std::string sha256) {
      // Array.from({ length: chunkCount }, ...): JavaScriptCore's array size limit.
      if (chunkCount > 4294967295.0) throw rt::RangeError("Array length must be a positive integer of safe magnitude.");
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      for (double index = 0; index < chunkCount; index += 1) reads.push_back(rt::Promise<rt::KvEntryMaybe<>>(kv.get(keyForChunk(index))));
      rt::Promise<std::vector<rt::KvEntryMaybe<>>> all = rt::all(std::move(reads));
      const std::vector<rt::KvEntryMaybe<>> entries = co_await all.wait();
      std::vector<std::span<const std::uint8_t>> chunks;
      double totalBytes = 0;
      for (const rt::KvEntryMaybe<>& entry : entries) {
        const std::optional<std::span<const std::uint8_t>> chunk = uint8ArrayBytes(entry.value);
        if (!chunk) co_return std::nullopt;
        chunks.push_back(*chunk);
        totalBytes += static_cast<double>(chunk->size());
      }
      if (totalBytes != compressedBytes) co_return std::nullopt;
      std::vector<std::uint8_t> compressed;
      compressed.reserve(static_cast<std::size_t>(totalBytes));
      for (const std::span<const std::uint8_t>& chunk : chunks) compressed.insert(compressed.end(), chunk.begin(), chunk.end());
      std::optional<std::string> result;
      try {
        auto inflating = gunzip(std::move(compressed));
        std::string body = co_await std::move(inflating);
        if (static_cast<double>(rt::utf8ByteLength(body)) == bodyBytes) {
          auto hashing = utils::sha256Hex(body);
          const std::string digest = co_await std::move(hashing);
          if (digest == sha256) result = std::move(body);
        }
      } catch (...) {
        result = std::nullopt;
      }
      co_return result;
    }

    bool applyCodexCatalogCompactionLimits(json::object& parsed);

    rt::Task<Catalog> loadCatalogFor(rt::Kv& kv, std::string version, rt::KvValue authGeneration, double nowMs) {
      auto reading = kv.get(metadataKey(version));
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      const std::optional<CodexCatalogMetadata> stored = catalogMetadata(entry.value);
      if (!stored || !equalsString(authGeneration, stored->auth_generation)) co_return nullptr;
      const CodexCatalogMetadata metadata = *stored;
      if (nowMs - metadata.fetched_at_ms >= types::CODEX_CATALOG_RETENTION_MS) co_return nullptr;
      const std::string memoKey = catalogMemoKey(metadata);
      if (const Catalog* cached = catalogMemo().get(memoKey)) {
        Catalog hit = *cached;
        memoizeCatalog(hit);
        co_return hit;
      }

      std::function<rt::KvKey(double)> keyForChunk = [version, generation = metadata.body_generation](double index) { return chunkKey(version, generation, index); };
      auto readingBody = readCatalogBody(kv, keyForChunk, metadata.chunk_count, metadata.compressed_bytes, metadata.body_bytes, metadata.sha256);
      std::optional<std::string> body = co_await std::move(readingBody);
      if (!body || body->empty()) co_return nullptr;
      std::optional<json::object> parsedBody = parseCatalogBody(*body);
      if (!parsedBody) co_return nullptr;
      // Persisted bytes are verified above. Project old caches before client ETag
      // validation without changing their upstream source or storage integrity.
      json::value parsed(std::move(*parsedBody));
      std::string servedBody = applyCodexCatalogCompactionLimits(parsed.as_object()) ? rt::jsonStringify(parsed) : std::move(*body);
      auto loaded = std::make_shared<LoadedCodexCatalog>(LoadedCodexCatalog{
        .metadata = metadata, .body = std::move(servedBody), .parsed = std::move(parsed.as_object())});
      memoizeCatalog(loaded);
      co_return loaded;
    }

    /**
     * Trim an optional header value, treating a missing or blank value as absent.
     * A non-empty value is returned verbatim, so blank input can never be stored as
     * if it were a real header.
     */
    std::optional<std::string> trimmedHeaderValue(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      std::string trimmed = rt::trim(*value);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }

    std::optional<double> positiveWindowCount(const json::value* value);
    std::optional<double> widestWindowCount(std::initializer_list<std::optional<double>> values);

    std::optional<double> windowTokens(const std::optional<std::int64_t>& tokens) {
      if (!tokens) return std::nullopt;
      return static_cast<double>(*tokens);
    }

    /**
     * Widen a Codex-facing catalog record to the model's real window when a source
     * states a larger one.
     *
     * The endpoint's catalog understates what it accepts: on 2026-09-17, direct
     * Codex-endpoint requests with coding headers were accepted at 916,463 input
     * tokens for `gpt-6-astra` and 918,450 for `gpt-5.6-luna`, while that catalog
     * advertises `context_window` 272,000 capped at `max_context_window` 872,000, and
     * Codex clamps an explicit `model_context_window` override to that maximum.
     * Advertising the model's actual window lets any client use it, and a client that
     * wants to stay inside a cheaper tier sets its own window or compaction limit.
     *
     * Enrichment supplies the wider window when it knows the id; ids no source widens
     * keep the endpoint's own numbers, so nothing unverified is inflated. Applied when
     * a catalog is stored, so every read path - including the verbatim-body fast path -
     * serves the widened values with matching integrity metadata. The source body is
     * retained separately so a 304 can apply a corrected enrichment without treating
     * the prior widened values as the endpoint's declaration.
     */
    bool widenCodexModelWindows(json::object& record) {
      const std::optional<std::string> id = modelId(record);
      if (!id || id->empty()) return false;
      const std::optional<models::openrouter_models::OpenRouterModelMetadata> enrichment = models::openrouter_models::openRouterMetadataFor(*id);
      if (!enrichment) return false;
      const std::optional<double> endpointWindow = positiveWindowCount(rt::get(record, "context_window"));
      const std::optional<double> endpointCeiling = positiveWindowCount(rt::get(record, "max_context_window"));
      const std::optional<double> widest =
        widestWindowCount({endpointWindow, endpointCeiling, windowTokens(enrichment->context_window_tokens), windowTokens(enrichment->max_context_window_tokens)});
      if (!widest) return false;
      bool changed = false;
      if (!endpointWindow || *widest > *endpointWindow) {
        rt::objectSet(record, "context_window", rt::makeNumber(*widest));
        changed = true;
      }
      if (!endpointCeiling || *widest > *endpointCeiling) {
        rt::objectSet(record, "max_context_window", rt::makeNumber(*widest));
        changed = true;
      }
      return changed;
    }

    std::optional<std::string> widenCodexCatalogWindows(std::string_view body) {
      std::optional<json::object> parsedBody = parseCatalogBody(body);
      if (!parsedBody) return std::nullopt;
      json::value parsed(std::move(*parsedBody));
      bool changed = false;
      for (json::value& model : *rt::getArray(parsed.as_object(), "models")) {
        if (!utils::isRecord(model) || !model.is_object()) continue;
        if (widenCodexModelWindows(model.as_object())) changed = true;
      }
      if (applyCodexCatalogCompactionLimits(parsed.as_object())) changed = true;
      return changed ? std::optional<std::string>(rt::jsonStringify(parsed)) : std::nullopt;
    }

    bool applyCodexCatalogCompactionLimits(json::object& parsed) {
      bool changed = false;
      for (json::value& entry : *rt::getArray(parsed, "models")) {
        json::object& model = entry.as_object();
        const std::optional<double> window = positiveWindowCount(rt::get(model, "context_window"));
        if (!window) continue;
        const double limit = recent_model_context::resolvedAutoCompactTokenLimit(*window, rt::getNumber(model, "auto_compact_token_limit"));
        const std::optional<double> current = rt::getNumber(rt::get(model, "auto_compact_token_limit"));
        if (current && *current == limit) continue;
        rt::objectSet(model, "auto_compact_token_limit", rt::makeNumber(limit));
        changed = true;
      }
      return changed;
    }

    std::optional<double> positiveWindowCount(const json::value* value) {
      const std::optional<double> number = rt::getNumber(value);
      if (number && rt::Number::isSafeInteger(*number) && *number > 0) return number;
      return std::nullopt;
    }

    std::optional<double> widestWindowCount(std::initializer_list<std::optional<double>> values) {
      std::optional<double> widest;
      for (const std::optional<double>& value : values) {
        if (!value) continue;
        if (!widest || *value > *widest) widest = value;
      }
      return widest;
    }

    // JS coercions for the stored values acquireRefreshLease compares as numbers.

    std::optional<std::string> joinedText(const rt::KvValue& value);

    // `String(element)` as Array.prototype.join writes it; nullopt for text that never reads as a number.
    std::optional<std::string> elementText(const rt::KvValue& item) {
      if (item.isNullish() || item.is<rt::KvHole>()) return std::string();
      if (item.isString()) return item.asString();
      if (item.isNumber()) return rt::numberToString(item.asNumber());
      if (item.isBigInt()) return item.asBigInt().toString();
      if (item.isBool()) return std::string(item.asBool() ? "true" : "false");
      return joinedText(item);
    }

    std::size_t viewElementSize(rt::KvViewKind kind) {
      switch (kind) {
        case rt::KvViewKind::Int8Array:
        case rt::KvViewKind::Uint8Array:
        case rt::KvViewKind::Uint8ClampedArray:
          return 1;
        case rt::KvViewKind::Int16Array:
        case rt::KvViewKind::Uint16Array:
          return 2;
        case rt::KvViewKind::Int32Array:
        case rt::KvViewKind::Uint32Array:
        case rt::KvViewKind::Float32Array:
          return 4;
        case rt::KvViewKind::Float64Array:
        case rt::KvViewKind::BigInt64Array:
        case rt::KvViewKind::BigUint64Array:
          return 8;
        case rt::KvViewKind::DataView:
          break;
      }
      return 0;
    }

    template <class T> T readElement(const rt::KvArrayBufferView& view) {
      T element{};
      std::memcpy(&element, view.buffer.data() + view.byteOffset, sizeof(T));
      return element;
    }

    // `String(typedArray)` of a one-element view.
    std::string viewElementText(const rt::KvArrayBufferView& view) {
      switch (view.kind) {
        case rt::KvViewKind::Int8Array:
          return rt::numberToString(readElement<std::int8_t>(view));
        case rt::KvViewKind::Uint8Array:
        case rt::KvViewKind::Uint8ClampedArray:
          return rt::numberToString(readElement<std::uint8_t>(view));
        case rt::KvViewKind::Int16Array:
          return rt::numberToString(readElement<std::int16_t>(view));
        case rt::KvViewKind::Uint16Array:
          return rt::numberToString(readElement<std::uint16_t>(view));
        case rt::KvViewKind::Int32Array:
          return rt::numberToString(readElement<std::int32_t>(view));
        case rt::KvViewKind::Uint32Array:
          return rt::numberToString(readElement<std::uint32_t>(view));
        case rt::KvViewKind::Float32Array:
          return rt::numberToString(static_cast<double>(readElement<float>(view)));
        case rt::KvViewKind::Float64Array:
          return rt::numberToString(readElement<double>(view));
        case rt::KvViewKind::BigInt64Array:
          return rt::BigInt(readElement<std::int64_t>(view)).toString();
        case rt::KvViewKind::BigUint64Array:
          return rt::BigInt::fromUint64(readElement<std::uint64_t>(view)).toString();
        case rt::KvViewKind::DataView:
          break;
      }
      return "[object DataView]";
    }

    // `String(value)` of an array or typed array (elements joined by ","), nullopt when the text can
    // never read as a number: two or more elements always contain the separator.
    std::optional<std::string> joinedText(const rt::KvValue& value) {
      if (value.isArray()) {
        const rt::KvArray& items = value.asArray();
        if (items.empty()) return std::string();
        if (items.size() > 1) return std::nullopt;
        return elementText(items.front());
      }
      if (value.isBytes()) {
        const rt::KvBytes& bytes = value.asBytes();
        if (bytes.empty()) return std::string();
        if (bytes.size() > 1) return std::nullopt;
        return rt::numberToString(bytes.front());
      }
      if (const auto* view = std::get_if<rt::KvArrayBufferView>(&value.variant())) {
        const std::size_t size = viewElementSize(view->kind);
        if (size == 0) return std::nullopt;
        const std::uint64_t elements = view->byteLength / size;
        if (elements == 0) return std::string();
        if (elements > 1) return std::nullopt;
        return viewElementText(*view);
      }
      return std::nullopt;
    }

    // ToNumber(ToPrimitive(value, number)) for every stored kind but bigint: objects other than
    // dates, arrays and typed arrays print "[object ...]" (or a RegExp or Error text), never a number.
    double primitiveNumber(const rt::KvValue& value) {
      if (value.isUndefined()) return NaN;
      if (value.isNull()) return 0;
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNumber()) return value.asNumber();
      if (value.isString()) return rt::stringToNumber(value.asString());
      if (const auto* date = std::get_if<rt::KvDate>(&value.variant())) return date->time;
      if (value.isArray() || value.isBytes() || value.is<rt::KvArrayBufferView>()) {
        const std::optional<std::string> text = joinedText(value);
        return text ? rt::stringToNumber(*text) : NaN;
      }
      return NaN;
    }

    // `left > right` with a stored `left` and a number `right`.
    bool greaterThan(const rt::KvValue& left, double right) {
      if (left.isBigInt()) {
        if (std::isnan(right) || right == std::numeric_limits<double>::infinity()) return false;
        if (right == -std::numeric_limits<double>::infinity()) return true;
        return left.asBigInt() > rt::BigInt::fromNumber(rt::Math::floor(right));
      }
      return primitiveNumber(left) > right;
    }

    // `value?.owner === owner`
    bool ownerIs(const rt::KvValue& value, std::string_view owner) {
      const rt::KvValue& member = value["owner"];
      return member.isString() && member.asString() == owner;
    }

    rt::Task<bool> renewRefreshLease(rt::Kv& kv, std::string version, std::string owner) {
      const rt::KvKey key = leaseKey(version);
      auto reading = kv.get(key);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (!ownerIs(entry.value, owner)) co_return false;
      const RefreshLease lease{.owner = owner, .lease_until_ms = rt::dateNow() + types::CODEX_CATALOG_REFRESH_LEASE_MS};
      const rt::KvSetOptions options{.expireIn = types::CODEX_CATALOG_REFRESH_LEASE_MS * 2};
      std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(entry).set(key, rt::KvValue(json::value_from(lease)), options);
      auto committing = operation->commit();
      const rt::KvCommitResult result = co_await std::move(committing);
      co_return result.ok;
    }

    struct HeartbeatState {
      rt::Kv* kv;
      std::string version;
      std::string owner;
      bool stopped = false;
      bool lost = false;
      std::optional<rt::Timeout> timer{};
      std::optional<rt::Promise<void>> renewal{};
    };

    void scheduleRenewal(const std::shared_ptr<HeartbeatState>& state) {
      std::function<void()> renew = [state] {
        rt::Promise<bool> renewing(renewRefreshLease(*state->kv, state->version, state->owner));
        state->renewal = renewing
                           .then([state](bool renewed) {
                             if (!renewed) state->lost = true;
                           })
                           .catch_([state](const rt::Reason& error) {
                             state->lost = true;
                             rt::console::error("[ai.ubq.fi] Codex catalog lease renewal failed for " + state->version + ":", error);
                           })
                           .finally_([state] {
                             state->renewal.reset();
                             if (!state->stopped && !state->lost) scheduleRenewal(state);
                           });
      };
      state->timer = rt::setTimeout(std::move(renew), rt::Math::floor(types::CODEX_CATALOG_REFRESH_LEASE_MS / 3));
    }

    // `await renewal` (`await null` while no renewal runs).
    rt::Task<void> awaitRenewal(std::optional<rt::Promise<void>> renewal) {
      if (renewal) co_await renewal->wait();
      else co_await rt::microtask();
    }
  }

  void resetCodexCatalogMemoForTest() { catalogMemo().clear(); }

  std::vector<std::string> getCodexCatalogMemoVersionsForTest() {
    std::vector<std::string> versions;
    for (const Catalog& catalog : catalogMemo().values()) versions.push_back(catalog->metadata.client_version);
    return versions;
  }

  std::optional<json::object> parseCatalogBody(std::string_view body) {
    json::value parsed;
    try {
      parsed = rt::jsonParse(body);
    } catch (const rt::JsError&) {
      return std::nullopt;
    }
    const json::array* models = rt::getArray(rt::get(parsed, "models"));
    if (!utils::isRecord(parsed) || models == nullptr || models->empty()) return std::nullopt;
    for (const json::value& model : *models) {
      if (!utils::isRecord(model)) return std::nullopt;
      const std::optional<std::string> id = modelId(model);
      if (!id || rt::trim(*id).empty()) return std::nullopt;
    }
    return std::move(parsed.as_object());
  }

  rt::Task<Catalog> loadCatalog(rt::Kv& kv, std::string version, std::string authGeneration, double nowMs) {
    return loadCatalogFor(kv, std::move(version), rt::KvValue(std::move(authGeneration)), nowMs);
  }

  rt::Task<std::optional<std::string>> loadCatalogSource(rt::Kv& kv, Catalog catalog) {
    const auto& metadata = catalog->metadata;
    if (!metadata.source_chunk_count) co_return std::nullopt;
    std::function<rt::KvKey(double)> keyForChunk = [version = metadata.client_version, generation = metadata.body_generation](double index) {
      return sourceChunkKey(version, generation, index);
    };
    auto reading = readCatalogBody(kv, std::move(keyForChunk), *metadata.source_chunk_count, metadata.source_compressed_bytes.value_or(0),
                                   metadata.source_body_bytes.value_or(0), metadata.source_sha256.value_or(""));
    auto body = co_await std::move(reading);
    if (!body || body->empty() || !parseCatalogBody(*body)) co_return std::nullopt;
    co_return body;
  }

  rt::Task<bool> storeCodexCatalog(rt::Kv& kv, StoreCodexCatalogInput input) {
    if (!models::codex_models::parseCodexClientVersion(input.clientVersion) || !parseCatalogBody(input.body)) co_return false;
    const double fetchedAtMs = input.fetchedAtMs ? *input.fetchedAtMs : rt::dateNow();
    std::optional<std::string> widened = widenCodexCatalogWindows(input.body);
    const std::string body = widened ? std::move(*widened) : input.body;
    auto compressing = gzip(body);
    const std::vector<std::uint8_t> compressed = co_await std::move(compressing);
    auto sourceCompressing = gzip(input.body);
    const std::vector<std::uint8_t> sourceCompressed = co_await std::move(sourceCompressing);
    const std::string bodyGeneration = rt::crypto::randomUUID();
    const double chunkCount = rt::Math::ceil(static_cast<double>(compressed.size()) / types::CODEX_CATALOG_CHUNK_BYTES);
    const double sourceChunkCount = rt::Math::ceil(static_cast<double>(sourceCompressed.size()) / types::CODEX_CATALOG_CHUNK_BYTES);
    const double expireIn = rt::Math::max(1.0, fetchedAtMs + types::CODEX_CATALOG_RETENTION_MS - rt::dateNow());
    const rt::KvSetOptions chunkOptions{.expireIn = expireIn};

    const auto slice = [](const std::vector<std::uint8_t>& bytes, double start) {
      const std::size_t first = static_cast<std::size_t>(start);
      const std::size_t last = std::min(bytes.size(), first + static_cast<std::size_t>(types::CODEX_CATALOG_CHUNK_BYTES));
      return rt::KvBytes(bytes.begin() + static_cast<std::ptrdiff_t>(first), bytes.begin() + static_cast<std::ptrdiff_t>(last));
    };
    for (double index = 0; index < chunkCount; index += 1) {
      const double start = index * types::CODEX_CATALOG_CHUNK_BYTES;
      rt::KvKey key = chunkKey(input.clientVersion, bodyGeneration, index);
      rt::KvValue chunk(slice(compressed, start));
      auto writing = kv.set(std::move(key), std::move(chunk), chunkOptions);
      co_await std::move(writing);
    }
    for (double index = 0; index < sourceChunkCount; index += 1) {
      const double start = index * types::CODEX_CATALOG_CHUNK_BYTES;
      rt::KvKey key = sourceChunkKey(input.clientVersion, bodyGeneration, index);
      rt::KvValue chunk(slice(sourceCompressed, start));
      auto writing = kv.set(std::move(key), std::move(chunk), chunkOptions);
      co_await std::move(writing);
    }

    auto hashing = utils::sha256Hex(body);
    const std::string sha256 = co_await std::move(hashing);
    auto sourceHashing = utils::sha256Hex(input.body);
    const std::string sourceSha256 = co_await std::move(sourceHashing);
    const CodexCatalogMetadata metadata{
      .client_version = input.clientVersion,
      .auth_generation = input.authGeneration,
      .body_generation = bodyGeneration,
      .content_type = trimmedHeaderValue(input.contentType).value_or("application/json"),
      .etag = trimmedHeaderValue(input.etag),
      .fetched_at_ms = fetchedAtMs,
      .chunk_count = chunkCount,
      .compressed_bytes = static_cast<double>(compressed.size()),
      // Integrity metadata describes the bytes actually stored, which carry the
      // raised override ceiling, not the upstream response verbatim.
      .body_bytes = static_cast<double>(rt::utf8ByteLength(body)),
      .sha256 = sha256,
      .source_chunk_count = sourceChunkCount,
      .source_compressed_bytes = static_cast<double>(sourceCompressed.size()),
      .source_body_bytes = static_cast<double>(rt::utf8ByteLength(input.body)),
      .source_sha256 = sourceSha256,
    };
    auto readingMetadata = kv.get(metadataKey(input.clientVersion));
    const rt::KvEntryMaybe<> metadataEntry = co_await std::move(readingMetadata);
    auto readingGeneration = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
    const rt::KvEntryMaybe<> generation = co_await std::move(readingGeneration);
    if (!equalsString(generation.value, input.authGeneration)) {
      auto deletingChunks = deleteCatalogChunks(kv, input.clientVersion, bodyGeneration, chunkCount);
      co_await std::move(deletingChunks);
      auto deletingSourceChunks = deleteCatalogSourceChunks(kv, input.clientVersion, bodyGeneration, sourceChunkCount);
      co_await std::move(deletingSourceChunks);
      co_return false;
    }
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    operation->check(generation).check(metadataEntry).set(metadataKey(input.clientVersion), rt::KvValue(json::value_from(metadata)), chunkOptions);
    auto publishing = operation->commit();
    const rt::KvCommitResult published = co_await std::move(publishing);
    if (!published.ok) {
      auto deletingChunks = deleteCatalogChunks(kv, input.clientVersion, bodyGeneration, chunkCount);
      co_await std::move(deletingChunks);
      auto deletingSourceChunks = deleteCatalogSourceChunks(kv, input.clientVersion, bodyGeneration, sourceChunkCount);
      co_await std::move(deletingSourceChunks);
      co_return false;
    }
    deleteCatalogMemoVersion(input.clientVersion);
    const std::optional<CodexCatalogMetadata> previous = catalogMetadata(metadataEntry.value);
    if (previous && previous->body_generation != bodyGeneration) {
      auto deletingPrevious = deleteCatalogBodies(kv, *previous);
      co_await std::move(deletingPrevious);
    }
    rt::Reason pruneFailure;
    try {
      auto pruning = pruneCatalogVersions(kv, input.clientVersion);
      co_await std::move(pruning);
    } catch (...) {
      pruneFailure = std::current_exception();
    }
    if (pruneFailure) rt::console::error("[ai.ubq.fi] Codex catalog version pruning failed:", pruneFailure);
    co_return true;
  }

  rt::Task<std::string> getAuthGeneration(rt::Kv& kv) {
    for (int attempt = 0; attempt < 3; attempt += 1) {
      auto reading = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (entry.value.truthy()) co_return entry.value.asString();
      const std::string generation = rt::crypto::randomUUID();
      std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(entry).set(CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(generation));
      auto committing = operation->commit();
      const rt::KvCommitResult commit = co_await std::move(committing);
      if (commit.ok) co_return generation;
    }
    auto reading = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    if (entry.value.truthy()) co_return entry.value.asString();
    throw rt::JsError("Deno KV could not initialize the Codex catalog auth generation");
  }

  rt::Task<bool> acquireRefreshLease(rt::Kv& kv, std::string version, std::string owner, double nowMs) {
    const rt::KvKey key = leaseKey(version);
    auto reading = kv.get(key);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    if (entry.value.truthy() && greaterThan(entry.value["lease_until_ms"], nowMs)) co_return false;
    const RefreshLease lease{.owner = owner, .lease_until_ms = nowMs + types::CODEX_CATALOG_REFRESH_LEASE_MS};
    const rt::KvSetOptions options{.expireIn = types::CODEX_CATALOG_REFRESH_LEASE_MS * 2};
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    operation->check(entry).set(key, rt::KvValue(json::value_from(lease)), options);
    auto committing = operation->commit();
    const rt::KvCommitResult result = co_await std::move(committing);
    co_return result.ok;
  }

  RefreshLeaseHeartbeat startRefreshLeaseHeartbeat(rt::Kv& kv, std::string version, std::string owner) {
    auto state = std::make_shared<HeartbeatState>(HeartbeatState{.kv = &kv, .version = std::move(version), .owner = std::move(owner)});
    scheduleRenewal(state);
    return RefreshLeaseHeartbeat{
      .lost = [state] { return state->lost; },
      .stop =
        [state] {
          state->stopped = true;
          if (state->timer) rt::clearTimeout(*state->timer);
          return awaitRenewal(state->renewal);
        },
    };
  }

  rt::Task<void> releaseRefreshLease(rt::Kv& kv, std::string version, std::string owner) {
    try {
      const rt::KvKey key = leaseKey(version);
      auto reading = kv.get(key);
      const rt::KvEntryMaybe<> entry = co_await std::move(reading);
      if (ownerIs(entry.value, owner)) {
        std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(entry).delete_(key);
        auto committing = operation->commit();
        co_await std::move(committing);
      }
    } catch (...) {
      rt::console::error("[ai.ubq.fi] Codex catalog lease release failed for " + version + ":", std::current_exception());
    }
  }

  rt::Task<bool> authGenerationIsCurrent(rt::Kv& kv, std::string expected) {
    auto reading = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
    const rt::KvEntryMaybe<> entry = co_await std::move(reading);
    co_return equalsString(entry.value, expected);
  }

  rt::Task<Catalog> loadCurrentGenerationCatalog(rt::Kv& kv, std::string version) {
    auto reading = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
    const rt::KvEntryMaybe<> generation = co_await std::move(reading);
    if (!generation.value.truthy()) co_return nullptr;
    auto loading = loadCatalogFor(kv, std::move(version), generation.value, rt::dateNow());
    auto catalog = co_await std::move(loading);
    co_return catalog;
  }

  rt::Task<Catalog> waitForColdCatalog(rt::Kv& kv, std::string version) {
    const double deadline = rt::dateNow() + types::CODEX_CATALOG_COLD_WAIT_MS;
    while (rt::dateNow() < deadline) {
      co_await rt::sleep(50);
      auto reading = kv.get(CODEX_CATALOG_AUTH_GENERATION_KEY);
      const rt::KvEntryMaybe<> authGeneration = co_await std::move(reading);
      if (!authGeneration.value.truthy()) continue;
      auto loading = loadCatalogFor(kv, version, authGeneration.value, rt::dateNow());
      auto catalog = co_await std::move(loading);
      if (catalog) co_return catalog;
    }
    co_return nullptr;
  }
}
