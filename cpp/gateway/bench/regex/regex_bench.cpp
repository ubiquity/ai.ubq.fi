#include "uos/codex/supervisor_secret.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;

  struct Outcome {
    std::optional<bool> test;
    std::optional<rt::RegexMatch> match;
    std::vector<rt::RegexMatch> matches;
    std::vector<std::string> parts;
    std::string text;
    std::int64_t redactions = 0;
    std::size_t lastIndex = 0;
  };

  json::value describeMatch(const rt::RegexMatch& match) {
    json::array captures;
    for (const auto& capture : match.captures) captures.push_back(capture ? json::value(*capture) : json::value(nullptr));
    return json::object{{"index", match.index}, {"end", match.endIndex()}, {"captures", std::move(captures)}};
  }

  json::value describe(const Outcome& result, std::string_view operation) {
    if (operation == "redact") return json::object{{"text", result.text}, {"redactions", result.redactions}};
    if (operation == "test") return json::object{{"test", *result.test}, {"lastIndex", result.lastIndex}};
    if (operation == "exec") return json::object{{"match", result.match ? describeMatch(*result.match) : json::value(nullptr)}, {"lastIndex", result.lastIndex}};
    if (operation == "replace") return json::object{{"text", result.text}, {"lastIndex", result.lastIndex}};
    if (operation == "split") {
      json::array parts;
      for (const auto& part : result.parts) parts.emplace_back(part);
      return json::object{{"parts", std::move(parts)}, {"lastIndex", result.lastIndex}};
    }
    json::array matches;
    for (const auto& match : result.matches) matches.push_back(describeMatch(match));
    return json::object{{"matches", std::move(matches)}, {"lastIndex", result.lastIndex}};
  }

  class Workload {
  public:
    explicit Workload(const json::object& fixture)
        : operation_(rt::toString(fixture.at("operation"))), input_(rt::toString(fixture.at("input"))),
          regex_(fixture.if_contains("pattern") ? rt::toString(fixture.at("pattern")) : "", fixture.if_contains("flags") ? rt::toString(fixture.at("flags")) : ""),
          initialIndex_(fixture.if_contains("lastIndex") ? static_cast<std::size_t>(rt::toNumber(fixture.at("lastIndex"))) : 0) {}

    Outcome run() {
      Outcome result;
      regex_.lastIndex = initialIndex_;
      if (operation_ == "redact") {
        auto redacted = uos::codex::supervisor_secret::redactSupervisorSecrets(input_);
        result.text = std::move(redacted.text);
        result.redactions = redacted.redactions;
      } else if (operation_ == "test") result.test = regex_.test(input_);
      else if (operation_ == "exec") result.match = regex_.exec(input_);
      else if (operation_ == "matchAll") result.matches = rt::matchAll(input_, regex_);
      else if (operation_ == "replace") result.text = rt::replace(input_, regex_, "[redacted]");
      else if (operation_ == "split") result.parts = rt::split(input_, regex_);
      else throw std::runtime_error("Unknown benchmark operation");
      result.lastIndex = regex_.lastIndex;
      return result;
    }

    const std::string& operation() const { return operation_; }

  private:
    std::string operation_;
    std::string input_;
    rt::Regex regex_;
    std::size_t initialIndex_;
  };
}

int main(int argc, char** argv) {
  try {
    if (argc != 6) throw std::runtime_error("Usage: regex_bench manifest case samples warmups iterations");
    std::ifstream file(argv[1]);
    if (!file) throw std::runtime_error("Cannot open benchmark manifest");
    const std::string raw{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    const auto manifest = rt::jsonParse(raw);
    const json::object* fixture = nullptr;
    for (const auto& value : manifest.as_array())
      if (value.as_object().at("name").as_string() == argv[2]) fixture = &value.as_object();
    if (!fixture) throw std::runtime_error("Unknown benchmark case");
    const int samples = std::stoi(argv[3]), warmups = std::stoi(argv[4]), iterations = std::stoi(argv[5]);
    if (samples < 1 || samples > 100 || warmups < 0 || warmups > 100 || iterations < 1 || iterations > 100000)
      throw std::runtime_error("Benchmark bounds exceeded");
    Workload workload(*fixture);
    Outcome result;
    for (int warmup = 0; warmup < warmups; ++warmup)
      for (int iteration = 0; iteration < iterations; ++iteration) result = workload.run();
    json::array times;
    for (int sample = 0; sample < samples; ++sample) {
      const auto started = std::chrono::steady_clock::now();
      for (int iteration = 0; iteration < iterations; ++iteration) result = workload.run();
      const auto elapsed = std::chrono::steady_clock::now() - started;
      times.emplace_back(std::chrono::duration<double, std::milli>(elapsed).count() / iterations);
    }
    std::cout << rt::jsonStringify(json::object{{"case", argv[2]}, {"samples_ms", std::move(times)}, {"result", describe(result, workload.operation())}}) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
