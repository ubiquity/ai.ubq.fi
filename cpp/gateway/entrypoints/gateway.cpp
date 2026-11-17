#include "uos/serve.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/process.hpp"
#include "runtime/startup.hpp"

#include <cstdio>
#include <exception>

int main(int argc, char** argv) {
  uos::rt::process::setArgv(argc, argv);
  uos::rt::installHttpClientTransport();
  uos::rt::loop().setUncaughtPolicy(uos::rt::UncaughtPolicy::report);
  try {
    const std::string manifest = uos::runtime::startup::manifestArgument(argc, argv);
    uos::rt::blockOn(uos::runtime::startup::run(manifest));
    return uos::rt::process::exitStatus();
  } catch (...) {
    const uos::rt::UncaughtError error(std::current_exception(), false);
    std::fprintf(stderr, "%s\n", error.report().c_str());
    return 1;
  }
}
