#pragma once
// Port of src/embeddings/handlers.ts.
#include "uos/openai_telemetry.hpp"
#include "uos/rt/kv.hpp"
namespace uos::embeddings::handlers {
  rt::Response withVoyageUpstreamHeader(rt::Response response);
  struct HandleUosEmbeddingsOptions { std::optional<std::shared_ptr<rt::Kv>> kv{}; };
  rt::Task<rt::Response> handleUosEmbeddings(rt::Request req, const openai_telemetry::UsageContext* usageContext = nullptr);
  rt::Task<rt::Response> handleUosEmbeddings(rt::Request req, const openai_telemetry::UsageContext* usageContext, HandleUosEmbeddingsOptions options);
}
