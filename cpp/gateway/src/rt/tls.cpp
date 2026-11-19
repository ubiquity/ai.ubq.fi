// OpenSSL client pieces shared by rt::ws and the fetch transport (see
// tls_internal.hpp): the trust store, Bun's certificate verification texts and
// Node's checkServerIdentity.
#include "tls_internal.hpp"

#include "uos/rt/env.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/string.hpp"

#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>

namespace uos::rt::detail::tls {
  namespace {
    std::optional<std::string> readFile(const std::string& path) {
      std::ifstream file(path, std::ios::binary | std::ios::ate);
      if (!file) return std::nullopt;
      const auto size = static_cast<std::streamoff>(file.tellg());
      if (size < 0 || size > static_cast<std::streamoff>(limits::kTrustStoreBytes)) return std::nullopt;
      std::string pem(static_cast<std::size_t>(size), '\0');
      if (!file.seekg(0) || !file.read(pem.data(), size)) return std::nullopt;
      return pem;
    }

    std::string asn1Text(const ASN1_STRING* value) {
      return std::string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(value)), static_cast<std::size_t>(ASN1_STRING_length(value)));
    }

    std::string formatIp(const ASN1_OCTET_STRING* value) {
      const unsigned char* data = ASN1_STRING_get0_data(value);
      const int length = ASN1_STRING_length(value);
      char text[64] = {0};
      if (length == 4) {
        std::snprintf(text, sizeof text, "%d.%d.%d.%d", data[0], data[1], data[2], data[3]);
      } else if (length == 16) {
        std::string out;
        for (int index = 0; index < 16; index += 2) {
          char group[8];
          std::snprintf(group, sizeof group, "%X", (data[index] << 8) | data[index + 1]);
          out += (index ? ":" : "") + std::string(group);
        }
        return out;
      } else {
        return "<invalid>";
      }
      return text;
    }

    std::string unfqdn(std::string host) {
      if (!host.empty() && host.back() == '.') host.pop_back();
      return host;
    }

    std::vector<std::string> splitHost(const std::string& host) {
      std::string lowered = unfqdn(host);
      for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
      }
      return rt::split(lowered, ".");
    }

    // tls.js `check(hostParts, pattern, wildcards)`.
    bool check(const std::vector<std::string>& hostParts, const std::string& pattern, bool wildcards) {
      if (pattern.empty()) return false;
      const std::vector<std::string> patternParts = splitHost(pattern);
      if (hostParts.size() != patternParts.size()) return false;
      for (const std::string& part : patternParts) {
        if (part.empty()) return false;
        for (const char byte : part) {
          const auto c = static_cast<unsigned char>(byte);
          if (c < 0x21 || c > 0x7f) return false;
        }
      }
      for (std::size_t index = hostParts.size() - 1; index > 0; --index) {
        if (hostParts[index] != patternParts[index]) return false;
      }
      const std::string& hostSubdomain = hostParts[0];
      const std::string& patternSubdomain = patternParts[0];
      const std::vector<std::string> patternSubdomainParts = rt::split(patternSubdomain, "*");
      if (patternSubdomainParts.size() == 1 || patternSubdomain.find("xn--") != std::string::npos) return hostSubdomain == patternSubdomain;
      if (!wildcards) return false;
      if (patternSubdomainParts.size() > 2) return false;
      if (patternParts.size() <= 2) return false;
      const std::string& prefix = patternSubdomainParts[0];
      const std::string& suffix = patternSubdomainParts[1];
      if (prefix.size() + suffix.size() > hostSubdomain.size()) return false;
      if (hostSubdomain.compare(0, prefix.size(), prefix) != 0) return false;
      return hostSubdomain.compare(hostSubdomain.size() - suffix.size(), suffix.size(), suffix) == 0;
    }
  }

  void addPemCertificates(X509_STORE* store, const std::string& pem) {
    if (pem.size() > limits::kTrustStoreBytes) return;
    const std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())), BIO_free);
    if (!bio) return;
    for (;;) {
      unsigned char* bytes = nullptr;
      long size = 0;
      if (PEM_bytes_read_bio(&bytes, &size, nullptr, PEM_STRING_X509, bio.get(), nullptr, nullptr) != 1) break;
      const unsigned char* cursor = bytes;
      X509* certificate = size > 0 && size <= static_cast<long>(limits::kCertificateBytes) ? d2i_X509(nullptr, &cursor, size) : nullptr;
      OPENSSL_free(bytes);
      if (!certificate) break;
      X509_STORE_add_cert(store, certificate);
      X509_free(certificate);
    }
    ERR_clear_error();
  }

  SSL_CTX* newContext() {
    SSL_CTX* context = SSL_CTX_new(TLS_client_method());
    if (!context) throw std::runtime_error("SSL_CTX_new failed");
    SSL_CTX_set_min_proto_version(context, TLS1_2_VERSION);
    SSL_CTX_set_options(context, SSL_OP_IGNORE_UNEXPECTED_EOF);
    // OpenSSL checks the Certificate message length before it decodes any DER.
    // Its pre-parse control is aggregate, so use the per-certificate cap here.
    constexpr long certListBytes = static_cast<long>(std::min(limits::kCertificateBytes, limits::kCertificateChainBytes));
    SSL_CTX_set_max_cert_list(context, std::min(SSL_CTX_get_max_cert_list(context), certListBytes));
    // The chain is verified during the handshake and judged afterwards, as
    // Node does (verifyError() then checkServerIdentity).
    SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
    return context;
  }

  // Bun trusts its bundled Mozilla roots; the system store (OpenSSL's default
  // paths, else the usual bundle files) holds the same list. NODE_EXTRA_CA_CERTS
  // adds certificates, as in Bun.
  SSL_CTX* defaultContext() {
    static SSL_CTX* const context = [] {
      SSL_CTX* created = newContext();
      SSL_CTX_set_default_verify_paths(created);
      X509_STORE* store = SSL_CTX_get_cert_store(created);
      if (!env::get("SSL_CERT_FILE")) {
        for (const char* path : {"/etc/ssl/certs/ca-certificates.crt", "/etc/pki/tls/certs/ca-bundle.crt", "/etc/ssl/cert.pem"}) {
          if (const auto pem = readFile(path)) {
            addPemCertificates(store, *pem);
            break;
          }
        }
      }
      if (const auto extra = env::get("NODE_EXTRA_CA_CERTS")) {
        if (const auto pem = readFile(*extra)) addPemCertificates(store, *pem);
      }
      ERR_clear_error();
      return created;
    }();
    return context;
  }

  // X509 verify results with the reason and code Bun (BoringSSL) reports.
  std::pair<std::string, std::string> verifyErrorText(long result) {
    switch (result) {
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT: return {"unable to get issuer certificate", "UNABLE_TO_GET_ISSUER_CERT"};
      case X509_V_ERR_UNABLE_TO_GET_CRL: return {"unable to get certificate CRL", "UNABLE_TO_GET_CRL"};
      case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE: return {"unable to decrypt certificate's signature", "UNABLE_TO_DECRYPT_CERT_SIGNATURE"};
      case X509_V_ERR_UNABLE_TO_DECRYPT_CRL_SIGNATURE: return {"unable to decrypt CRL's signature", "UNABLE_TO_DECRYPT_CRL_SIGNATURE"};
      case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY: return {"unable to decode issuer public key", "UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY"};
      case X509_V_ERR_CERT_SIGNATURE_FAILURE: return {"certificate signature failure", "CERT_SIGNATURE_FAILURE"};
      case X509_V_ERR_CRL_SIGNATURE_FAILURE: return {"CRL signature failure", "CRL_SIGNATURE_FAILURE"};
      case X509_V_ERR_CERT_NOT_YET_VALID: return {"certificate is not yet valid", "CERT_NOT_YET_VALID"};
      case X509_V_ERR_CERT_HAS_EXPIRED: return {"certificate has expired", "CERT_HAS_EXPIRED"};
      case X509_V_ERR_CRL_NOT_YET_VALID: return {"CRL is not yet valid", "CRL_NOT_YET_VALID"};
      case X509_V_ERR_CRL_HAS_EXPIRED: return {"CRL has expired", "CRL_HAS_EXPIRED"};
      case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD: return {"format error in certificate's notBefore field", "ERROR_IN_CERT_NOT_BEFORE_FIELD"};
      case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD: return {"format error in certificate's notAfter field", "ERROR_IN_CERT_NOT_AFTER_FIELD"};
      case X509_V_ERR_ERROR_IN_CRL_LAST_UPDATE_FIELD: return {"format error in CRL's lastUpdate field", "ERROR_IN_CRL_LAST_UPDATE_FIELD"};
      case X509_V_ERR_ERROR_IN_CRL_NEXT_UPDATE_FIELD: return {"format error in CRL's nextUpdate field", "ERROR_IN_CRL_NEXT_UPDATE_FIELD"};
      case X509_V_ERR_OUT_OF_MEM: return {"out of memory", "OUT_OF_MEM"};
      case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT: return {"self signed certificate", "DEPTH_ZERO_SELF_SIGNED_CERT"};
      case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN: return {"self signed certificate in certificate chain", "SELF_SIGNED_CERT_IN_CHAIN"};
      case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY: return {"unable to get local issuer certificate", "UNABLE_TO_GET_ISSUER_CERT_LOCALLY"};
      case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE: return {"unable to verify the first certificate", "UNABLE_TO_VERIFY_LEAF_SIGNATURE"};
      case X509_V_ERR_CERT_CHAIN_TOO_LONG: return {"certificate chain too long", "CERT_CHAIN_TOO_LONG"};
      case X509_V_ERR_CERT_REVOKED: return {"certificate revoked", "CERT_REVOKED"};
      case X509_V_ERR_INVALID_CA: return {"invalid CA certificate", "INVALID_CA"};
      case X509_V_ERR_PATH_LENGTH_EXCEEDED: return {"path length constraint exceeded", "PATH_LENGTH_EXCEEDED"};
      case X509_V_ERR_INVALID_PURPOSE: return {"unsupported certificate purpose", "INVALID_PURPOSE"};
      case X509_V_ERR_CERT_UNTRUSTED: return {"certificate not trusted", "CERT_UNTRUSTED"};
      case X509_V_ERR_CERT_REJECTED: return {"certificate rejected", "CERT_REJECTED"};
      default: {
        const char* text = X509_verify_cert_error_string(result);
        return {text ? text : "certificate verification failed", "UNSPECIFIED"};
      }
    }
  }

  // `net.isIP(host)` and the address bytes.
  std::optional<std::vector<std::uint8_t>> ipBytes(const std::string& text) {
    unsigned char buffer[16];
    if (inet_pton(AF_INET, text.c_str(), buffer) == 1) return std::vector<std::uint8_t>(buffer, buffer + 4);
    if (inet_pton(AF_INET6, text.c_str(), buffer) == 1) return std::vector<std::uint8_t>(buffer, buffer + 16);
    return std::nullopt;
  }

  namespace {
    // `canonicalizeIP(ip)`.
    std::string canonicalIp(const std::string& text) {
      const auto bytes = ipBytes(text);
      if (!bytes) return text;
      char out[INET6_ADDRSTRLEN] = {0};
      inet_ntop(bytes->size() == 4 ? AF_INET : AF_INET6, bytes->data(), out, sizeof out);
      return out;
    }
  }

  // tls.checkServerIdentity(hostname, cert): the reason when it fails.
  std::optional<std::string> checkServerIdentity(const std::string& requested, X509* certificate) {
    std::vector<std::string> dnsNames;
    std::vector<std::string> ips;
    std::string altNames;
    if (auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr))) {
      for (int index = 0; index < sk_GENERAL_NAME_num(names); ++index) {
        const GENERAL_NAME* name = sk_GENERAL_NAME_value(names, index);
        std::string entry;
        if (name->type == GEN_DNS) {
          entry = "DNS:" + asn1Text(name->d.dNSName);
          dnsNames.push_back(entry.substr(4));
        } else if (name->type == GEN_IPADD) {
          entry = "IP Address:" + formatIp(name->d.iPAddress);
          ips.push_back(canonicalIp(entry.substr(11)));
        } else if (name->type == GEN_EMAIL) {
          entry = "email:" + asn1Text(name->d.rfc822Name);
        } else if (name->type == GEN_URI) {
          entry = "URI:" + asn1Text(name->d.uniformResourceIdentifier);
        } else {
          continue;
        }
        altNames += (altNames.empty() ? "" : ", ") + entry;
      }
      GENERAL_NAMES_free(names);
    }
    std::optional<std::string> commonName;
    if (X509_NAME* subject = X509_get_subject_name(certificate)) {
      const int position = X509_NAME_get_index_by_NID(subject, NID_commonName, -1);
      if (position >= 0) commonName = asn1Text(X509_NAME_ENTRY_get_data(X509_NAME_get_entry(subject, position)));
    }
    const std::string hostname = unfqdn(requested);
    if (ipBytes(hostname)) {
      const std::string canonical = canonicalIp(hostname);
      for (const std::string& ip : ips) {
        if (ip == canonical) return std::nullopt;
      }
      std::string list;
      for (std::size_t index = 0; index < ips.size(); ++index) list += (index ? ", " : "") + ips[index];
      return "IP: " + hostname + " is not in the cert's list: " + list;
    }
    if (!dnsNames.empty() || (commonName && !commonName->empty())) {
      const std::vector<std::string> hostParts = splitHost(hostname);
      if (!dnsNames.empty()) {
        for (const std::string& pattern : dnsNames) {
          if (check(hostParts, pattern, true)) return std::nullopt;
        }
        return "Host: " + hostname + ". is not in the cert's altnames: " + altNames;
      }
      if (check(hostParts, *commonName, true)) return std::nullopt;
      return "Host: " + hostname + ". is not cert's CN: " + *commonName;
    }
    return std::string("Cert does not contain a DNS name");
  }
}
