// Port of lib/jev_compaction/client.ts.
#include "uos/lib/jev_compaction/client.hpp"

#include "uos/lib/jev_compaction/request.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include <utility>

namespace uos::lib::jev_compaction::client {
  using request::BuildJevRequestParams;
  using request::buildJevRequest;
  using request::JevRequest;
  using request::parseJevResponse;

  JevClient::JevClient(const JevClientOptions& options)
    : apiKey(options.apiKey),
      model(options.model),
      baseUrl(options.baseUrl),
      fetcher(options.fetch ? options.fetch : rt::globalFetch()),
      signal(options.signal) {}

  rt::Task<types::JevResponse> JevClient::ask(types::JevState state, types::JevQuestions questions) {
    if (!rt::truthy(std::string_view(apiKey))) throw rt::JsError("TYPESAFE_API_KEY is not configured");
    const BuildJevRequestParams params{apiKey, model, baseUrl};
    const JevRequest request = buildJevRequest(params, state, questions);
    rt::Headers headers;
    for (const auto& [name, value] : request.headers) headers.append(name, value);
    rt::RequestInit init;
    init.method = request.method;
    init.headers = std::move(headers);
    init.body = rt::BodyInit(request.body);
    if (signal) init.signal = *signal;
    rt::Task<rt::Response> call = fetcher(rt::RequestInfo(request.url), std::move(init));
    const rt::Response response = co_await std::move(call);
    const int status = response.status();
    const bool ok = response.ok();
    const std::string text = co_await response.text();
    co_return parseJevResponse(status, ok, text);
  }
}
