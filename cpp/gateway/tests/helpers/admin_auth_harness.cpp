#include "tests/helpers/admin_auth_harness.hpp"

#include "uos/kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <variant>

namespace uos::tests::helpers::admin_auth_harness {
  namespace {
    double atomicCommitsToFail = 0;
    std::optional<double> atomicCommitsBeforeFailure;

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

    // `JSON.parse(encodedKey) as KvKey`: the store only ever holds keys of string, number and
    // boolean parts.
    rt::KvKey keyFromString(const std::string& encodedKey) {
      rt::KvKey key;
      const json::value parsed = rt::jsonParse(encodedKey);
      for (const json::value& part : parsed.as_array()) {
        if (const std::optional<std::string_view> text = rt::getString(part)) key.emplace_back(std::string(*text));
        else if (const std::optional<double> number = rt::getNumber(part)) key.emplace_back(*number);
        else if (part.is_bool()) key.emplace_back(part.get_bool());
        else throw std::logic_error("admin-auth-harness: stored key part is not a string, number or boolean");
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

    class HarnessListIterator final : public rt::KvListIterator {
    public:
      HarnessListIterator(std::vector<rt::KvEntry<>> limited, std::string cursor) : limited_(std::move(limited)), cursor_(std::move(cursor)) {}
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        std::optional<rt::KvEntry<>> value;
        if (position_ < limited_.size()) value = limited_[position_++];
        co_await rt::microtask();
        co_return value;
      }
      std::string cursor() const override { return cursor_; }
      // The TS iterator has no `return`: `for await ... break` stops without one.
      rt::Task<void> return_() override { co_return; }

    private:
      std::vector<rt::KvEntry<>> limited_;
      std::size_t position_ = 0;
      std::string cursor_;
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
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck>) override { return *this; }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("chain.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.max is not a function"); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
        ops_.push_back(HarnessOp{.set = true, .key = key, .value = std::move(value)});
        atomicSetCalls.push_back(AtomicSetCall{.key = std::move(key), .expireIn = options.expireIn});
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        ops_.push_back(HarnessOp{.set = false, .key = std::move(key), .value = rt::KvValue()});
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        bool ok = true;
        if (atomicCommitsBeforeFailure) {
          if (*atomicCommitsBeforeFailure == 0) {
            atomicCommitsBeforeFailure = std::nullopt;
            ok = false;
          } else {
            *atomicCommitsBeforeFailure -= 1;
          }
        }
        if (ok && atomicCommitsToFail > 0) {
          atomicCommitsToFail -= 1;
          ok = false;
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
      std::vector<HarnessOp> ops_;
    };

    class HarnessKv final : public rt::Kv {
    public:
      using rt::Kv::get;
      using rt::Kv::getMany;
      rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions) override {
        const rt::KvValue* stored = kvStore.get(keyToString(key));
        rt::KvEntryMaybe<> entry{.key = std::move(key), .value = stored && !stored->isNullish() ? *stored : rt::KvValue(nullptr), .versionstamp = std::nullopt};
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
        std::vector<rt::KvEntry<>> entries;
        for (const auto& [encodedKey, value] : kvStore) {
          rt::KvKey key = keyFromString(encodedKey);
          if (!matchesPrefix(key, prefix)) continue;
          entries.push_back(rt::KvEntry<>{.key = std::move(key), .value = value, .versionstamp = "00000000000000000000"});
        }
        rt::sort(entries, [](const rt::KvEntry<>& left, const rt::KvEntry<>& right) { return admin_auth_harness::compareKvKeys(left.key, right.key); });
        if (options.reverse) std::reverse(entries.begin(), entries.end());
        // A numeric index cursor stands in for Deno's opaque cursor, so the
        // production pagination contract (limit + iterator.cursor) is exercisable
        // against this in-memory store. A malformed cursor throws the same
        // TypeError the real Deno KV raises.
        const double startIndex = options.cursor ? rt::stringToNumber(*options.cursor) : 0;
        if (!rt::Number::isSafeInteger(startIndex) || startIndex < 0) throw rt::TypeError("Invalid cursor");
        const std::size_t start = std::min(entries.size(), static_cast<std::size_t>(startIndex));
        std::size_t end = entries.size();
        if (options.limit) {
          const double requested = startIndex + *options.limit;
          end = requested <= static_cast<double>(start) ? start : std::min(entries.size(), static_cast<std::size_t>(rt::Math::trunc(requested)));
        }
        std::vector<rt::KvEntry<>> limited(entries.begin() + static_cast<std::ptrdiff_t>(start), entries.begin() + static_cast<std::ptrdiff_t>(end));
        const double nextIndex = startIndex + static_cast<double>(limited.size());
        // Real Deno KV reports an exhausted iterator with an empty cursor.
        std::string cursor = nextIndex >= static_cast<double>(entries.size()) ? "" : rt::numberToString(nextIndex);
        return std::make_unique<HarnessListIterator>(std::move(limited), std::move(cursor));
      }
      std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<HarnessAtomicOperation>(); }
      void close() override {}
    };
  }

  TestKvStore kvStore;
  std::vector<AtomicSetCall> atomicSetCalls;

  std::string keyToString(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      out += partJson(key[index]);
    }
    return out + "]";
  }

  std::string urlOf(const rt::RequestInfo& input) {
    if (const std::string* text = std::get_if<std::string>(&input)) return *text;
    if (const rt::URL* url = std::get_if<rt::URL>(&input)) return url->toString();
    return std::get<rt::Request>(input).url();
  }

  void TestKvStore::clear() {
    rt::OrderedMap<std::string, rt::KvValue>::clear();
    atomicCommitsToFail = 0;
    atomicCommitsBeforeFailure = std::nullopt;
    atomicSetCalls.clear();
    runtime_config::resetRuntimeConfigCacheForTest();
    codex::auth::resetCodexAuthCacheForTest();
  }

  double compareKvKeyPart(const rt::KvKeyPart& left, const rt::KvKeyPart& right) {
    if (strictEquals(left, right)) return 0;
    if (left.isNumber() && right.isNumber()) return left.number() - right.number();
    const std::string leftText = left.toString();
    const std::string rightText = right.toString();
    return rt::lessThan(leftText, rightText) ? -1 : 1;
  }

  double compareKvKeys(const rt::KvKey& left, const rt::KvKey& right) {
    const std::size_t maxLength = std::max(left.size(), right.size());
    for (std::size_t index = 0; index < maxLength; index += 1) {
      if (index >= left.size()) return -1;
      if (index >= right.size()) return 1;
      const double comparison = compareKvKeyPart(left[index], right[index]);
      if (comparison != 0) return comparison;
    }
    return 0;
  }

  bool matchesPrefix(const rt::KvKey& key, const rt::KvKey& prefix) {
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (index >= key.size() || !strictEquals(key[index], prefix[index])) return false;
    }
    return true;
  }

  bool kvStoreHasPrefix(const rt::KvKey& prefix) {
    for (const auto& [encodedKey, value] : kvStore) {
      if (matchesPrefix(keyFromString(encodedKey), prefix)) return true;
    }
    return false;
  }

  std::shared_ptr<rt::Kv> kvStub() {
    static const std::shared_ptr<rt::Kv> stub = std::make_shared<HarnessKv>();
    return stub;
  }

  void setAtomicCommitsToFail(double value) { atomicCommitsToFail = value; }
  void setAtomicCommitsBeforeFailure(std::optional<double> value) { atomicCommitsBeforeFailure = value; }

  void installHarness() { kv::setKvForTest(kvStub()); }

  void resetHarness() {
    kvStore.clear();
    atomicSetCalls.clear();
    installHarness();
  }

  void cleanupHarness() { kv::setKvForTest(nullptr); }
  std::string stringEntryLine(const rt::KvKey& key, const std::string& value) {
    return rt::jsonStringify(json::object{{"key", json::value_from(kv_conversion::profile_json::keyToJson(key))}, {"value", json::object{{"type", "string"}, {"value", value}}}, {"versionstamp", "00000000000000000000"}});
  }
  void seedCodexSnapshot(const models::codex_models::CodexModelsSnapshot& snapshot) {
    kvStore.set(keyToString(rt::KvKey{"ubq_ai", "codex_models"}), snapshot);
    const auto runtime = runtime_config::buildRuntimeConfig(snapshot);
    kvStore.set(keyToString(rt::KvKey{"uos_ai", "runtime_config", "v2"}), json::value_from(runtime));
    runtime_config::cacheRuntimeConfig(runtime);
  }
  rt::Request makeRequest(const json::value& body) {
    return rt::Request("https://ai.ubq.fi/admin/codex/auth", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(body))});
  }
  rt::Response meteredMetadataResponse(const std::string& url) {
    const rt::ResponseInit init{.status = 200, .headers = rt::Headers{{"Content-Type", "application/json"}}};
    if (url == "https://api.openlux.ai/api/ratio_config") return rt::Response(rt::jsonStringify(json::object{{"success", true}, {"data", json::object{{"model_ratio", json::object{{"gpt-5.6-sol", 1}, {"not-in-codex-catalog", 1}}}, {"model_price", json::object()}}}}), init);
    if (url == "https://api.openlux.ai/api/status") return rt::Response(rt::jsonStringify(json::object{{"success", true}, {"data", json::object{{"setup", true}, {"quota_per_unit", 500000}}}}), init);
    throw rt::JsError("Unexpected Metered metadata URL: " + url);
  }

}
