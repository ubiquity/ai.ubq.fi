#include "tests/helpers/passkeys_harness.hpp"

#include "uos/kv.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <stdexcept>
#include <variant>

namespace uos::tests::helpers::passkeys_harness {
  namespace {
    std::function<void()> beforeAtomicCommit;
    double kvGetDelayMs = 0;

    // `JSON.stringify(part)` of one key part.
    std::string partJson(const rt::KvKeyPart& part) {
      if (part.isString()) return rt::jsonQuote(part.string());
      if (part.isNumber()) return rt::Number::isFinite(part.number()) ? rt::numberToString(part.number()) : "null";
      if (part.isBool()) return part.boolean() ? "true" : "false";
      if (part.isBigInt()) throw rt::TypeError("JSON.stringify cannot serialize BigInt.");
      std::string out = "{";
      const rt::KvBytes& bytes = part.bytes();
      for (std::size_t index = 0; index < bytes.size(); ++index) {
        if (index > 0) out += ",";
        out += "\"" + std::to_string(index) + "\":" + std::to_string(bytes[index]);
      }
      return out + "}";
    }

    // `JSON.parse(rawKey) as KvKey`: the store only ever holds keys of string, number and
    // boolean parts.
    rt::KvKey keyFromString(const std::string& rawKey) {
      rt::KvKey key;
      const json::value parsed = rt::jsonParse(rawKey);
      for (const json::value& part : parsed.as_array()) {
        if (const std::optional<std::string_view> text = rt::getString(part)) key.emplace_back(std::string(*text));
        else if (const std::optional<double> number = rt::getNumber(part)) key.emplace_back(*number);
        else if (part.is_bool()) key.emplace_back(part.get_bool());
        else throw std::logic_error("passkeys-harness: stored key part is not a string, number or boolean");
      }
      return key;
    }

    // `left === right` for two key parts.
    bool strictEquals(const rt::KvKeyPart& left, const rt::KvKeyPart& right) {
      if (left.isString() && right.isString()) return left.string() == right.string();
      if (left.isNumber() && right.isNumber()) return left.number() == right.number();
      if (left.isBool() && right.isBool()) return left.boolean() == right.boolean();
      if (left.isBigInt() && right.isBigInt()) return left.bigint() == right.bigint();
      return false;  // Uint8Arrays compare by identity.
    }

    // `prefix.every((part, index) => key[index] === part)`
    bool matchesPrefix(const rt::KvKey& key, const rt::KvKey& prefix) {
      for (std::size_t index = 0; index < prefix.size(); ++index) {
        if (index >= key.size() || !strictEquals(key[index], prefix[index])) return false;
      }
      return true;
    }

    // The TS `function*` list generator, read through `for await`: one microtask per step. The
    // passkeys flows never write while they list, so the rows are taken when the list starts.
    class HarnessListIterator final : public rt::KvListIterator {
    public:
      explicit HarnessListIterator(std::vector<rt::KvEntry<>> rows) : rows_(std::move(rows)) {}
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        std::optional<rt::KvEntry<>> value;
        if (position_ < rows_.size()) value = rows_[position_++];
        co_await rt::microtask();
        co_return value;
      }
      // The stub's iterator has no `cursor`.
      std::string cursor() const override { return ""; }
      rt::Task<void> return_() override {
        position_ = rows_.size();
        co_return;
      }

    private:
      std::vector<rt::KvEntry<>> rows_;
      std::size_t position_ = 0;
    };

    struct HarnessOp {
      bool set;
      rt::KvKey key;
      rt::KvValue value;
    };

    class HarnessAtomicOperation final : public rt::AtomicOperation {
    public:
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        for (rt::AtomicCheck& item : items) checks_.push_back(std::move(item));
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("chain.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.max is not a function"); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) override {
        ops_.push_back(HarnessOp{.set = true, .key = std::move(key), .value = std::move(value)});
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        ops_.push_back(HarnessOp{.set = false, .key = std::move(key), .value = rt::KvValue()});
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        const std::function<void()> hook = beforeAtomicCommit;
        if (hook) hook();
        beforeAtomicCommit = nullptr;
        bool ok = true;
        for (const rt::AtomicCheck& check : checks_) {
          // `"" ` is null for an rt::AtomicCheck, as for the SDK.
          const std::optional<std::string> expected = check.versionstamp && !check.versionstamp->empty() ? check.versionstamp : std::nullopt;
          if (versionstampFor(keyToString(check.key)) != expected) {
            ok = false;
            break;
          }
        }
        if (ok) {
          for (const HarnessOp& op : ops_) {
            if (op.set) kvStore.set(keyToString(op.key), op.value);
            else kvStore.delete_(keyToString(op.key));
          }
        }
        co_await rt::microtask();
        co_return rt::KvCommitResult{.ok = ok, .versionstamp = ""};
      }

    private:
      std::vector<rt::AtomicCheck> checks_;
      std::vector<HarnessOp> ops_;
    };

    class HarnessKv final : public rt::Kv {
    public:
      using rt::Kv::get;
      using rt::Kv::getMany;
      rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
        if (kvGetDelayMs > 0) co_await rt::sleep(kvGetDelayMs);
        const std::string rawKey = keyToString(key);
        const rt::KvValue* stored = kvStore.get(rawKey);
        rt::KvEntryMaybe<> entry{.key = std::move(key), .value = stored ? *stored : rt::KvValue(nullptr), .versionstamp = versionstampFor(rawKey)};
        co_await rt::microtask();
        co_return entry;
      }
      rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions) override {
        throw rt::TypeError("kv.getMany is not a function");
      }
      rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions) override {
        kvStore.set(keyToString(key), std::move(value));
        co_await rt::microtask();
        co_return rt::KvCommitResult{.ok = true, .versionstamp = ""};
      }
      rt::Task<void> delete_(rt::KvKey key) override {
        kvStore.delete_(keyToString(key));
        co_await rt::microtask();
      }
      std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions options) override {
        const rt::KvKey prefix = selector.prefix.value_or(rt::KvKey());
        const double limit = options.limit.value_or(rt::Number::POSITIVE_INFINITY);
        std::vector<rt::KvEntry<>> rows;
        double yielded = 0;
        for (const auto& [rawKey, value] : kvStore) {
          rt::KvKey key = keyFromString(rawKey);
          if (!matchesPrefix(key, prefix)) continue;
          rows.push_back(rt::KvEntry<>{.key = std::move(key), .value = value, .versionstamp = "00000000000000010000"});
          yielded += 1;
          if (yielded >= limit) break;
        }
        return std::make_unique<HarnessListIterator>(std::move(rows));
      }
      std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<HarnessAtomicOperation>(); }
      void close() override {}
    };
  }

  rt::OrderedMap<std::string, double> kvVersions;
  KvTestStore kvStore;

  std::string stripBase64Padding(std::string_view value) {
    double end = static_cast<double>(rt::length(value));
    while (end > 0 && rt::charAt(value, end - 1) == "=") end -= 1;
    return rt::slice(value, 0, end);
  }

  std::string encodeBase64Url(std::string_view value) {
    static const rt::Regex PLUS("\\+", "g");
    static const rt::Regex SLASH("\\/", "g");
    return rt::replace(rt::replace(stripBase64Padding(rt::btoa(value)), PLUS, "-"), SLASH, "_");
  }

  std::string keyToString(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      out += partJson(key[index]);
    }
    return out + "]";
  }

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const std::string* text = std::get_if<std::string>(&input)) return *text;
    if (const rt::URL* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  void setBeforeAtomicCommit(std::function<void()> value) { beforeAtomicCommit = std::move(value); }
  void setKvGetDelayMs(double value) { kvGetDelayMs = value; }

  KvTestStore& KvTestStore::set(const std::string& key, rt::KvValue value) {
    const double* version = kvVersions.get(key);
    kvVersions.set(key, (version ? *version : 0) + 1);
    rt::OrderedMap<std::string, rt::KvValue>::set(key, std::move(value));
    return *this;
  }

  void KvTestStore::clear() {
    beforeAtomicCommit = nullptr;
    kvGetDelayMs = 0;
    kvVersions.clear();
    rt::OrderedMap<std::string, rt::KvValue>::clear();
  }

  std::optional<std::string> versionstampFor(const std::string& rawKey) {
    if (!kvStore.has(rawKey)) return std::nullopt;
    const double* version = kvVersions.get(rawKey);
    return rt::padStart(rt::numberToString(version ? *version : 0), 20, "0");
  }

  std::shared_ptr<rt::Kv> kvStub() {
    static const std::shared_ptr<rt::Kv> stub = std::make_shared<HarnessKv>();
    return stub;
  }

  rt::Task<void> withEnv(std::vector<std::pair<std::string, std::optional<std::string>>> updates, std::function<rt::Task<void>()> fn) {
    const rt::env::ScopedOverride scope(std::move(updates));
    rt::Task<void> running = fn();
    co_await std::move(running);
  }

  SeededPasskeySession seedPasskeySession(std::string token, SeedPasskeySessionOptions options) {
    const double now = rt::dateNow();
    auth::passkeys::PasskeyUserRecord user{
      .id = "user-test",
      .handle = "uos-passkey-test",
      .is_admin = options.isAdmin,
      .credential_ids = {"credential-test"},
      .created_at_ms = now,
      .updated_at_ms = now,
    };
    kvStore.set(keyToString(auth::passkeys::passkeyUserKey(user.id)), rt::KvValue(json::value_from(user)));
    kvStore.set(keyToString(auth::passkeys::passkeyHandleKey(user.handle)), rt::KvValue(user.id));
    const auth::passkeys::PasskeySessionRecord session{
      .token = token,
      .user_id = user.id,
      .created_at_ms = now,
      .expires_at_ms = now + auth::passkeys::PASSKEY_SESSION_TTL_MS,
      .audience_origin = options.audienceOrigin.empty() ? std::nullopt : std::optional<std::string>(options.audienceOrigin),
    };
    kvStore.set(keyToString(auth::passkeys::passkeySessionKey(token)), rt::KvValue(json::value_from(session)));
    return {.token = std::move(token), .user = std::move(user)};
  }

  void installHarness() { kv::setKvForTest(kvStub()); }

  void resetHarness() {
    kvStore.clear();
    installHarness();
  }

  void cleanupHarness() { kv::setKvForTest(nullptr); }
}
