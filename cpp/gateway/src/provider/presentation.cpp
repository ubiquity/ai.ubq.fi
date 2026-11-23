#include "uos/provider/presentation.hpp"

#include "uos/rt/record.hpp"

#include <utility>

namespace uos::provider::presentation {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
  }

  ProviderPresentation providerPresentation(const std::string& id) {
    for (const auto& [knownId, known] : PROVIDER_PRESENTATION) {
      if (knownId == id) return known;
    }
    return ProviderPresentation{
      .label = id,
      .tier = "direct",
      .detail = id + " has no presentation entry yet.",
      .endpoints = {},
      // The fallback reports under its own id, the convention every direct route
      // already follows, so a new provider can be rostered before its health key.
      .health_key = id,
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderPresentation& value) {
    out = Writer()
            .required("label", value.label)
            .required("tier", value.tier)
            .required("detail", value.detail)
            .required("endpoints", value.endpoints)
            .required("health_key", value.health_key)
            .done();
  }

  ProviderPresentation tag_invoke(const json::value_to_tag<ProviderPresentation>&, const json::value& value) {
    const Reader in(value, "ProviderPresentation");
    return ProviderPresentation{
      .label = in.required<std::string>("label"),
      .tier = in.required<std::string>("tier"),
      .detail = in.required<std::string>("detail"),
      .endpoints = in.required<std::vector<std::string>>("endpoints"),
      .health_key = in.required<std::string>("health_key"),
    };
  }
}
