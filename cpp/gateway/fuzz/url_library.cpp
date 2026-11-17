#include "fuzz/support.hpp"
#include "uos/rt/url.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  using namespace uos::rt;
  const auto input = uos::fuzz::text(data, size);
  const auto name = input.substr(0, size / 2);
  const auto value = input.substr(size / 2);

  uos::fuzz::malformed([&] {
    auto url = URL(input);
    (void)url.origin();
    (void)url.record();
    (void)fileURLToPath(url);
  });
  uos::fuzz::malformed([&] {
    URL url("https://fixture.invalid/base/path?a=1");
    auto& params = url.searchParams();
    url.host(input);
    url.hostname(input);
    url.port(input);
    url.username(input);
    url.password(input);
    url.pathname(input);
    url.hash(input);
    url.search(input);
    params.set(name, value);
    params.sort();
    params.delete_(name, value);
    url.protocol(input);
    (void)url.href();
    url.href(input);
    (void)params.entries();
  });
  uos::fuzz::malformed([&] {
    const auto relative = URL::parse(input, "https://fixture.invalid/base/path?a=1");
    if (relative) (void)relative->searchParams().toString();
    (void)URL::canParse(input);
    (void)URL::canParse(value, name);
    (void)URL::parse(value, name);
    (void)fileURLToPath(pathToFileURL(input));
  });
  uos::fuzz::malformed([&] {
    URLSearchParams params(input);
    params.append(name, value);
    params.sort();
    (void)params.getAll(name);
    (void)params.toString();
  });
  return 0;
}
