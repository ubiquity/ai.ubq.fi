#include "src/auth/passkeys.cpp"
#include "fuzz/support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size > uos::fuzz::maxInput) return 0;
  namespace passkeys = uos::auth::passkeys;
  const std::span<const std::uint8_t> bytes(data, size);
  uos::fuzz::malformed([&] {
    const auto item = passkeys::isoCBOR::decodeFirst(bytes);
    const auto encoded = passkeys::isoCBOR::encode(item);
    (void)passkeys::isoCBOR::decodeFirst(encoded);
  });
  uos::fuzz::malformed([&] { (void)passkeys::parseAuthenticatorData(passkeys::Bytes(bytes.begin(), bytes.end())); });
  return 0;
}
