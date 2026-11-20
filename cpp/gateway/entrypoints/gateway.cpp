#include "uos/rt/async.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/process.hpp"
#include "runtime/startup.hpp"

#include <cstdio>
#include <exception>
#include <utility>

int main(int argc, char** argv) {
  uos::rt::process::setArgv(argc, argv);
  try {
    auto arguments = uos::runtime::startup::parseArguments(argc, argv);
    // run() completes the synchronous confinement bootstrap before returning its task.
    auto serving = uos::runtime::startup::run(std::move(arguments));
    uos::rt::installHttpClientTransport();
    uos::rt::loop().setUncaughtPolicy(uos::rt::UncaughtPolicy::report);
    uos::rt::blockOn(std::move(serving));
    return uos::rt::process::exitStatus();
  } catch (...) {
    const uos::rt::UncaughtError error(std::current_exception(), false);
    std::fprintf(stderr, "%s\n", error.report().c_str());
    return 1;
  }
}
