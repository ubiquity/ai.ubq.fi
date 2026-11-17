#include "uos/kv_conversion/proof.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/process.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
  uos::rt::process::setArgv(argc, argv);
  uos::rt::installHttpClientTransport();
  try {
    uos::rt::blockOn(uos::kv_conversion::proof::importMetaMain());
    return uos::rt::process::exitStatus();
  } catch (...) {
    const uos::rt::UncaughtError error(std::current_exception(), false);
    std::fprintf(stderr, "%s\n", error.report().c_str());
    return 1;
  }
}
