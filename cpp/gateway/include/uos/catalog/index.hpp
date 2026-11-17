#pragma once
// Port of src/catalog/index.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include <functional>
#include <optional>
#include <string>
namespace uos::catalog::index {
  struct CodexCatalogDependencies {
    std::function<double()> now{};
    std::function<rt::Task<bool>(double)> recordSentinelDegradation{};
  };
  rt::Task<rt::Response> handleCodexCatalogModels(rt::Request req, std::string rawVersion);
  rt::Task<rt::Response> handleCodexCatalogModels(rt::Request req, std::string rawVersion, CodexCatalogDependencies dependencies);
  std::optional<std::string> getCatalogClientVersion(const rt::Request& req);
}
