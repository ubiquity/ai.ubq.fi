// Links: tests/legacy/passkeys_asn1_library_test.cpp src/auth/relay.cpp src/http.cpp src/request.cpp src/utils.cpp src/kv.cpp
//   src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
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

TEST("DER certificate parsing admits the byte ceiling and rejects the next byte") {
  const auto leaf = rt::atobBytes(packedLeaf);
  auto paddedCertificate = [&](std::size_t padding) {
    const auto parsed = passkeys::parseCertificate(leaf);
    const Bytes contents(padding, 'a');
    const passkeys::asn1::Ptr<ASN1_OCTET_STRING, ASN1_OCTET_STRING_free> value(ASN1_OCTET_STRING_new());
    ASSERT_EQ(ASN1_OCTET_STRING_set(value.get(), contents.data(), static_cast<int>(contents.size())), 1);
    const passkeys::asn1::Ptr<X509_EXTENSION, X509_EXTENSION_free> extension(
        X509_EXTENSION_create_by_NID(nullptr, NID_netscape_comment, 0, value.get()));
    ASSERT(extension);
    ASSERT_EQ(X509_add_ext(parsed.x509.get(), extension.get(), -1), 1);
    ASSERT(i2d_re_X509_tbs(parsed.x509.get(), nullptr) > 0);
    const int length = i2d_X509(parsed.x509.get(), nullptr);
    ASSERT(length > 0);
    Bytes der(static_cast<std::size_t>(length));
    unsigned char* cursor = der.data();
    ASSERT_EQ(i2d_X509(parsed.x509.get(), &cursor), length);
    return der;
  };
  constexpr auto limit = rt::limits::kCertificateBytes;
  const auto initialPadding = limit - leaf.size() - 64;
  const auto sample = paddedCertificate(initialPadding);
  ASSERT(sample.size() < limit);
  const auto padding = initialPadding + limit - sample.size();
  const auto atLimit = paddedCertificate(padding);
  ASSERT_EQ(atLimit.size(), limit);
  EXPECT(passkeys::parseCertificate(atLimit).x509);
  const auto overLimit = paddedCertificate(padding + 1);
  ASSERT_EQ(overLimit.size(), limit + 1);
  EXPECT_THROWS_MATCHING(passkeys::parseCertificate(overLimit), "certificate exceeds limit");
}

TEST("attestation chains budget every decoded certificate before parsing the leaf") {
  constexpr auto certificate = rt::limits::kCertificateBytes;
  constexpr auto chainBytes = rt::limits::kCertificateChainBytes;
  passkeys::Cbor::Array values;
  for (std::size_t size = 0; size < chainBytes; size += certificate) values.push_back(passkeys::bytesCbor(Bytes(certificate, 'a')));
  passkeys::Cbor chain{values};
  EXPECT_EQ(passkeys::firstCertificate(&chain).size(), certificate);
  values.push_back(passkeys::bytesCbor(Bytes{0}));
  const passkeys::Cbor extra{std::move(values)};
  EXPECT_THROWS_MATCHING(passkeys::firstCertificate(&extra), "certificate chain exceeds limit");
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

TEST("name projection rejects empty RDNs that OpenSSL drops from its entry list") {
  const auto empty = passkeys::asn1::decode<X509_NAME_free>(rt::fromHex("3000"), d2i_X509_NAME, "X.509 Name");
  EXPECT(passkeys::parseName(empty.get()).empty());
  const auto emptyRdn = passkeys::asn1::decode<X509_NAME_free>(rt::fromHex("30023100"), d2i_X509_NAME, "X.509 Name");
  EXPECT_EQ(X509_NAME_entry_count(emptyRdn.get()), 0);
  EXPECT_THROWS(passkeys::parseName(emptyRdn.get()), rt::JsError);
}

TEST("revocation extensions reject constructed values with implicit string tags") {
  passkeys::Certificate certificate{};
  certificate.x509.reset(X509_new(), X509_free);
  ASSERT(certificate.x509);
  const auto addExtension = [&](int nid, std::string_view hex) {
    const Bytes bytes = rt::fromHex(hex);
    const passkeys::asn1::Ptr<ASN1_OCTET_STRING, ASN1_OCTET_STRING_free> data(ASN1_OCTET_STRING_new());
    ASSERT(data);
    ASSERT_EQ(ASN1_OCTET_STRING_set(data.get(), bytes.data(), static_cast<int>(bytes.size())), 1);
    const passkeys::asn1::Ptr<X509_EXTENSION, X509_EXTENSION_free> extension(X509_EXTENSION_create_by_NID(nullptr, nid, 0, data.get()));
    ASSERT(extension);
    ASSERT_EQ(X509_add_ext(certificate.x509.get(), extension.get(), -1), 1);
  };

  addExtension(NID_authority_key_identifier, "3005a003040161");
  EXPECT_THROWS((passkeys::certificateExtension<AUTHORITY_KEYID, AUTHORITY_KEYID_free>(
                  certificate, NID_authority_key_identifier, "Authority Key Identifier")),
                rt::JsError);

  addExtension(NID_crl_distribution_points, "300b3009a007a005a603160161");
  EXPECT_THROWS(passkeys::crlDistributionPointUrl(certificate), rt::JsError);
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
      Bytes bytes = rt::fromBase64(androidCrl);
      bytes.push_back(0);
      rt::ResponseInit init;
      init.status = 404;
      return rt::Response(rt::BodyInit(std::move(bytes)), init);
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
