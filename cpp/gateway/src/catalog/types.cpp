#include "uos/catalog/types.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include <string_view>
#include <type_traits>

namespace uos::catalog::types {
  const rt::KvKey CODEX_CATALOG_AUTH_GENERATION_KEY{"ubq_ai", "codex_catalog_auth_generation"};
  const rt::KvKey CODEX_CATALOG_PREFIX{"ubq_ai", "codex_catalog"};
  const rt::KvKey CODEX_CATALOG_CHUNK_PREFIX{"ubq_ai", "codex_catalog_chunk"};
  const rt::KvKey CODEX_CATALOG_SOURCE_CHUNK_PREFIX{"ubq_ai", "codex_catalog_source_chunk"};
  const rt::KvKey CODEX_CATALOG_LEASE_PREFIX{"ubq_ai", "codex_catalog_refresh_lease"};

  namespace {
    // Member-wise JSON conversion (the local copy every record module carries; rt has none).
    std::string describe(const json::value* value) {
      if (!value) return "undefined";
      if (value->is_null()) return "null";
      if (value->is_array()) return "array";
      return std::string(rt::typeOf(*value));
    }

    template <class T> std::string expected() {
      if constexpr (std::is_same_v<T, double>) return "number";
      else if constexpr (std::is_same_v<T, std::string>) return "string";
      else return "object";
    }

    template <class T> json::value toJson(const T& value) {
      if constexpr (std::is_same_v<T, double>) return rt::makeNumber(value);
      else if constexpr (std::is_same_v<T, std::string>) return json::value(json::string(value));
      else if constexpr (std::is_same_v<T, json::object>) return rt::canonicalNumbers(json::value(value));
      else return json::value_from(value);
    }

    class Writer {
    public:
      template <class T> Writer& required(std::string_view key, const T& value) {
        object_[key] = toJson(value);
        return *this;
      }
      // `T | null`
      template <class T> Writer& orNull(std::string_view key, const std::optional<T>& value) {
        object_[key] = value ? toJson(*value) : json::value(nullptr);
        return *this;
      }
      // `key?: T`
      template <class T> Writer& orUndefined(std::string_view key, const std::optional<T>& value) {
        if (value) object_[key] = toJson(*value);
        return *this;
      }
      json::value done() { return json::value(std::move(object_)); }

    private:
      json::object object_;
    };

    class Reader {
    public:
      Reader(const json::value& value, std::string_view type) : type_(type), object_(value.if_object()) {
        if (!object_) throw rt::TypeError(std::string(type) + " is " + describe(&value) + ", not object");
      }
      template <class T> T required(std::string_view key) const {
        const json::value* member = rt::get(*object_, key);
        if (!member) fail(key, member, expected<T>());
        return convert<T>(key, *member, "");
      }
      template <class T> std::optional<T> orNull(std::string_view key) const {
        const json::value* member = rt::get(*object_, key);
        if (!member) fail(key, member, expected<T>() + " or null");
        if (member->is_null()) return std::nullopt;
        return convert<T>(key, *member, " or null");
      }
      template <class T> std::optional<T> orUndefined(std::string_view key) const {
        const json::value* member = rt::get(*object_, key);
        if (!member) return std::nullopt;
        return convert<T>(key, *member, " or undefined");
      }

    private:
      [[noreturn]] void fail(std::string_view key, const json::value* member, const std::string& wanted) const {
        throw rt::TypeError(std::string(type_) + "." + std::string(key) + " is " + describe(member) + ", not " + wanted);
      }
      template <class T> T convert(std::string_view key, const json::value& member, std::string_view suffix) const {
        if constexpr (std::is_same_v<T, double>) {
          if (const std::optional<double> number = rt::getNumber(member)) return *number;
        } else if constexpr (std::is_same_v<T, std::string>) {
          if (member.is_string()) return std::string(member.get_string());
        } else if constexpr (std::is_same_v<T, json::object>) {
          if (member.is_object()) return member.get_object();
        } else {
          if (member.is_object()) return json::value_to<T>(member);
        }
        fail(key, &member, expected<T>() + std::string(suffix));
      }

      std::string_view type_;
      const json::object* object_;
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexCatalogMetadata& value) {
    out = Writer()
      .required("client_version", value.client_version)
      .required("auth_generation", value.auth_generation)
      .required("body_generation", value.body_generation)
      .required("content_type", value.content_type)
      .orNull("etag", value.etag)
      .required("fetched_at_ms", value.fetched_at_ms)
      .required("chunk_count", value.chunk_count)
      .required("compressed_bytes", value.compressed_bytes)
      .required("body_bytes", value.body_bytes)
      .required("sha256", value.sha256)
      .orUndefined("source_chunk_count", value.source_chunk_count)
      .orUndefined("source_compressed_bytes", value.source_compressed_bytes)
      .orUndefined("source_body_bytes", value.source_body_bytes)
      .orUndefined("source_sha256", value.source_sha256)
      .done();
  }

  CodexCatalogMetadata tag_invoke(const json::value_to_tag<CodexCatalogMetadata>&, const json::value& value) {
    const Reader in(value, "CodexCatalogMetadata");
    return {
      .client_version = in.required<std::string>("client_version"),
      .auth_generation = in.required<std::string>("auth_generation"),
      .body_generation = in.required<std::string>("body_generation"),
      .content_type = in.required<std::string>("content_type"),
      .etag = in.orNull<std::string>("etag"),
      .fetched_at_ms = in.required<double>("fetched_at_ms"),
      .chunk_count = in.required<double>("chunk_count"),
      .compressed_bytes = in.required<double>("compressed_bytes"),
      .body_bytes = in.required<double>("body_bytes"),
      .sha256 = in.required<std::string>("sha256"),
      .source_chunk_count = in.orUndefined<double>("source_chunk_count"),
      .source_compressed_bytes = in.orUndefined<double>("source_compressed_bytes"),
      .source_body_bytes = in.orUndefined<double>("source_body_bytes"),
      .source_sha256 = in.orUndefined<std::string>("source_sha256"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const LoadedCodexCatalog& value) {
    out = Writer()
      .required("metadata", value.metadata)
      .required("body", value.body)
      .required("parsed", value.parsed)
      .orUndefined("source_body", value.source_body)
      .done();
  }

  LoadedCodexCatalog tag_invoke(const json::value_to_tag<LoadedCodexCatalog>&, const json::value& value) {
    const Reader in(value, "LoadedCodexCatalog");
    return {
      .metadata = in.required<CodexCatalogMetadata>("metadata"),
      .body = in.required<std::string>("body"),
      .parsed = in.required<json::object>("parsed"),
      .source_body = in.orUndefined<std::string>("source_body"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const RefreshLease& value) {
    out = Writer().required("owner", value.owner).required("lease_until_ms", value.lease_until_ms).done();
  }

  RefreshLease tag_invoke(const json::value_to_tag<RefreshLease>&, const json::value& value) {
    const Reader in(value, "RefreshLease");
    return {.owner = in.required<std::string>("owner"), .lease_until_ms = in.required<double>("lease_until_ms")};
  }
}
