#include "uos/codex/auth_import.hpp"

#include "uos/codex/auth.hpp"
#include "uos/codex/auth_repair.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/sqlite.hpp"
#include "uos/rt/url.hpp"
#include "uos/storage/kv.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <utility>
#include <vector>

namespace uos::codex::auth_import {
  namespace {
    using types::CodexAuthPoolState;
    using types::CodexAuthState;
    namespace repair = auth_repair;
    namespace sqlite = rt::sqlite;

    struct StoredPool {
      CodexAuthPoolState pool;
      std::int64_t version;
    };

    StoredPool decodePool(const std::optional<sqlite::Row>& row) {
      const auto* codec = row ? row->get("codec") : nullptr;
      if (!codec || !std::holds_alternative<std::string>(*codec) || std::get<std::string>(*codec) != storage::kv::BUN_KV_CODEC)
        throw rt::JsError("Codex auth import requires a marked bun-jsc-v1 database.");
      const auto* bytesValue = row ? row->get("v") : nullptr;
      const auto* versionValue = row ? row->get("version") : nullptr;
      const auto* bytes = bytesValue ? std::get_if<sqlite::Bytes>(bytesValue) : nullptr;
      const auto* version = versionValue ? std::get_if<std::int64_t>(versionValue) : nullptr;
      if (!bytes || !version || *version < 0) throw rt::JsError("Codex auth import requires a valid stored auth pool in both databases.");
      std::optional<CodexAuthPoolState> pool;
      try {
        pool = auth::parseCodexAuthPool(rt::storedJsonView(rt::kvJscDeserialize(*bytes)));
      } catch (...) {}
      if (!pool) throw rt::JsError("Codex auth import could not decode a valid auth pool.");
      return StoredPool{std::move(*pool), *version};
    }

    std::optional<sqlite::Row> readPoolRow(const sqlite::Database& database) {
      // Read the codec and the one durable auth row in the same snapshot.
      return database.query(
        "SELECT (SELECT value FROM uos_kv_metadata WHERE key = 'codec') AS codec, pool.v, pool.version "
        "FROM (SELECT 1) LEFT JOIN kv AS pool "
        "ON pool.k = ? AND pool.v_encoding = 1 AND pool.expiration_ms = -1 AND length(pool.v) <= 65536 LIMIT 1")
        .safeIntegers(true).get(rt::packKey(auth::CODEX_AUTH_POOL_KV_KEY));
    }

    StoredPool readStoppedPool(const std::string& path) {
      rt::URL uri = rt::pathToFileURL(path);
      uri.searchParams().set("immutable", "1");
      const sqlite::Database database(uri.href(), sqlite::constants::SQLITE_OPEN_READONLY | sqlite::constants::SQLITE_OPEN_URI);
      return decodePool(readPoolRow(database));
    }

    struct ValidatedPool {
      CodexAuthPoolState pool;
      std::vector<std::size_t> adoptedSlots;

      void requireFresh() const {
        for (const auto slot : adoptedSlots) {
          if (auth::needsRefresh(pool.accounts[slot]))
            throw rt::JsError("A leader Codex access token began to need renewal during validation.");
        }
      }
    };

    void writePool(const std::string& path, std::int64_t expectedVersion, const ValidatedPool& validated) {
      const auto encoded = rt::kvJscSerialize(rt::KvValue(json::value_from(validated.pool)));
      if (encoded.size() > 65'536) throw rt::JsError("Imported Codex auth pool exceeds the KV value limit.");
      const sqlite::Database database(path, {.readwrite = true, .safeIntegers = true});
      database.run("PRAGMA busy_timeout = 5000");
      database.run("PRAGMA synchronous = FULL");
      // The serving KV opener also sweeps expiry and recovers queues. This
      // transaction only updates the pool and advances the normal KV revision.
      database.transaction([&] {
        if (decodePool(readPoolRow(database)).version != expectedVersion) throw rt::JsError("Follower auth pool changed during validation.");
        validated.requireFresh();
        const auto revision = database.query("UPDATE data_version SET version = version + 1 WHERE k = 0 AND version >= ? AND version < ? RETURNING version")
          .get(expectedVersion, std::numeric_limits<std::int64_t>::max());
        if (!revision) throw rt::JsError("Follower KV revision is incompatible.");
        const auto updated = database.run("UPDATE kv SET v = ?, version = ? WHERE k = ?",
          encoded, std::get<std::int64_t>(*revision->get("version")), rt::packKey(auth::CODEX_AUTH_POOL_KV_KEY));
        if (updated.changes != 1) throw rt::JsError("Follower auth pool could not be updated.");
      }).immediate();
    }

    rt::Task<repair::CodexAuthProbeResult> probeAccessToken(const CodexAuthState& account) {
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = rt::Headers{{"Accept", "application/json"}, {"Authorization", "Bearer " + account.access_token},
          {"ChatGPT-Account-ID", account.account_id}, {"User-Agent", auth::codexUserAgent()}};
        init.redirect = "manual";
        init.signal = rt::AbortSignal::timeout(5'000);
        auto fetching = rt::fetch(provider::capacity_parse::codexUsageUrl(), std::move(init));
        const auto response = co_await std::move(fetching);
        if (auto body = response.body()) co_await body->cancel();
        const double status = response.status();
        if (response.ok()) co_return repair::CodexAuthProbeResultValid{status};
        if (status == 401) co_return repair::CodexAuthProbeResultInvalid{status};
        co_return repair::CodexAuthProbeResultInconclusive{status};
      } catch (...) {
        co_return repair::CodexAuthProbeResultInconclusive{std::nullopt};
      }
    }

    rt::Task<ValidatedPool> validatePoolUpdate(const CodexAuthPoolState& source, const CodexAuthPoolState& destination) {
      ValidatedPool result{destination, {}};
      bool matched = false;
      for (const auto& incoming : source.accounts) {
        const auto previous = std::find_if(destination.accounts.begin(), destination.accounts.end(),
          [&incoming](const CodexAuthState& account) { return account.account_id == incoming.account_id; });
        if (previous == destination.accounts.end()) continue;
        matched = true;
        if (incoming.access_token == previous->access_token && incoming.refresh_token == previous->refresh_token) continue;
        const auto expiry = auth::getJwtExpMs(incoming.access_token);
        if (!expiry || !rt::Number::isFinite(*expiry)) throw rt::JsError("A leader Codex access token has an unreadable lifetime.");
        const auto previousExpiry = auth::getJwtExpMs(previous->access_token);
        if (previousExpiry && (*expiry < *previousExpiry || (*expiry == *previousExpiry && incoming.updated_at_ms <= previous->updated_at_ms))) continue;
        if (auth::needsRefresh(incoming)) throw rt::JsError("A leader Codex access token already needs renewal.");

        auto probing = probeAccessToken(incoming);
        const auto probe = co_await std::move(probing);
        const repair::CodexAuthCandidate candidate{"leader-pool", incoming.account_id, incoming.access_token, incoming.refresh_token, incoming.updated_at_ms};
        if (!repair::isCodexAuthCandidateUsable({candidate, expiry, probe}, rt::dateNow()))
          throw rt::JsError("The Codex usage probe did not accept a leader access token.");

        if (incoming.native_owner) {
          auto hashing = native_auth::nativeCodexCredentialGeneration(incoming);
          const auto generation = co_await std::move(hashing);
          if (generation != incoming.native_owner->generation_hash ||
              (previous->native_owner && (previous->native_owner->auth_mode != incoming.native_owner->auth_mode ||
                previous->native_owner->chatgpt_user_id != incoming.native_owner->chatgpt_user_id)))
            throw rt::JsError("The leader Codex credential has inconsistent native ownership.");
        }
        const auto next = auth::upsertCodexAuthAccount(result.pool, incoming);
        if (!next) throw rt::JsError("The follower cannot represent the imported Codex auth pool.");
        result.pool = *next;
        result.adoptedSlots.push_back(static_cast<std::size_t>(previous - destination.accounts.begin()));
      }
      if (!matched) throw rt::JsError("The leader and follower Codex pools have no matching accounts.");
      co_return result;
    }
  }

  rt::Task<std::size_t> importCodexAuthPool(std::string sourcePath, std::string destinationPath) {
    const auto source = readStoppedPool(sourcePath);
    const auto destination = readStoppedPool(destinationPath);
    const auto validated = co_await validatePoolUpdate(source.pool, destination.pool);
    if (!validated.adoptedSlots.empty()) writePool(destinationPath, destination.version, validated);
    co_return validated.adoptedSlots.size();
  }
}
