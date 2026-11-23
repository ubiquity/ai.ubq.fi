#include "fuzz/support.hpp"
#include "uos/rt/env.hpp"

#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace env = uos::rt::env;
  const auto input = uos::fuzz::text(data, size);
  const std::string name(input.substr(0, size / 2));
  const std::string value(input.substr(size / 2));
  const auto original = env::get(name);
  {
    const env::ScopedEnvironment snapshot({{name, value}});
    if (env::get(name) != value) std::abort();
    {
      const env::ScopedOverride hidden({{name, std::nullopt}});
      if (env::has(name)) std::abort();
    }
    if (env::get(name) != value) std::abort();
  }
  if (env::get(name) != original) std::abort();
  return 0;
}
