#include "uos/rt/url.hpp"
#include "url_limits.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

// Ada 4.0.0's amalgamated header triggers Clang's signedness diagnostics.
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wsign-conversion"
#endif
#include "third_party/ada/ada.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <ranges>

namespace uos::rt {
  namespace {
    std::string_view usv(std::string_view input, std::string& storage) {
      if (isUtf8(input)) return input;
      storage = toWellFormed(input);
      return storage;
    }

    // Long URLs are used for image references. Keep Ada's authority and IDNA
    // input small while allowing a large path in an absolute HTTP(S) URL.
    bool boundedUrl(std::string_view input) {
      if (input.size() <= limits::kUrlAuthorityBytes) return true;
      if (input.size() > limits::kUrlBytes) return false;
      const auto scheme = toLowerCase(input.substr(0, 8));
      if (scheme.starts_with("https://")) input.remove_prefix(8);
      else if (scheme.starts_with("http://")) input.remove_prefix(7);
      else if (scheme.starts_with("file:///")) return true;
      else return input.front() == '/' && input[1] != '/' && input[1] != '\\';
      const auto first = input.find_first_not_of("/\\\t\r\n");
      if (first == std::string_view::npos) return false;
      input.remove_prefix(first);
      const auto authority = input.substr(0, input.find_first_of("/\\?#"));
      return authority.size() <= limits::kUrlAuthorityBytes;
    }

    void checkQueryOperands(std::string_view name, std::string_view value = {}) {
      if (name.size() > limits::kUrlQueryBytes || value.size() > limits::kUrlQueryBytes - name.size())
        throw TypeError("URL search parameters exceed limit");
    }

    bool unsupportedBidiCodePoint(char32_t point) {
      // Unicode 17.0 R/AL/AN code points that Ada 4.0.0 accepts inside "a…b",
      // where RFC 5893 forbids them. Recheck these ranges when upgrading Ada.
      constexpr std::pair<char32_t, char32_t> missingDirections[] = {
          {0x061d, 0x061d}, {0x0870, 0x088f}, {0x08b5, 0x08b5}, {0x08c8, 0x08c9},
          {0xfbc2, 0xfbc2}, {0x10940, 0x10959}, {0x10d40, 0x10d4f}, {0x10d6f, 0x10d85},
          {0x10d8e, 0x10d8f}, {0x10ec2, 0x10ec7}, {0x10f70, 0x10f81}, {0x10f86, 0x10f89},
      };
      return std::ranges::any_of(missingDirections, [point](const auto& range) {
        return range.first <= point && point <= range.second;
      });
    }

    bool asciiBidiCompanion(std::string_view label) {
      if (label.empty()) return true;
      auto letter = [](char c) { return c >= 'a' && c <= 'z'; };
      auto alphanumeric = [letter](char c) { return letter(c) || (c >= '0' && c <= '9'); };
      return letter(label.front()) && alphanumeric(label.back()) &&
             std::ranges::all_of(label, [alphanumeric](char c) { return alphanumeric(c) || c == '-'; });
    }

    bool validHost(const ada::url_aggregator& url) {
      if (!url.is_special()) return true;
      const auto host = url.get_hostname();
      if (host.size() > limits::kUrlAuthorityBytes) return false;
      if (host.find("xn--") == std::string_view::npos) return true;

      // Ada's ASCII fast path skips A-label validation. ToUnicode leaves an
      // invalid A-label untouched.
      std::u32string prefixed;
      bool hasRtl = false;
      bool hasUnsafeCompanion = false;
      for (const auto part : host | std::views::split('.')) {
        const std::string_view label(part.begin(), part.end());
        if (!label.starts_with("xn--")) {
          hasUnsafeCompanion |= !asciiBidiCompanion(label);
          continue;
        }
        const std::string decoded = ada::idna::to_unicode(label);
        // Ada 4.0.0 omits some combining marks from its leading-mark check.
        static const Regex leadingMark(R"(^\p{Mark})", "u");
        if (decoded == label || decoded.find('.') != std::string::npos || leadingMark.test(decoded)) return false;
        if (std::ranges::any_of(decoded, [](char c) {
              return static_cast<unsigned char>(c) < 0x80 &&
                     ada::idna::contains_forbidden_domain_code_point(std::string_view(&c, 1));
            })) return false;
        prefixed.resize(decoded.size() + 1);
        prefixed.front() = U'a';
        const auto length = ada::idna::utf8_to_utf32(decoded.data(), decoded.size(), prefixed.data() + 1);
        if (length == 0) return false;
        prefixed.resize(length + 1);
        // Ada can stop at the first joiner, before checking the rest of the
        // label. Refuse joiners and code points with missing direction data.
        if (std::ranges::any_of(prefixed, [](char32_t point) {
              return point == U'\u200c' || point == U'\u200d' || unsupportedBidiCodePoint(point);
            })) return false;
        // A leading L character makes the validated label fail Ada's check
        // exactly when it contains RTL text. Ada validates those labels in
        // isolation, so require ordinary DNS labels for their LTR companions.
        if (ada::idna::is_label_valid(prefixed)) hasUnsafeCompanion = true;
        else hasRtl = true;
      }
      return !hasRtl || !hasUnsafeCompanion;
    }

    ada::result<ada::url_aggregator> parseUrl(std::string_view input, const ada::url_aggregator* base = nullptr) {
      if (!boundedUrl(input)) return tl::unexpected(ada::errors::type_error);
      std::string storage;
      const auto prepared = usv(input, storage);
      if (!storage.empty() && !boundedUrl(prepared)) return tl::unexpected(ada::errors::type_error);
      auto parsed = ada::parse(prepared, base);
      if (parsed && (!validHost(*parsed) || !detail::boundedUrlQuery(parsed->get_search()))) return tl::unexpected(ada::errors::type_error);
      return parsed;
    }

    ada::url_aggregator parseOrThrow(std::string_view input, const ada::url_aggregator* base = nullptr) {
      auto parsed = parseUrl(input, base);
      if (!parsed) throw InvalidURLError(input.size() > limits::kUrlAuthorityBytes ? std::string{} : std::string(input));
      return std::move(*parsed);
    }

    detail::URLRecord describeRecord(const ada::url_aggregator& url) {
      detail::URLRecord out;
      const auto protocol = url.get_protocol();
      out.scheme = protocol.substr(0, protocol.size() - 1);
      out.username = url.get_username();
      out.password = url.get_password();
      if (url.has_hostname()) out.host = url.get_hostname();
      if (url.has_port()) out.port = static_cast<std::uint16_t>(url.get_components().port);
      out.hasOpaquePath = url.has_opaque_path;
      auto path = url.get_pathname();
      if (out.hasOpaquePath) out.opaquePath = path;
      else if (!path.empty()) {
        path.remove_prefix(1);
        for (;;) {
          const auto slash = path.find('/');
          out.path.emplace_back(path.substr(0, slash));
          if (slash == std::string_view::npos) break;
          path.remove_prefix(slash + 1);
        }
      }
      const auto search = url.get_search();
      const auto hash = url.get_hash();
      if (url.has_search()) out.query = search.empty() ? std::string_view{} : search.substr(1);
      if (url.has_hash()) out.fragment = hash.empty() ? std::string_view{} : hash.substr(1);
      return out;
    }

    std::uint32_t arrayIndex(std::string_view key) {
      constexpr auto nonIndex = std::numeric_limits<std::uint32_t>::max();
      if (key.empty() || (key.size() > 1 && key.front() == '0')) return nonIndex;
      std::uint32_t value = 0;
      const auto [end, error] = std::from_chars(key.data(), key.data() + key.size(), value);
      return error == std::errc{} && end == key.data() + key.size() ? value : nonIndex;
    }

    constexpr auto filePathEscapes = [] {
      std::array<std::uint8_t, 32> bytes{};
      auto add = [&](unsigned c) { bytes[c / 8] |= static_cast<std::uint8_t>(1u << (c % 8)); };
      add(0);
      for (const char c : std::string_view("\t\n\r \"#%<>?[\\]^{}|\x60~")) add(static_cast<unsigned char>(c));
      for (unsigned c = 0x7f; c <= 0xff; ++c) add(c);
      return bytes;
    }();
  }

  URLSearchParams::URLSearchParams() noexcept = default;

  URLSearchParams::URLSearchParams(std::string_view init) {
    if (!detail::boundedUrlQuery(init)) throw TypeError("URL search parameters exceed limit");
    std::string storage;
    reset(usv(init, storage));
  }

  URLSearchParams::URLSearchParams(std::vector<Entry> sequence) {
    for (const auto& [name, value] : sequence) append(name, value);
  }

  URLSearchParams URLSearchParams::fromSequence(const std::vector<std::vector<std::string>>& sequence) {
    URLSearchParams out;
    for (const auto& pair : sequence) {
      if (pair.size() != 2) throw TypeError("Type error");
      out.append(pair[0], pair[1]);
    }
    return out;
  }

  URLSearchParams URLSearchParams::fromRecord(const std::vector<Entry>& record) {
    auto ordered = record;
    std::stable_sort(ordered.begin(), ordered.end(), [](const Entry& a, const Entry& b) {
      return arrayIndex(a.first) < arrayIndex(b.first);
    });
    URLSearchParams out;
    for (const auto& [name, value] : ordered) out.set(name, value);
    return out;
  }

  URLSearchParams::URLSearchParams(const URLSearchParams& other)
      : params_(other.params_ ? std::make_unique<ada::url_search_params>(*other.params_) : nullptr) {}

  URLSearchParams::URLSearchParams(URLSearchParams&& other) noexcept : params_(std::move(other.params_)) {}

  URLSearchParams& URLSearchParams::operator=(const URLSearchParams& other) {
    if (this != &other) {
      params_ = other.params_ ? std::make_unique<ada::url_search_params>(*other.params_) : nullptr;
      update();
    }
    return *this;
  }

  URLSearchParams& URLSearchParams::operator=(URLSearchParams&& other) noexcept {
    if (this != &other) {
      params_ = std::move(other.params_);
      update();
    }
    return *this;
  }

  URLSearchParams::~URLSearchParams() = default;

  ada::url_search_params& URLSearchParams::data() {
    if (!params_) params_ = std::make_unique<ada::url_search_params>();
    return *params_;
  }

  void URLSearchParams::reset(std::string_view query) {
    if (!detail::boundedUrlQuery(query)) throw TypeError("URL search parameters exceed limit");
    if (query.empty()) params_.reset();
    else data().reset(query);
  }

  void URLSearchParams::update() {
    if (url_) url_->setQuery(toString());
  }

  std::size_t URLSearchParams::size() const { return params_ ? params_->size() : 0; }

  void URLSearchParams::append(std::string_view name, std::string_view value) {
    checkQueryOperands(name, value);
    if (size() == limits::kUrlQueryPairs) throw TypeError("URL search parameters exceed limit");
    std::string keyStorage, valueStorage;
    name = usv(name, keyStorage);
    value = usv(value, valueStorage);
    checkQueryOperands(name, value);
    auto candidate = data();
    candidate.append(name, value);
    if (!detail::boundedUrlQuery(candidate.to_string())) throw TypeError("URL search parameters exceed limit");
    *params_ = std::move(candidate);
    update();
  }

  void URLSearchParams::delete_(std::string_view name) {
    checkQueryOperands(name);
    std::string storage;
    if (params_) params_->remove(usv(name, storage));
    update();
  }

  void URLSearchParams::delete_(std::string_view name, std::string_view value) {
    checkQueryOperands(name, value);
    std::string keyStorage, valueStorage;
    if (params_) params_->remove(usv(name, keyStorage), usv(value, valueStorage));
    update();
  }

  std::optional<std::string> URLSearchParams::get(std::string_view name) const {
    checkQueryOperands(name);
    std::string storage;
    if (params_)
      if (const auto value = params_->get(usv(name, storage))) return std::string(*value);
    return std::nullopt;
  }

  std::vector<std::string> URLSearchParams::getAll(std::string_view name) const {
    checkQueryOperands(name);
    std::string storage;
    return params_ ? params_->get_all(usv(name, storage)) : std::vector<std::string>{};
  }

  bool URLSearchParams::has(std::string_view name) const {
    checkQueryOperands(name);
    std::string storage;
    return params_ && params_->has(usv(name, storage));
  }

  bool URLSearchParams::has(std::string_view name, std::string_view value) const {
    checkQueryOperands(name, value);
    std::string keyStorage, valueStorage;
    return params_ && params_->has(usv(name, keyStorage), usv(value, valueStorage));
  }

  void URLSearchParams::set(std::string_view name, std::string_view value) {
    checkQueryOperands(name, value);
    std::string keyStorage, valueStorage;
    name = usv(name, keyStorage);
    value = usv(value, valueStorage);
    checkQueryOperands(name, value);
    auto candidate = data();
    candidate.set(name, value);
    if (!detail::boundedUrlQuery(candidate.to_string())) throw TypeError("URL search parameters exceed limit");
    *params_ = std::move(candidate);
    update();
  }

  void URLSearchParams::sort() {
    if (params_) params_->sort();
    update();
  }

  std::string URLSearchParams::toString() const { return params_ ? params_->to_string() : std::string{}; }

  std::vector<URLSearchParams::Entry> URLSearchParams::entries() const {
    return params_ ? std::vector<Entry>(params_->begin(), params_->end()) : std::vector<Entry>{};
  }

  std::vector<std::string> URLSearchParams::keys() const {
    std::vector<std::string> keys;
    keys.reserve(size());
    if (params_)
      for (const auto& entry : *params_) keys.push_back(entry.first);
    return keys;
  }

  std::vector<std::string> URLSearchParams::values() const {
    std::vector<std::string> values;
    values.reserve(size());
    if (params_)
      for (const auto& entry : *params_) values.push_back(entry.second);
    return values;
  }

  void URLSearchParams::forEach(const std::function<void(const std::string& value, const std::string& key)>& callback) const {
    for (const auto& [key, value] : *this) callback(value, key);
  }

  URLSearchParams::Entry URLSearchParams::iterator::operator*() const { return (*owner_->params_)[index_]; }

  URL::URL(ada::url_aggregator parsed) { adopt(std::move(parsed)); }

  URL::URL(std::string_view input) : URL(parseOrThrow(input)) {}

  URL::URL(std::string_view input, std::string_view base) {
    const auto parsedBase = parseUrl(base);
    if (!parsedBase) throw InvalidURLError(input.size() > limits::kUrlAuthorityBytes ? std::string{} : std::string(input));
    adopt(parseOrThrow(input, &*parsedBase));
  }

  URL::URL(std::string_view input, const URL& base) : URL(parseOrThrow(input, base.url_.get())) {}

  URL::URL(const URL& other)
      : url_(other.url_ ? std::make_unique<ada::url_aggregator>(*other.url_) : nullptr), searchParams_(other.searchParams_) {
    searchParams_.url_ = this;
  }

  URL::URL(URL&& other) noexcept : url_(std::move(other.url_)), searchParams_(std::move(other.searchParams_)), record_(std::move(other.record_)) {
    searchParams_.url_ = this;
  }

  URL& URL::operator=(const URL& other) {
    if (this != &other) *this = URL(other);
    return *this;
  }

  URL& URL::operator=(URL&& other) noexcept {
    if (this != &other) {
      url_ = std::move(other.url_);
      searchParams_.params_ = std::move(other.searchParams_.params_);
      searchParams_.url_ = this;
      record_ = std::move(other.record_);
    }
    return *this;
  }

  URL::~URL() = default;

  void URL::adopt(ada::url_aggregator parsed) {
    if (url_) *url_ = std::move(parsed);
    else url_ = std::make_unique<ada::url_aggregator>(std::move(parsed));
    searchParams_.url_ = this;
    searchParams_.reset(url_->get_search());
    refreshRecord();
  }

  void URL::refreshRecord() {
    if (record_) *record_ = describeRecord(*url_);
  }

  const detail::URLRecord& URL::record() const {
    if (!record_) record_ = std::make_unique<detail::URLRecord>(url_ ? describeRecord(*url_) : detail::URLRecord{});
    return *record_;
  }

  bool URL::canParse(std::string_view input) { return parseUrl(input).has_value(); }

  bool URL::canParse(std::string_view input, std::string_view base) {
    const auto parsedBase = parseUrl(base);
    return parsedBase && parseUrl(input, &*parsedBase).has_value();
  }

  std::optional<URL> URL::parse(std::string_view input) {
    auto parsed = parseUrl(input);
    if (!parsed) return std::nullopt;
    return URL(std::move(*parsed));
  }

  std::optional<URL> URL::parse(std::string_view input, std::string_view base) {
    const auto parsedBase = parseUrl(base);
    if (!parsedBase) return std::nullopt;
    auto parsed = parseUrl(input, &*parsedBase);
    if (!parsed) return std::nullopt;
    return URL(std::move(*parsed));
  }

  std::string URL::href() const { return url_ ? std::string(url_->get_href()) : std::string{}; }
  void URL::href(std::string_view value) { adopt(parseOrThrow(value)); }

  std::string URL::origin() const {
    if (!url_) return "null";
    auto value = url_->get_origin();
    if (url_->get_protocol() == "blob:" && value != "null" && !parseUrl(value)) return "null";
    return value;
  }

  std::string URL::protocol() const { return url_ ? std::string(url_->get_protocol()) : std::string{}; }
  std::string URL::username() const { return url_ ? std::string(url_->get_username()) : std::string{}; }
  std::string URL::password() const { return url_ ? std::string(url_->get_password()) : std::string{}; }
  std::string URL::host() const { return url_ ? std::string(url_->get_host()) : std::string{}; }
  std::string URL::hostname() const { return url_ ? std::string(url_->get_hostname()) : std::string{}; }
  std::string URL::port() const { return url_ ? std::string(url_->get_port()) : std::string{}; }
  std::string URL::pathname() const { return url_ ? std::string(url_->get_pathname()) : std::string{}; }
  std::string URL::search() const { return url_ ? std::string(url_->get_search()) : std::string{}; }
  std::string URL::hash() const { return url_ ? std::string(url_->get_hash()) : std::string{}; }

  void URL::setComponent(std::string_view value, bool (ada::url_aggregator::*setter)(std::string_view), bool checkHost) {
    const auto limit = checkHost ? limits::kUrlAuthorityBytes : limits::kUrlBytes;
    if (!url_ || value.size() > limit) return;
    std::string storage;
    const auto input = usv(value, storage);
    if (input.size() > limit) return;
    if (checkHost) {
      auto candidate = *url_;
      if (!(candidate.*setter)(input) || !validHost(candidate)) return;
      *url_ = std::move(candidate);
    } else if (!(url_.get()->*setter)(input)) {
      return;
    }
    refreshRecord();
  }

  void URL::protocol(std::string_view value) { setComponent(value, &ada::url_aggregator::set_protocol); }
  void URL::username(std::string_view value) { setComponent(value, &ada::url_aggregator::set_username); }
  void URL::password(std::string_view value) { setComponent(value, &ada::url_aggregator::set_password); }
  void URL::host(std::string_view value) { setComponent(value, &ada::url_aggregator::set_host, true); }
  void URL::hostname(std::string_view value) { setComponent(value, &ada::url_aggregator::set_hostname, true); }
  void URL::port(std::string_view value) { setComponent(value, &ada::url_aggregator::set_port); }
  void URL::pathname(std::string_view value) { setComponent(value, &ada::url_aggregator::set_pathname); }

  void URL::setQuery(std::string_view query) {
    if (!url_) return;
    url_->set_search(query);
    refreshRecord();
  }

  void URL::search(std::string_view value) {
    if (!url_ || !detail::boundedUrlQuery(value)) return;
    std::string storage;
    const auto input = usv(value, storage);
    if (input.size() > limits::kUrlQueryBytes + 1) return;
    auto candidate = *url_;
    candidate.set_search(input);
    if (!detail::boundedUrlQuery(candidate.get_search())) return;
    *url_ = std::move(candidate);
    refreshRecord();
    searchParams_.reset(url_->get_search());
  }

  void URL::hash(std::string_view value) {
    if (!url_ || value.size() > limits::kUrlBytes) return;
    std::string storage;
    const auto input = usv(value, storage);
    if (input.size() > limits::kUrlBytes) return;
    url_->set_hash(input);
    refreshRecord();
  }

  URL pathToFileURL(std::string_view path) {
    if (path.size() > limits::kUrlAuthorityBytes) throw NodeURLError("File URL path exceeds limit", "ERR_INVALID_FILE_URL_PATH");
    auto resolved = path::resolve(path);
    if (resolved.size() > limits::kUrlAuthorityBytes) throw NodeURLError("File URL path exceeds limit", "ERR_INVALID_FILE_URL_PATH");
    if (path.ends_with('/') && !resolved.ends_with('/')) resolved += '/';
    std::string storage;
    const auto input = usv(resolved, storage);
    if (input.size() > limits::kUrlAuthorityBytes) throw NodeURLError("File URL path exceeds limit", "ERR_INVALID_FILE_URL_PATH");
    return URL("file://" + ada::unicode::percent_encode(input, filePathEscapes.data()));
  }

  std::string fileURLToPath(const URL& url) {
    if (url.protocol() != "file:") throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    if (!url.hostname().empty()) throw NodeURLError("File URL host must be \"localhost\" or empty on linux", "ERR_INVALID_FILE_URL_HOST");
    const std::string pathname = url.pathname();
    if (pathname.size() > 3 * limits::kUrlAuthorityBytes) throw NodeURLError("File URL path exceeds limit", "ERR_INVALID_FILE_URL_PATH");
    if (pathname.find("%2f") != std::string::npos || pathname.find("%2F") != std::string::npos)
      throw NodeURLError("File URL path must not include encoded / characters", "ERR_INVALID_FILE_URL_PATH");
    std::string decoded = ada::unicode::percent_decode(pathname, pathname.find('%'));
    if (decoded.size() > limits::kUrlAuthorityBytes) throw NodeURLError("File URL path exceeds limit", "ERR_INVALID_FILE_URL_PATH");
    return isUtf8(decoded) ? std::move(decoded) : std::string{};
  }

  std::string fileURLToPath(std::string_view url) {
    auto parsed = URL::parse(url);
    if (!parsed) throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    return fileURLToPath(*parsed);
  }
}
