// The production attestation decoders are private to the passkeys translation unit.
#include "src/auth/passkeys.cpp"
#include "fuzz/support.hpp"

namespace passkeys = uos::auth::passkeys;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
  if (size == 0 || size > 4096) return 0;
  const std::span<const std::uint8_t> bytes(data + 1, size - 1);
  uos::fuzz::malformed([&] {
    switch (data[0] % 7) {
      case 0: (void)passkeys::parseEcdsaSigValue(bytes); break;
      case 1: (void)passkeys::parseOctetString(bytes); break;
      case 2: {
        const auto cert = passkeys::getCertificateInfo(bytes).parsedCertificate;
        (void)passkeys::certificateExtension<AUTHORITY_KEYID, AUTHORITY_KEYID_free>(cert, NID_authority_key_identifier, "Authority Key Identifier");
        (void)passkeys::crlDistributionPointUrl(cert);
        (void)passkeys::validateExtFIDOGenCEAAGUID(cert, {});
        break;
      }
      case 3: (void)passkeys::parseCrl(bytes); break;
      case 4: {
        const auto key = passkeys::parseAndroidKeyDescription(bytes);
        (void)passkeys::hasAllApplications(key->softwareEnforced);
        (void)passkeys::hasAllApplications(key->teeEnforced);
        break;
      }
      case 5: (void)passkeys::parseAppleNonce(bytes); break;
      case 6: (void)passkeys::convertX509PublicKeyToCOSE(bytes); break;
    }
  });
  return 0;
}
