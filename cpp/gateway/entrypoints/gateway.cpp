#include "uos/rt/async.hpp"
#include "uos/rt/fetch_transport.hpp"
#include "uos/rt/process.hpp"
#include "runtime/startup.hpp"

#include <cstdio>
#include <exception>
#include <utility>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

int main(int argc, char** argv) {
#if defined(__GLIBC__)
  // Keep glibc's initial mmap threshold. Freeing catalog-sized mappings
  // would otherwise raise it and send later large allocations into arenas.
  (void)::mallopt(M_MMAP_THRESHOLD, 128 * 1024);
#endif
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
