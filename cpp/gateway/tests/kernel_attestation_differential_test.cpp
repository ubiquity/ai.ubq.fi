// Differential cases for src/kernel/attestation.ts beyond the ported kernel-coverage cases, recorded
// from the TypeScript under Bun 1.4.0 with Date.now fixed and console.error recorded: 48 sequential
// cases sharing the module's key cache, covering UOS_AI_KERNEL_PUBLIC_KEY shapes (escaped newlines,
// several blocks, a bare body, garbage), every stored key-row shape the `.map((p) => p.pem)` read can
// meet (falsy values, non-arrays, typed arrays, holes, odd elements) with the TypeErrors and import
// failures they produce, the header alg diagnostic, payload validation and clock messages, the
// installation header's Number() parsing, repo binding, signature paths, the 60 s key cache and the
// context helpers. Tokens were signed in the oracle with a fixed key, so they replay byte for byte.
// Links: tests/kernel_attestation_differential_test.cpp src/kernel/attestation.cpp src/http.cpp src/auth/relay.cpp
//   src/config.cpp src/provider/cerebras_rate_limits.cpp src/kv.cpp src/storage/kv.cpp src/utils.cpp src/release.cpp
//   src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
#include "uos/kernel/attestation.hpp"
#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "kernel_attestation_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::kernel::attestation;

namespace {
  const json::object& vectors() {
    static const json::value parsed = rt::jsonParse(kAttestationVectors);
    return parsed.as_object();
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    std::fprintf(stderr, "    %s\n      actual:   %s\n      expected: %s\n", label.c_str(), rt::jsonStringify(actual).c_str(), rt::jsonStringify(expected).c_str());
    return false;
  }

  json::value tag(const json::value& value) {
    if (rt::isNumber(value)) {
      const double number = rt::toNumber(value);
      if (number == 0 && std::signbit(number)) return json::object{{"$num", "-0"}};
      if (!rt::Number::isFinite(number)) return json::object{{"$num", rt::numberToString(number)}};
      return value;
    }
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.as_array()) out.push_back(tag(element));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      for (const auto& member : value.as_object()) out[member.key()] = tag(member.value());
      return out;
    }
    return value;
  }

  rt::KvBytes littleEndian(const json::array& numbers, bool bigIntElements) {
    rt::KvBytes bytes;
    for (const json::value& number : numbers) {
      std::uint64_t bits = 0;
      if (bigIntElements) {
        bits = static_cast<std::uint64_t>(static_cast<std::int64_t>(rt::toNumber(number)));
      } else {
        const double value = rt::toNumber(number);
        std::memcpy(&bits, &value, sizeof bits);
      }
      for (int shift = 0; shift < 64; shift += 8) bytes.push_back(static_cast<std::uint8_t>(bits >> shift));
    }
    return bytes;
  }

  // The oracle's tagged stored values back to the JS values they stand for.
  rt::KvValue decode(const json::value& value) {
    if (value.is_array()) {
      rt::KvArray out;
      for (const json::value& element : value.as_array()) out.push_back(decode(element));
      return rt::KvValue(std::move(out));
    }
    if (!value.is_object()) return rt::KvValue(value);
    const json::object& object = value.as_object();
    if (const json::value* number = rt::get(object, "$num")) return rt::KvValue(rt::stringToNumber(number->as_string()));
    if (const json::value* bytes = rt::get(object, "$u8")) {
      rt::KvBytes out;
      for (const json::value& byte : bytes->as_array()) out.push_back(static_cast<std::uint8_t>(rt::toNumber(byte)));
      return rt::KvValue(std::move(out));
    }
    if (const json::value* doubles = rt::get(object, "$f64")) {
      rt::KvBytes buffer = littleEndian(doubles->as_array(), false);
      const std::uint64_t length = buffer.size();
      return rt::KvValue(rt::KvArrayBufferView{.kind = rt::KvViewKind::Float64Array, .buffer = std::move(buffer), .byteOffset = 0, .byteLength = length});
    }
    if (const json::value* bigints = rt::get(object, "$bi64")) {
      rt::KvBytes buffer = littleEndian(bigints->as_array(), true);
      const std::uint64_t length = buffer.size();
      return rt::KvValue(rt::KvArrayBufferView{.kind = rt::KvViewKind::BigInt64Array, .buffer = std::move(buffer), .byteOffset = 0, .byteLength = length});
    }
    if (const json::value* time = rt::get(object, "$date")) return rt::KvValue(rt::KvDate{.time = rt::toNumber(*time)});
    if (const json::value* bigint = rt::get(object, "$bigint")) return rt::KvValue(rt::BigInt::fromString(std::string(bigint->as_string())));
    if (const json::value* entries = rt::get(object, "$map")) {
      rt::KvMap map;
      for (const json::value& entry : entries->as_array()) map.set(decode(entry.as_array()[0]), decode(entry.as_array()[1]));
      return rt::KvValue(std::move(map));
    }
    if (const json::value* sparse = rt::get(object, "$sparse")) {
      const json::object& spec = sparse->as_object();
      rt::KvArray out(static_cast<std::size_t>(rt::toNumber(spec.at("length"))), rt::KvValue(rt::KvHole{}));
      for (const auto& member : spec.at("at").as_object()) out[static_cast<std::size_t>(rt::stringToNumber(member.key()))] = decode(member.value());
      return rt::KvValue(std::move(out));
    }
    rt::KvObject out;
    for (const auto& member : object) out.set(std::string(member.key()), decode(member.value()));
    return rt::KvValue(std::move(out));
  }

  rt::Request requestOf(const json::object& spec) {
    rt::Headers headers;
    const json::value& kernelToken = spec.at("kernelToken");
    if (!kernelToken.is_null()) headers.append("X-Ubiquity-Kernel-Token", kernelToken.as_string());
    if (const json::value* extra = rt::get(spec, "headers")) {
      for (const auto& member : extra->as_object()) headers.append(member.key(), member.value().as_string());
    }
    rt::RequestInit init;
    init.method = "POST";
    init.headers = headers;
    return rt::Request("https://ai.ubq.fi/v1/responses", init);
  }

  std::optional<std::string> nullableString(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    return std::string(value.as_string());
  }

  json::value repoJson(const std::optional<GitHubRepo>& repo) {
    if (!repo) return nullptr;
    return json::object{{"owner", repo->owner}, {"repo", repo->repo}};
  }

  rt::Task<json::value> outcomeOf(json::object spec) {
    json::object outcome;
    const std::string mode = spec.contains("mode") ? std::string(spec.at("mode").as_string()) : std::string("verify");
    try {
      const rt::Request request = requestOf(spec);
      if (mode == "context") {
        rt::Task<std::optional<GitHubRepo>> resolving = getKernelAttestationContext(request, nullableString(spec.at("token")));
        const std::optional<GitHubRepo> context = co_await std::move(resolving);
        outcome["context"] = repoJson(context);
      } else if (mode == "headers") {
        outcome["headers"] = repoJson(getGitHubRepoHeaders(request));
      } else {
        VerifyKernelAttestationOptions options;
        options.token = std::string(spec.at("token").as_string());
        if (const json::value* owner = rt::get(spec, "owner")) options.owner = owner->is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(std::string(owner->as_string()));
        if (const json::value* repo = rt::get(spec, "repo")) options.repo = repo->is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(std::string(repo->as_string()));
        rt::Task<KernelAttestationResult> verifying = verifyKernelAttestation(request, std::move(options));
        const KernelAttestationResult result = co_await std::move(verifying);
        if (const KernelAttestationSuccess* success = std::get_if<KernelAttestationSuccess>(&result)) {
          outcome["ok"] = true;
          outcome["payload"] = tag(json::value_from(success->payload));
        } else {
          const rt::Response& response = std::get<KernelAttestationFailure>(result).response;
          outcome["ok"] = false;
          outcome["status"] = response.status();
          const json::value body = co_await response.json();
          outcome["body"] = body;
        }
      }
    } catch (const rt::JsError& error) {
      outcome.clear();
      outcome["error"] = error.name + ": " + error.message;
    }
    co_return outcome;
  }
}

ASYNC_TEST("environment keys, stored key rows, verification and the key cache match the oracle") {
  const rt::env::ScopedOverride restoreEnvironment({{"UOS_AI_KERNEL_PUBLIC_KEY", std::nullopt}});
  double clock = 1'800'000'000'123;
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [&clock] { return clock; };
  const rt::KvKey key{"uos_ai", "kernel_pubkeys"};
  int index = 0;
  for (const json::value& item : vectors().at("cases").as_array()) {
    const json::object& testCase = item.as_object();
    const std::string label(testCase.at("label").as_string());
    const json::object& setup = testCase.at("setup").as_object();
    if (const json::value* at = rt::get(setup, "clock")) clock = rt::toNumber(*at);
    if (const json::value* env = rt::get(setup, "env")) {
      if (env->is_null()) rt::env::remove("UOS_AI_KERNEL_PUBLIC_KEY");
      else rt::env::set("UOS_AI_KERNEL_PUBLIC_KEY", env->as_string());
    }
    if (const json::value* stored = rt::get(setup, "kv")) {
      const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
      if (!(stored->is_string() && stored->as_string() == "<absent>")) {
        const rt::KvValue value = decode(*stored);
        co_await kv->set(key, value);
      }
      uos::kv::setKvForTest(kv);
    }

    rt::console::Capture capture({rt::console::Method::error});
    json::value reload = nullptr;
    if (const json::value* wanted = rt::get(setup, "reload"); wanted != nullptr && wanted->as_bool()) {
      rt::Reason failure;
      try {
        co_await reloadKernelPublicKeys();
      } catch (...) {
        failure = std::current_exception();
      }
      reload = failure ? json::value(json::object{{"error", *rt::errorName(failure) + ": " + *rt::errorMessage(failure)}}) : json::value(json::object{{"ok", true}});
    }
    EXPECT(same(reload, testCase.at("reload"), label + " reload"));

    const json::array& requests = testCase.at("requests").as_array();
    const json::array& outcomes = testCase.at("outcomes").as_array();
    ASSERT_EQ(requests.size(), outcomes.size());
    for (std::size_t request = 0; request < requests.size(); request += 1) {
      rt::Task<json::value> running = outcomeOf(requests[request].as_object());
      const json::value outcome = co_await std::move(running);
      EXPECT(same(outcome, outcomes[request], label + " request " + std::to_string(request)));
    }

    json::array logs;
    for (const rt::console::Call& call : capture.calls()) {
      const rt::console::ErrorInfo& error = call.args.at(1).errorInfo();
      logs.push_back(json::array{json::value(call.args.at(0).string()), json::value(error.name), json::value(error.message)});
    }
    EXPECT(same(logs, testCase.at("logs"), label + " logs"));
    ++index;
  }
  EXPECT_EQ(index, 48);

  rt::env::set("UOS_AI_KERNEL_PUBLIC_KEY", "env-value");
  EXPECT_EQ(getEnv("UOS_AI_KERNEL_PUBLIC_KEY"), std::optional<std::string>(std::string(vectors().at("envRead").as_string())));
  rt::env::remove("UOS_AI_KERNEL_PUBLIC_KEY");
  EXPECT(!getEnv("UOS_AI_KERNEL_PUBLIC_KEY").has_value() && vectors().at("envMissing").as_string() == "<undefined>");
  uos::kv::setKvForTest(nullptr);
  rt::globalDateNow() = realNow;
}
