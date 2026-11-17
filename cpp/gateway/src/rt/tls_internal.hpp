#pragma once
// OpenSSL client pieces shared by the rt TLS clients (rt::ws and the fetch
// transport): the trust store Bun uses (system roots plus NODE_EXTRA_CA_CERTS),
// the certificate verification texts Bun reports, and Node's
// checkServerIdentity.
#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::rt::detail::tls {
  // A client context: TLS 1.2 or later, unexpected EOF read as a close, and no
  // verification during the handshake (the chain is judged afterwards, as Node
  // and Bun do). The caller owns it.
  SSL_CTX* newContext();
  // The process-wide client context trusting the system store (OpenSSL's
  // default paths, else the usual bundle files) plus NODE_EXTRA_CA_CERTS. Never
  // freed; take a reference with SSL_CTX_up_ref.
  SSL_CTX* defaultContext();
  // Adds every certificate of a PEM bundle to `store`.
  void addPemCertificates(X509_STORE* store, const std::string& pem);
  // An X509 verify result as Bun (BoringSSL) reports it: {reason, code}, e.g.
  // {"self signed certificate", "DEPTH_ZERO_SELF_SIGNED_CERT"}.
  std::pair<std::string, std::string> verifyErrorText(long result);
  // `net.isIP(host)` and the address bytes (4 or 16).
  std::optional<std::vector<std::uint8_t>> ipBytes(const std::string& text);
  // `tls.checkServerIdentity(hostname, cert)`: the reason text when the
  // certificate does not cover `hostname` (DNS names with wildcards, IP
  // addresses, the CN when there are no DNS names).
  std::optional<std::string> checkServerIdentity(const std::string& hostname, X509* certificate);
}
