#include "uos/kv_conversion/cli.hpp"

#include "uos/kv_conversion/convert.hpp"
#include "uos/kv_conversion/cursors.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"

#include <exception>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace uos::kv_conversion::cli {
  namespace {
    rt::Task<void> main() {
      rt::OrderedMap<std::string, std::string> flags;
      const std::vector<std::string> argv = rt::process::argv();
      for (std::size_t index = 2; index < argv.size(); index += 2) {
        const std::string& key = argv[index];
        const std::optional<std::string> value = index + 1 < argv.size() ? std::optional<std::string>(argv[index + 1]) : std::nullopt;
        if (!rt::startsWith(key, "--") || !value || value->empty() || rt::startsWith(*value, "--") || flags.has(key))
          throw rt::JsError("Expected unique --flag value pairs");
        flags.set(key, *value);
      }
      const auto required = [&flags](const std::string& name) -> std::string {
        const std::string* value = flags.get("--" + name);
        if (value == nullptr || value->empty()) throw rt::JsError("--" + name + " is required");
        return *value;
      };
      const rt::AbortController signal;
      const rt::process::Listener interrupt = [signal] { signal.abort(rt::makeReason(rt::JsError("Conversion interrupted before publication"))); };
      rt::process::once("SIGINT", interrupt);
      rt::process::once("SIGTERM", interrupt);
      rt::Reason failure;
      try {
        convert::ConvertOptions options{
          .direction = required("direction"),
          .source = required("source"),
          .destination = required("destination"),
          .receipt = required("receipt"),
          .deno = required("deno"),
          .denoSha256 = required("deno-sha256"),
          .signal = signal.signal(),
        };
        rt::Task<convert::ConversionReceipt> converting = convert::convertKvDatabase(std::move(options));
        const convert::ConversionReceipt receipt = co_await std::move(converting);
        rt::console::log(rt::jsonStringify(json::value_from(receipt), 2));
      } catch (...) {
        failure = std::current_exception();
      }
      rt::process::off("SIGINT", interrupt);
      rt::process::off("SIGTERM", interrupt);
      if (failure) std::rethrow_exception(failure);
    }
  }

  rt::Task<void> importMetaMain() {
    rt::Reason failure;
    try {
      rt::Task<void> running = main();
      co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return;
    const std::optional<std::string> message = rt::isError(failure) ? rt::errorMessage(failure) : std::nullopt;
    rt::console::error(message ? *message : std::string("Conversion failed"));
    rt::process::setExitCode(1);
  }
}
