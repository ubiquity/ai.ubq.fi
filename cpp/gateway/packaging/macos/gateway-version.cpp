#include <cstdio>
#include <string_view>

int uosGatewayMain(int argc, char** argv);

int main(int argc, char** argv) {
  if (argc == 2 && std::string_view(argv[1]) == "--version") {
    return std::puts("uos-gateway " UOS_PACKAGE_GIT_COMMIT) < 0 ? 1 : 0;
  }
  return uosGatewayMain(argc, argv);
}
