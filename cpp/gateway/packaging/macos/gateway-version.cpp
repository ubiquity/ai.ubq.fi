#include "uos/release.hpp"

#include <cstdio>
#include <string_view>

int uosGatewayMain(int argc, char** argv);

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    const auto commit = uos::release::RELEASE_GIT_SHA;
    return std::printf("uos-gateway %.*s\n", static_cast<int>(commit.size()), commit.data()) < 0 ? 1 : 0;
  }
  return uosGatewayMain(argc, argv);
}
