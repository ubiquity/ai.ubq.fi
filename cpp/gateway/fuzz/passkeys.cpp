// The production CBOR, BER and X.509 parsers are private in this one source TU.
#include "src/auth/passkeys.cpp"
#include "fuzz/support.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > 4096) return 0;
  const std::span<const std::uint8_t> bytes(data + 1, size - 1);
  uos::fuzz::malformed([&] {
    switch (data[0] % 3) {
      case 0: (void)uos::auth::passkeys::isoCBOR::decodeFirst(bytes); break;
      case 1: (void)uos::auth::passkeys::asn1::fromBER(bytes); break;
      case 2: (void)uos::auth::passkeys::parseCertificate(bytes); break;
    }
  });
  return 0;
}
