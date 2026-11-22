#include "uos/static.hpp"
#include "uos/rt/memory.hpp"

#include "uos/config.hpp"
#include "uos/http.hpp"
#include "uos/kv.hpp"
#include "runtime/paths.hpp"

#include "uos/rt/console.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <array>
#include <exception>
#include <filesystem>
#include <memory>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace uos::static_ {
  namespace {
    std::filesystem::path resourcePath(std::string_view relative) { return std::filesystem::path(runtime::paths::resourceRoot()) / relative; }

    std::string readResourceText(std::string_view relative) { return rt::fs::promises::readFile(resourcePath(relative).native(), "utf8"); }

    using StaticBody = std::variant<std::string, rt::Bytes>;

    struct StaticAsset {
      std::filesystem::path path;
      std::string label;
      std::string contentType;
      std::string readAs;  // "text" | "bytes"
      std::string security;  // "html" | "asset"
      std::optional<StaticBody> body{};
    };

    // The module's load-time state: the preloaded texts, the cache policy and the route table
    // (routes of one asset share it, as the JS Map entries share the object).
    struct StaticModule {
      std::string aboutHtmlText;
      std::string contactHtmlText;
      std::string developersHtmlText;
      std::string homeMarkdownText;
      std::string indexHtmlText;
      std::string llmsFullText;
      std::string llmsText;
      std::string modelsHtmlText;
      std::string openApiText;
      std::string privacyHtmlText;
      std::string robotsText;
      std::string sitemapText;
      std::string staticCacheControl;
      rt::OrderedMap<std::string, std::shared_ptr<const StaticAsset>> staticAssets;
      std::shared_ptr<const StaticAsset> indexHtmlAsset;
      std::shared_ptr<const StaticAsset> indexMarkdownAsset;
      std::size_t assetBytes = 0;

      StaticModule()
        : aboutHtmlText(readResourceText("static/about.html")),
          contactHtmlText(readResourceText("static/contact.html")),
          developersHtmlText(readResourceText("static/developers.html")),
          homeMarkdownText(readResourceText("static/home.md")),
          indexHtmlText(readResourceText("static/index.html")),
          llmsFullText(readResourceText("static/docs/llms-agents.md")),
          llmsText(readResourceText("static/llms.txt")),
          modelsHtmlText(readResourceText("static/models.html")),
          openApiText(readResourceText("static/openapi.json")),
          privacyHtmlText(readResourceText("static/privacy.html")),
          robotsText(readResourceText("static/robots.txt")),
          sitemapText(readResourceText("static/sitemap.xml")),
          staticCacheControl(config::config.isDeploy ? "public, max-age=300" : "no-store") {
        indexHtmlAsset = textAsset({"/index.html"}, "index.html", "text/html; charset=utf-8", "html", indexHtmlText);
        indexMarkdownAsset = textAsset({}, "home.md", "text/markdown; charset=utf-8", "asset", homeMarkdownText);

        textAsset({"/docs", "/docs.html"}, "docs.html", "text/html; charset=utf-8", "html");
        textAsset({"/developers", "/developers.html"}, "developers.html", "text/html; charset=utf-8", "html", developersHtmlText);
        textAsset({"/about", "/about.html"}, "about.html", "text/html; charset=utf-8", "html", aboutHtmlText);
        textAsset({"/contact", "/contact.html"}, "contact.html", "text/html; charset=utf-8", "html", contactHtmlText);
        textAsset({"/privacy", "/privacy.html"}, "privacy.html", "text/html; charset=utf-8", "html", privacyHtmlText);
        textAsset({"/chat", "/chat.html"}, "chat.html", "text/html; charset=utf-8", "html");
        textAsset({"/models", "/models.html"}, "models.html", "text/html; charset=utf-8", "html", modelsHtmlText);
        textAsset({"/admin", "/admin.html"}, "admin.html", "text/html; charset=utf-8", "html");

        textAsset({"/style.css"}, "style.css", "text/css; charset=utf-8");
        textAsset({"/docs.css"}, "docs.css", "text/css; charset=utf-8");
        textAsset({"/chat.css"}, "chat.css", "text/css; charset=utf-8");
        textAsset({"/models.css"}, "models.css", "text/css; charset=utf-8");
        textAsset({"/home.css"}, "home.css", "text/css; charset=utf-8");
        textAsset({"/admin.css"}, "admin.css", "text/css; charset=utf-8");
        textAsset({"/admin-supervisor.css"}, "admin-supervisor.css", "text/css; charset=utf-8");

        textAsset({"/app.js"}, "app.js", "text/javascript; charset=utf-8");
        textAsset({"/docs.js"}, "docs.js", "text/javascript; charset=utf-8");
        textAsset({"/chat.js"}, "chat.js", "text/javascript; charset=utf-8");
        textAsset({"/chat-stats.js"}, "chat-stats.js", "text/javascript; charset=utf-8");
        textAsset({"/models.js"}, "models.js", "text/javascript; charset=utf-8");
        textAsset({"/toast.js"}, "toast.js", "text/javascript; charset=utf-8");
        textAsset({"/admin.js"}, "admin.js", "text/javascript; charset=utf-8");
        textAsset({"/admin-reset.js"}, "admin-reset.js", "text/javascript; charset=utf-8");
        textAsset({"/admin-supervisor.js"}, "admin-supervisor.js", "text/javascript; charset=utf-8");
        textAsset({"/admin-cache.js"}, "admin-cache.js", "text/javascript; charset=utf-8");
        textAsset({"/auth.js"}, "auth.js", "text/javascript; charset=utf-8");
        textAsset({"/auth-relay.js"}, "auth-relay.js", "text/javascript; charset=utf-8");
        textAsset({"/foreground-refresh.js"}, "foreground-refresh.js", "text/javascript; charset=utf-8");
        textAsset({"/network.js"}, "network.js", "text/javascript; charset=utf-8");
        textAsset({"/reasoning-select.js"}, "reasoning-select.js", "text/javascript; charset=utf-8");

        textAsset({"/company-logo.svg"}, "company-logo.svg", "image/svg+xml; charset=utf-8");
        textAsset({"/llms.txt"}, "llms.txt", "text/plain; charset=utf-8", "asset", llmsText);
        textAsset({"/llms-full.txt"}, "docs/llms-agents.md", "text/plain; charset=utf-8", "asset", llmsFullText);
        textAsset({"/docs/llms-agents.md"}, "docs/llms-agents.md", "text/markdown; charset=utf-8", "asset", llmsFullText);
        textAsset({"/openapi.json"}, "openapi.json", "application/json; charset=utf-8", "asset", openApiText);
        textAsset({"/robots.txt"}, "robots.txt", "text/plain; charset=utf-8", "asset", robotsText);
        textAsset({"/sitemap.xml"}, "sitemap.xml", "application/xml; charset=utf-8", "asset", sitemapText);

        bytesAsset({"/favicon.ico", "/favicon.png"}, "favicon.png", "image/png");
        bytesAsset({"/favicon-32.png"}, "favicon-32.png", "image/png");
        for (const auto* text : {&aboutHtmlText, &contactHtmlText, &developersHtmlText, &homeMarkdownText, &indexHtmlText,
                                &llmsFullText, &llmsText, &modelsHtmlText, &openApiText, &privacyHtmlText, &robotsText, &sitemapText})
          assetBytes += rt::memory::bytes(*text);
        rt::memory::setCache(rt::memory::Cache::static_assets, staticAssets.size(), assetBytes);
      }

      std::shared_ptr<const StaticAsset> registerAsset(const std::vector<std::string>& routes, std::shared_ptr<const StaticAsset> asset) {
        assetBytes += sizeof(StaticAsset) + rt::memory::bytes(asset->path) + rt::memory::bytes(asset->label) +
                      rt::memory::bytes(asset->contentType) + rt::memory::bytes(asset->readAs) + rt::memory::bytes(asset->security);
        if (asset->body) {
          if (const auto* text = std::get_if<std::string>(&*asset->body)) assetBytes += rt::memory::bytes(*text);
        }
        for (const std::string& route : routes) {
          staticAssets.set(route, asset);
          assetBytes += 2 * (sizeof(route) + rt::memory::bytes(route));
        }
        return asset;
      }

      std::shared_ptr<const StaticAsset> textAsset(const std::vector<std::string>& routes, const std::string& path, const std::string& contentType,
                                                   const std::string& security = "asset", const std::optional<std::string>& body = std::nullopt) {
        const std::string relative = "static/" + path;
        return registerAsset(routes, std::make_shared<const StaticAsset>(StaticAsset{
                                       .path = resourcePath(relative),
                                       .label = relative,
                                       .contentType = contentType,
                                       .readAs = "text",
                                       .security = security,
                                       .body = body ? std::optional<StaticBody>(StaticBody(*body)) : std::nullopt,
                                     }));
      }

      std::shared_ptr<const StaticAsset> bytesAsset(const std::vector<std::string>& routes, const std::string& path, const std::string& contentType) {
        return registerAsset(routes, std::make_shared<const StaticAsset>(StaticAsset{
                                       .path = resourcePath(path),
                                       .label = path,
                                       .contentType = contentType,
                                       .readAs = "bytes",
                                       .security = "asset",
                                     }));
      }
    };

    const StaticModule& staticModule() {
      static const StaticModule module;
      return module;
    }

    // std::nullopt is null.
    rt::Task<std::optional<StaticBody>> readAsset(std::shared_ptr<const StaticAsset> asset) {
      std::optional<StaticBody> body;
      try {
        if (asset->body) {
          body = *asset->body;
        } else if (asset->readAs == "text") {
          body = StaticBody(rt::fs::promises::readFile(asset->path.native(), "utf8"));
        } else {
          body = StaticBody(rt::fs::promises::readFile(asset->path.native()));
        }
      } catch (...) {
        rt::console::error("[ai.ubq.fi] Failed to load " + asset->label + ":", rt::console::Value(std::current_exception()));
        body = std::nullopt;
      }
      if (!asset->body) co_await rt::microtask();
      co_return body;
    }

    rt::Response notFoundResponse() {
      return http::json(404, rt::jsonParse(R"({"error":{"message":"Public resource not found. Read /openapi.json for API endpoints or /llms.txt for agent guidance.","type":"not_found","code":"not_found"}})"));
    }

    rt::Headers htmlSecurityHeaders() {
      return rt::Headers{
        {"Cache-Control", staticModule().staticCacheControl},
        {"Content-Security-Policy",
         "default-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'; img-src 'self' data:; object-src 'none'; style-src 'self'; script-src 'self'; connect-src 'self' https://ai.ubq.fi"},
        {"Referrer-Policy", "no-referrer"},
        {"X-Content-Type-Options", "nosniff"},
      };
    }

    // Spread order preserved exactly: Content-Type first, then the per-security
    // defaults, then `extra` (which therefore wins on any collision).
    rt::Headers staticHeaders(const StaticAsset& asset, const rt::Headers& extra) {
      rt::Headers headers{{"Content-Type", asset.contentType}};
      const rt::Headers securityHeaders = asset.security == "html"
                                            ? htmlSecurityHeaders()
                                            : rt::Headers{{"Cache-Control", staticModule().staticCacheControl}, {"X-Content-Type-Options", "nosniff"}};
      for (const auto& [name, value] : securityHeaders.entries()) headers.set(name, value);
      for (const auto& [name, value] : rt::Headers(extra).entries()) headers.set(name, value);
      return headers;
    }

    // The overload without `extraHeaders` is undefined (`{}`).
    rt::Task<rt::Response> serveAsset(std::shared_ptr<const StaticAsset> asset, rt::Headers extraHeaders) {
      auto reading = readAsset(asset);
      const std::optional<StaticBody> body = co_await std::move(reading);
      if (!body) co_return notFoundResponse();
      rt::ResponseInit init;
      init.status = 200;
      init.headers = staticHeaders(*asset, extraHeaders);
      rt::BodyInit bodyInit = std::holds_alternative<std::string>(*body) ? rt::BodyInit(std::get<std::string>(*body)) : rt::BodyInit(std::get<rt::Bytes>(*body));
      co_return rt::Response(std::move(bodyInit), std::move(init));
    }

    // `Readonly<{ quality: number; specificity: number; index: number }>`
    struct MediaPreference {
      double quality;
      double specificity;
      double index;
    };

    double mediaSpecificity(const std::string& rangeType, const std::string& rangeSubtype) {
      if (rangeType == "*") return 0;
      if (rangeSubtype == "*") return 1;
      return 2;
    }

    std::optional<MediaPreference> parseMediaEntry(const std::string& entry, double index, const std::string& targetType, const std::string& targetSubtype) {
      const std::vector<std::string> parts = rt::split(rt::toLowerCase(rt::trim(entry)), ";");
      const std::string& rawRange = parts[0];
      const std::vector<std::string> range = rt::split(rt::trim(rawRange), "/");
      const std::string rangeType = range[0];
      const std::optional<std::string> rangeSubtype = range.size() > 1 ? std::optional<std::string>(range[1]) : std::nullopt;
      if (rangeType.empty() || !rangeSubtype || rangeSubtype->empty()) return std::nullopt;
      if (rangeType != "*" && rangeType != targetType) return std::nullopt;
      if (*rangeSubtype != "*" && *rangeSubtype != targetSubtype) return std::nullopt;

      // rawParameters.map((parameter) => parameter.trim().split("=", 2)).find(([name]) => name === "q")?.[1]
      std::optional<std::string> rawQuality;
      for (std::size_t position = 1; position < parts.size(); ++position) {
        const std::vector<std::string> pair = rt::split(rt::trim(parts[position]), "=", 2);
        if (pair[0] == "q") {
          if (pair.size() > 1) rawQuality = pair[1];
          break;
        }
      }
      const double parsedQuality = !rawQuality ? 1 : rt::stringToNumber(*rawQuality);
      const double quality = rt::Number::isFinite(parsedQuality) && parsedQuality >= 0 && parsedQuality <= 1 ? parsedQuality : 0;
      return MediaPreference{.quality = quality, .specificity = mediaSpecificity(rangeType, *rangeSubtype), .index = index};
    }

    bool isBetterPreference(const MediaPreference& candidate, const std::optional<MediaPreference>& best) {
      return !best || candidate.specificity > best->specificity || (candidate.specificity == best->specificity && candidate.index < best->index);
    }

    std::optional<MediaPreference> mediaPreference(const std::string& accept, const std::string& target) {
      const std::vector<std::string> targetParts = rt::split(target, "/");
      const std::string targetType = targetParts[0];
      const std::string targetSubtype = targetParts.size() > 1 ? targetParts[1] : std::string();
      if (targetType.empty() || targetSubtype.empty()) return std::nullopt;

      std::optional<MediaPreference> best;
      const std::vector<std::string> entries = rt::split(accept, ",");
      for (std::size_t index = 0; index < entries.size(); ++index) {
        const std::optional<MediaPreference> candidate = parseMediaEntry(entries[index], static_cast<double>(index), targetType, targetSubtype);
        if (candidate && isBetterPreference(*candidate, best)) best = candidate;
      }
      return best;
    }

    std::optional<MediaPreference> preferredMedia(std::initializer_list<std::optional<MediaPreference>> preferences) {
      std::optional<MediaPreference> best;
      for (const std::optional<MediaPreference>& candidate : preferences) {
        if (!candidate || !best || candidate->quality > best->quality || (candidate->quality == best->quality && candidate->specificity > best->specificity) ||
            (candidate->quality == best->quality && candidate->specificity == best->specificity && candidate->index < best->index)) {
          if (candidate) best = candidate;
        }
      }
      return best;
    }

    // "html" | "markdown" | "json"; std::nullopt is null.
    std::optional<std::string> rootRepresentation(const std::string& accept) {
      if (rt::trim(accept).empty()) return "html";
      struct Candidate {
        std::string representation;
        MediaPreference preference;
        double defaultPriority;
      };
      const std::array<std::tuple<std::string, std::optional<MediaPreference>, double>, 3> ranked{{
        {"html", preferredMedia({mediaPreference(accept, "text/html"), mediaPreference(accept, "application/xhtml+xml")}), 2},
        {"markdown", mediaPreference(accept, "text/markdown"), 1},
        {"json", mediaPreference(accept, "application/json"), 0},
      }};
      std::vector<Candidate> candidates;
      for (const auto& [representation, preference, defaultPriority] : ranked) {
        if (preference && rt::truthy(preference->quality)) candidates.push_back({representation, *preference, defaultPriority});
      }
      if (candidates.empty()) return std::nullopt;
      // `a || b` over numbers: the first difference that is neither 0 nor NaN.
      rt::sort(candidates, [](const Candidate& left, const Candidate& right) {
        for (const double difference : {right.preference.quality - left.preference.quality, right.preference.specificity - left.preference.specificity,
                                         left.preference.index - right.preference.index}) {
          if (rt::truthy(difference)) return difference;
        }
        return right.defaultPriority - left.defaultPriority;
      });
      return candidates[0].representation;
    }

    rt::Headers rootVaryHeaders() { return rt::Headers{{"Vary", "Accept, Accept-Encoding"}}; }

    rt::Response notAcceptable() {
      return http::json(
        406,
        rt::jsonParse(R"({"error":{"message":"The homepage is available as text/html, text/markdown, or application/json.","type":"not_acceptable","code":"not_acceptable"}})"),
        rootVaryHeaders());
    }

    std::string resolveAuthMode(bool isDeploy, bool hasAuthTokens, bool hasKv) {
      if (isDeploy && !hasAuthTokens && !hasKv) return "misconfigured";
      if (isDeploy || hasAuthTokens || hasKv) return "required";
      return "disabled (local only)";
    }
  }

  rt::Task<std::optional<rt::Response>> handleStaticAsset(std::string path) {
    const std::shared_ptr<const StaticAsset>* asset = staticModule().staticAssets.get(path);
    if (!asset) co_return std::nullopt;
    auto serving = serveAsset(*asset, rt::Headers());
    rt::Response response = co_await std::move(serving);
    co_return response;
  }

  bool hasStaticAsset(const std::string& path) { return staticModule().staticAssets.has(path); }

  rt::Task<rt::Response> handleRoot(rt::Request req) {
    const StaticModule& module = staticModule();
    const rt::URL url(req.url());
    const std::string path = url.pathname();

    if (path == "/index.html") {
      auto serving = serveAsset(module.indexHtmlAsset, rootVaryHeaders());
      rt::Response response = co_await std::move(serving);
      co_return response;
    }

    const std::optional<std::string> representation = rootRepresentation(req.headers().get("Accept").value_or(""));
    if (!representation) co_return notAcceptable();
    if (*representation == "html") {
      auto serving = serveAsset(module.indexHtmlAsset, rootVaryHeaders());
      rt::Response response = co_await std::move(serving);
      co_return response;
    }
    if (*representation == "markdown") {
      auto serving = serveAsset(module.indexMarkdownAsset, rootVaryHeaders());
      rt::Response response = co_await std::move(serving);
      co_return response;
    }

    auto pending = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await pending.wait();
    const std::string auth = resolveAuthMode(config::config.isDeploy, !config::config.authTokens.empty(), static_cast<bool>(kv));

    json::object endpoints;
    endpoints["openai_compat"] = "/v1/*";
    endpoints["health"] = "/health";
    json::object discovery;
    discovery["llms"] = "/llms.txt";
    discovery["llms_full"] = "/llms-full.txt";
    discovery["openapi"] = "/openapi.json";
    discovery["developers"] = "/developers";
    discovery["sitemap"] = "/sitemap.xml";
    json::object body;
    body["service"] = "ai.ubq.fi";
    body["name"] = "UbiquityOS AI Gateway";
    body["upstream"] = "chatgpt_codex";
    body["auth"] = auth;
    body["endpoints"] = std::move(endpoints);
    body["discovery"] = std::move(discovery);
    co_return http::json(200, json::value(std::move(body)), rootVaryHeaders());
  }
}
