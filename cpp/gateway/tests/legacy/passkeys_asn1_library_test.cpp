// Links: tests/legacy/passkeys_asn1_library_test.cpp src/auth/relay.cpp src/http.cpp src/request.cpp src/utils.cpp src/kv.cpp
//   src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/native_addon.cpp
//   src/config.cpp src/provider/cerebras_rate_limits.cpp
#if defined(__GNUC__) && !defined(__clang__)
// GCC diagnoses TU-local coroutine fields when their source is textually included.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsubobject-linkage"
#endif
#include "src/auth/passkeys.cpp"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "uos/rt/test_async.hpp"

namespace passkeys = uos::auth::passkeys;
namespace rt = uos::rt;
using passkeys::Bytes;

namespace {
  // DER inputs from the packed and Android Key WebAuthn replay vectors.
  constexpr std::string_view packedLeaf =
    "MIIBtzCCAV2gAwIBAgICAQIwCgYIKoZIzj0EAwIwMDESMBAGA1UEAxMJVGVzdCBSb290MQ0wCwYDVQQKEwRUZXN0MQswCQYDVQQGEwJVUzAeFw0y"
    "MDAxMDEwMDAwMDBaFw00OTAxMDEwMDAwMDBaMGQxCzAJBgNVBAYTAlVTMRQwEgYDVQQKEwtUZXN0IFZlbmRvcjEiMCAGA1UECxMZQXV0aGVudGlj"
    "YXRvciBBdHRlc3RhdGlvbjEbMBkGA1UEAxMSVGVzdCBBdXRoZW50aWNhdG9yMFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEb0i3uTS26BWVDmZE"
    "gPYymx+UwP8qComi3wbl3PksKwgK05HmlKzO3xZZuNoDp0oWFHzSQ0Uc4ZdoFpOG9LimkaMzMDEwDAYDVR0TAQH/BAIwADAhBgsrBgEEAYLlHAEB"
    "BAQSBBCtzgACNbzGCmSLCyXx8FUDMAoGCCqGSM49BAMCA0gAMEUCIQD2+Y6hPDSDaE58uUiJNUcfiWH8I/6vQQiS4a86qwMlTgIgeXt13xRFbzKK"
    "+4TGQagFLy2q6BBYr7J5i8D0u6T3UPM=";
  constexpr std::string_view androidLeaf =
    "MIIBzjCCAXSgAwIBAgIBDDAKBggqhkjOPQQDAjAXMRUwEwYDVQQDEwxGaXh0dXJlIFJvb3QwHhcNMjAwMTAxMDAwMDAwWhcNNDkwMTAxMDAwMDAw"
    "WjAfMR0wGwYDVQQDExRBbmRyb2lkIEtleXN0b3JlIEtleTBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABBeeUsBMycDH5cf175aNRNqiI8jCNjI9"
    "I9dDkGidsT70z2AE4W63d8ER3EfCaT/8mjxq7kN3uSlW+K3DJqLlpvyjgagwgaUwCQYDVR0TBAIwADAfBgNVHSMEGDAWgBRH02Yo8iWIrkOt4rCz"
    "HFIHeYYWezAxBgNVHR8EKjAoMCagJKAihiBodHRwOi8vY3JsLnRlc3QvYW5kcm9pZC1sZWFmLmNybDBEBgorBgEEAdZ5AgERBDYwNAIBAwoBAQIB"
    "BAoBAQQgooE+XVCWbkx3UencqwzDDmFY94M5JLh8/YGUqcNo744EADAAMAAwCgYIKoZIzj0EAwIDSAAwRQIhAM6j4xSpfgwMho+TbI/0xg28WJTV"
    "ERnmvY4D/7ZbmyeSAiBKvlz5yz1ObOwGhsJFwgRDIvb3z0Og45hjpqlGmNzPfQ==";
  constexpr std::string_view androidRoot =
    "MIIBgzCCASigAwIBAgIBATAKBggqhkjOPQQDAjAXMRUwEwYDVQQDEwxGaXh0dXJlIFJvb3QwHhcNMjAwMTAxMDAwMDAwWhcNNDkwMTAxMDAwMDAw"
    "WjAXMRUwEwYDVQQDEwxGaXh0dXJlIFJvb3QwWTATBgcqhkjOPQIBBggqhkjOPQMBBwNCAATFSEfhizLLI0w/Lo+c/YC7cPLi43Rfqq9xqB75J7Tq"
    "1NG8AYWIuw7eRAcPLFVV/jx9LX4c8/4zogEFb8Gq42Uuo2UwYzAPBgNVHRMBAf8EBTADAQH/MB0GA1UdDgQWBBRH02Yo8iWIrkOt4rCzHFIHeYYW"
    "ezAxBgNVHR8EKjAoMCagJKAihiBodHRwOi8vY3JsLnRlc3QvYW5kcm9pZC1yb290LmNybDAKBggqhkjOPQQDAgNJADBGAiEAqmWFVzpunZq0jJPd"
    "mtEdrSoEiH8ddHYoLJMFTxObyVICIQCoOUXxIU4RVFqPR5C98eKqsBglf0mVEQFHPJlRulweeA==";
  constexpr std::string_view androidCrl =
    "MIG3MF4CAQEwCgYIKoZIzj0EAwIwFzEVMBMGA1UEAxMMRml4dHVyZSBSb290Fw0yNjAxMDEwMDAwMDBaFw00ODAxMDEwMDAwMDBaMBYwFAIBfxcN"
    "MjYwMjAxMDAwMDAwWjAAMAoGCCqGSM49BAMCA0kAMEYCIQCpb/PulnXBSitdb/e7XBRDGLmKbUfoP4Xf7T/Carvd9AIhAMANX8yRG5QdwSfN1yu8"
    "f5P+XDpzQh/q/KHyorOMYaSi";
}

TEST("ECDSA decoding enforces canonical DER and positive scalars") {
  const auto signature = passkeys::parseEcdsaSigValue(rt::fromHex("300702020080020102"));
  EXPECT_EQ(BN_get_word(ECDSA_SIG_get0_r(signature.get())), 128UL);
  EXPECT_EQ(BN_get_word(ECDSA_SIG_get0_s(signature.get())), 2UL);
  for (const std::string_view malformed : {"", "3000", "a006020101020102", "30800201010201020000",
                                           "300602010102010200", "30060201ff020102", "3006020100020102",
                                           "300702020001020102", "308106020101020102", "3006020101"}) {
    EXPECT_THROWS(passkeys::parseEcdsaSigValue(rt::fromHex(malformed)), rt::JsError);
  }
}

TEST("OCTET STRING consumes one primitive value") {
  EXPECT_EQ(rt::toHex(passkeys::parseOctetString(rt::fromHex("0403616263"))), "616263");
  EXPECT(passkeys::parseOctetString(rt::fromHex("0400")).empty());
  for (const std::string_view malformed : {"04016100", "2403040161", "24800401610000", "04810161", "0500"}) {
    EXPECT_THROWS(passkeys::parseOctetString(rt::fromHex(malformed)), rt::JsError);
  }
}

TEST("private attestation extensions enforce their tags and challenge shape") {
  EXPECT_EQ(rt::toHex(passkeys::parseAppleNonce(rt::fromHex("3005a103040161"))), "61");
  EXPECT_THROWS(passkeys::parseAppleNonce(rt::fromHex("3005a003040161")), rt::JsError);
  EXPECT_THROWS(passkeys::parseAppleNonce(rt::fromHex("3005a10304016100")), rt::JsError);

  const auto key = passkeys::parseAndroidKeyDescription(rt::fromHex("301b0201030a01000201030a0100040161040030003006bf8458020500"));
  EXPECT_EQ(rt::toHex(passkeys::asn1::stringBytes(key->attestationChallenge)), "61");
  EXPECT(!passkeys::hasAllApplications(key->softwareEnforced));
  EXPECT(passkeys::hasAllApplications(key->teeEnforced));
  EXPECT_THROWS(passkeys::parseAndroidKeyDescription(rt::fromHex("30100201030201000201030a0100040004003000")), rt::JsError);
}

TEST("certificate fields and Basic Constraints criticality come from X.509") {
  Bytes bytes = rt::fromBase64(packedLeaf);
  const auto info = passkeys::getCertificateInfo(bytes);
  EXPECT_EQ(info.version, 2.0);
  EXPECT_EQ(info.subject.OU.value_or(""), "Authenticator Attestation");
  EXPECT_EQ(info.notBefore, 1'577'836'800'000.0);
  EXPECT_EQ(info.notAfter, 2'493'072'000'000.0);
  EXPECT(!info.basicConstraintsCA);
  X509* cert = info.parsedCertificate.x509.get();
  const int index = X509_get_ext_by_NID(cert, NID_basic_constraints, -1);
  ASSERT(index >= 0);
  EXPECT_EQ(X509_EXTENSION_get_critical(X509_get_ext(cert, index)), 1);
  EXPECT_EQ(passkeys::subjectPublicKey(info.parsedCertificate).size(), 65U);
  ASSERT_EQ(X509_add_ext(cert, X509_get_ext(cert, index), -1), 1);
  EXPECT_THROWS((passkeys::certificateExtension<BASIC_CONSTRAINTS, BASIC_CONSTRAINTS_free>(
                  info.parsedCertificate, NID_basic_constraints, "Basic Constraints")),
                rt::JsError);
  bytes.push_back(0);
  EXPECT_THROWS(passkeys::parseCertificate(bytes), rt::JsError);
}

TEST("certificate verification uses the decoded signing key and original signed data") {
  auto leafBytes = rt::fromBase64(androidLeaf);
  const auto leaf = passkeys::parseCertificate(leafBytes);
  const auto root = passkeys::parseCertificate(rt::fromBase64(androidRoot));
  EXPECT(passkeys::sameName(root));
  EXPECT(passkeys::verifyCertificateSignature(leaf, root));
  leafBytes.back() ^= 1;
  const auto modified = passkeys::parseCertificate(leafBytes);
  EXPECT(!passkeys::verifyCertificateSignature(modified, root));
}

TEST("CRLs consume the complete DER and use unsigned serial magnitudes") {
  Bytes bytes = rt::fromBase64(androidCrl);
  const auto crl = passkeys::parseCrl(bytes);
  EXPECT_EQ(crl.revokedCerts, std::vector<std::string>{"7f"});
  EXPECT_EQ(crl.nextUpdate.value_or(0), 2'461'449'600'000.0);
  bytes.push_back(0);
  EXPECT_THROWS(passkeys::parseCrl(bytes), rt::JsError);

  const auto serial = passkeys::asn1::decode<ASN1_INTEGER_free>(rt::fromHex("02020080"), d2i_ASN1_INTEGER, "X.509 serial");
  EXPECT_EQ(passkeys::serialHex(serial.get()), "80");
}

ASYNC_TEST("invalid published CRL fails the revocation check and is never cached") {
  const auto leaf = passkeys::parseCertificate(rt::fromBase64(androidLeaf));
  passkeys::cacheRevokedCerts().clear();
  {
    const rt::GlobalFetchOverride fetchMock([](rt::RequestInfo, rt::RequestInit) {
      return rt::Response(rt::BodyInit(std::string("malformed")));
    });
    auto check = passkeys::isCertRevoked(leaf);
    EXPECT_REJECTS(std::move(check), rt::JsError);
  }
  EXPECT(passkeys::cacheRevokedCerts().empty());
  {
    const rt::GlobalFetchOverride fetchMock([](rt::RequestInfo, rt::RequestInit) {
      rt::ResponseInit init;
      init.status = 404;
      return rt::Response(rt::BodyInit(std::string("missing")), init);
    });
    auto check = passkeys::isCertRevoked(leaf);
    const bool revoked = co_await std::move(check);
    EXPECT(!revoked);
  }
}
