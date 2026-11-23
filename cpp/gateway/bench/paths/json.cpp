// A bounded hot-path probe of the production rt JSON implementation, not boost::json alone.
#include "uos/rt/json.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
  std::uint64_t checksum(std::string_view text) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char byte : text) {
      value ^= byte;
      value *= 1099511628211ULL;
    }
    return value;
  }

  int bounded(const char* text, int maximum) {
    const std::string source(text);
    std::size_t used = 0;
    const long value = std::stol(source, &used);
    if (used != source.size() || value < 1 || value > maximum) throw std::runtime_error("Invalid benchmark bound");
    return static_cast<int>(value);
  }
}

int main(int argc, char** argv) {
  try {
    if (argc != 5) throw std::runtime_error("usage: uos-json-bench input.json parse|stringify iterations samples");
    const std::string operation(argv[2]);
    if (operation != "parse" && operation != "stringify" && operation != "bytes") throw std::runtime_error("Unknown benchmark operation");
    const int iterations = bounded(argv[3], 200000);
    const int samples = bounded(argv[4], 31);
    std::ifstream input(argv[1], std::ios::binary);
    if (!input) throw std::runtime_error("Cannot open benchmark input");
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.empty() || text.size() > 2 * 1024 * 1024) throw std::runtime_error("Benchmark input exceeds bound");
    const auto coldParseStarted = std::chrono::steady_clock::now();
    const auto initial = uos::rt::jsonParse(text);
    const auto coldParseFinished = std::chrono::steady_clock::now();
    const std::string canonical = uos::rt::jsonStringify(initial);
    const auto coldStringifyFinished = std::chrono::steady_clock::now();
    if (operation == "bytes") {
      if (std::fwrite(canonical.data(), 1, canonical.size(), stdout) != canonical.size()) throw std::runtime_error("Cannot write canonical JSON bytes");
      return 0;
    }
    const auto coldElapsed = operation == "parse" ? coldParseFinished - coldParseStarted : coldStringifyFinished - coldParseFinished;
    const double coldOperationUs = std::chrono::duration<double, std::micro>(coldElapsed).count();
    std::uint64_t consumed = 0;
    std::vector<double> elapsed;
    elapsed.reserve(static_cast<std::size_t>(samples));
    for (int sample = 0; sample < samples; ++sample) {
      const auto started = std::chrono::steady_clock::now();
      for (int iteration = 0; iteration < iterations; ++iteration) {
        if (operation == "parse") {
          auto value = uos::rt::jsonParse(text);
          consumed += value.is_object() ? value.as_object().size() : 1;
        } else {
          const auto serialized = uos::rt::jsonStringify(initial);
          consumed += serialized.size();
        }
      }
      const auto finished = std::chrono::steady_clock::now();
      elapsed.push_back(std::chrono::duration<double, std::micro>(finished - started).count());
    }
    std::printf("{\"runtime\":\"native\",\"operation\":\"%s\",\"input_bytes\":%zu,\"canonical_bytes\":%zu,"
                "\"canonical_checksum\":\"%llu\",\"iterations\":%d,\"cold_operation_us\":%.3f,\"samples_us\":[",
                operation.c_str(), text.size(), canonical.size(), static_cast<unsigned long long>(checksum(canonical)), iterations, coldOperationUs);
    for (std::size_t index = 0; index < elapsed.size(); ++index) {
      std::printf("%s%.3f", index == 0 ? "" : ",", elapsed[index]);
    }
    std::printf("],\"consumed\":\"%llu\"}\n", static_cast<unsigned long long>(consumed));
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
  }
}
