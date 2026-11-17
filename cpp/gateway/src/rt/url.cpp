#include "uos/rt/url.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "third_party/ada/ada.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <ranges>
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
#include <unicode/uchar.h>
#endif

namespace uos::rt {
  namespace {
    std::string_view usv(std::string_view input, std::string& storage) {
      if (isUtf8(input)) return input;
      storage = toWellFormed(input);
      return storage;
    }

#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
    bool validJoiners(std::u32string_view label) {
      auto joiningType = [](char32_t c) { return u_getIntPropertyValue(static_cast<UChar32>(c), UCHAR_JOINING_TYPE); };
      auto nearestJoiningType = [&](auto neighbors) {
        for (const char32_t c : neighbors)
          if (const auto type = joiningType(c); type != U_JT_TRANSPARENT) return type;
        return static_cast<int32_t>(U_JT_NON_JOINING);
      };
      for (std::size_t i = 0; i < label.size(); ++i) {
        if (label[i] != U'\u200c' && label[i] != U'\u200d') continue;
        if (i != 0 && u_getCombiningClass(static_cast<UChar32>(label[i - 1])) == 9) continue;
        if (label[i] == U'\u200d') return false;
        const auto before = nearestJoiningType(label.substr(0, i) | std::views::reverse);
        const auto after = nearestJoiningType(label.substr(i + 1));
        if ((before != U_JT_LEFT_JOINING && before != U_JT_DUAL_JOINING) ||
            (after != U_JT_RIGHT_JOINING && after != U_JT_DUAL_JOINING)) return false;
      }
      return true;
    }

    constexpr unsigned bidiMask(std::initializer_list<UCharDirection> directions) {
      unsigned mask = 0;
      for (const auto direction : directions) mask |= 1u << direction;
      return mask;
    }

    // RFC 5893 applies to every label if any label contains RTL text.
    struct BidiDomain {
      bool rtl = false;
      bool valid = true;

      void add(std::u32string_view label) {
        if (label.empty()) return;
        constexpr unsigned rtlTypes = bidiMask({U_RIGHT_TO_LEFT, U_RIGHT_TO_LEFT_ARABIC, U_ARABIC_NUMBER});
        constexpr unsigned neutral = bidiMask({U_EUROPEAN_NUMBER, U_EUROPEAN_NUMBER_SEPARATOR, U_COMMON_NUMBER_SEPARATOR,
                                               U_EUROPEAN_NUMBER_TERMINATOR, U_OTHER_NEUTRAL, U_BOUNDARY_NEUTRAL, U_DIR_NON_SPACING_MARK});
        unsigned types = 0;
        UCharDirection last = U_DIR_NON_SPACING_MARK;
        for (const char32_t c : label) {
          const auto direction = u_charDirection(static_cast<UChar32>(c));
          types |= 1u << direction;
          if (direction != U_DIR_NON_SPACING_MARK) last = direction;
        }
        rtl = rtl || (types & rtlTypes) != 0;
        const auto first = u_charDirection(static_cast<UChar32>(label.front()));
        if (first == U_LEFT_TO_RIGHT) {
          valid = valid && (types & ~(neutral | bidiMask({U_LEFT_TO_RIGHT}))) == 0 &&
                  (last == U_LEFT_TO_RIGHT || last == U_EUROPEAN_NUMBER);
        } else {
          valid = valid && (first == U_RIGHT_TO_LEFT || first == U_RIGHT_TO_LEFT_ARABIC) &&
                  (types & ~(neutral | rtlTypes)) == 0 &&
                  (bidiMask({last}) & (rtlTypes | bidiMask({U_EUROPEAN_NUMBER}))) != 0 &&
                  !((types & bidiMask({U_ARABIC_NUMBER})) && (types & bidiMask({U_EUROPEAN_NUMBER})));
        }
      }
    };
#endif

    bool validHost(const ada::url_aggregator& url) {
      if (!url.is_special()) return true;
      const auto host = url.get_hostname();
      if (host.find("xn--") == std::string_view::npos) return true;

      // Ada's ASCII fast path skips A-label validation. Validate through
      // ToUnicode and check the domain-wide rules that its label parser omits.
      std::u32string codePoints;
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
      BidiDomain bidi;
#endif
      for (const auto part : host | std::views::split('.')) {
        const std::string_view label(part.begin(), part.end());
        if (label.starts_with("xn--")) {
          const std::string decoded = ada::idna::to_unicode(label);
          if (decoded == label || decoded.find('.') != std::string::npos) return false;
          if (std::ranges::any_of(decoded, [](char c) {
                return static_cast<unsigned char>(c) < 0x80 &&
                       ada::idna::contains_forbidden_domain_code_point(std::string_view(&c, 1));
              })) return false;
          codePoints.resize(decoded.size());
          codePoints.resize(ada::idna::utf8_to_utf32(decoded.data(), decoded.size(), codePoints.data()));
          if (codePoints.empty()) return false;
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
          if (!validJoiners(codePoints)) return false;
#else
          // The SDK comparison profile has no Unicode property library.
          if (std::ranges::any_of(codePoints, [](char32_t c) { return c > 0xff; })) return false;
#endif
        } else {
          codePoints.assign(label.begin(), label.end());
        }
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
        bidi.add(codePoints);
#endif
      }
#ifdef UOS_GATEWAY_NATIVE_LIBRARIES
      return !bidi.rtl || bidi.valid;
#else
      return true;
#endif
    }

    ada::result<ada::url_aggregator> parseUrl(std::string_view input, const ada::url_aggregator* base = nullptr) {
      std::string storage;
      auto parsed = ada::parse(usv(input, storage), base);
      if (parsed && !validHost(*parsed)) return tl::unexpected(ada::errors::type_error);
      return parsed;
    }

    ada::url_aggregator parseOrThrow(std::string_view input, const ada::url_aggregator* base = nullptr) {
      auto parsed = parseUrl(input, base);
      if (!parsed) throw InvalidURLError(std::string(input));
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
    if (query.empty()) params_.reset();
    else data().reset(query);
  }

  void URLSearchParams::update() {
    if (url_) url_->setQuery(toString());
  }

  std::size_t URLSearchParams::size() const { return params_ ? params_->size() : 0; }

  void URLSearchParams::append(std::string_view name, std::string_view value) {
    std::string keyStorage, valueStorage;
    data().append(usv(name, keyStorage), usv(value, valueStorage));
    update();
  }

  void URLSearchParams::delete_(std::string_view name) {
    std::string storage;
    if (params_) params_->remove(usv(name, storage));
    update();
  }

  void URLSearchParams::delete_(std::string_view name, std::string_view value) {
    std::string keyStorage, valueStorage;
    if (params_) params_->remove(usv(name, keyStorage), usv(value, valueStorage));
    update();
  }

  std::optional<std::string> URLSearchParams::get(std::string_view name) const {
    std::string storage;
    if (params_)
      if (const auto value = params_->get(usv(name, storage))) return std::string(*value);
    return std::nullopt;
  }

  std::vector<std::string> URLSearchParams::getAll(std::string_view name) const {
    std::string storage;
    return params_ ? params_->get_all(usv(name, storage)) : std::vector<std::string>{};
  }

  bool URLSearchParams::has(std::string_view name) const {
    std::string storage;
    return params_ && params_->has(usv(name, storage));
  }

  bool URLSearchParams::has(std::string_view name, std::string_view value) const {
    std::string keyStorage, valueStorage;
    return params_ && params_->has(usv(name, keyStorage), usv(value, valueStorage));
  }

  void URLSearchParams::set(std::string_view name, std::string_view value) {
    std::string keyStorage, valueStorage;
    data().set(usv(name, keyStorage), usv(value, valueStorage));
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
    if (!parsedBase) throw InvalidURLError(std::string(input));
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
    if (!url_) return;
    std::string storage;
    const auto input = usv(value, storage);
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
    if (!url_) return;
    std::string storage;
    setQuery(usv(value, storage));
    searchParams_.reset(url_->get_search());
  }

  void URL::hash(std::string_view value) {
    if (!url_) return;
    std::string storage;
    url_->set_hash(usv(value, storage));
    refreshRecord();
  }

  URL pathToFileURL(std::string_view path) {
    auto resolved = path::resolve(path);
    if (path.ends_with('/') && !resolved.ends_with('/')) resolved += '/';
    std::string storage;
    return URL("file://" + ada::unicode::percent_encode(usv(resolved, storage), filePathEscapes.data()));
  }

  std::string fileURLToPath(const URL& url) {
    if (url.protocol() != "file:") throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    if (!url.hostname().empty()) throw NodeURLError("File URL host must be \"localhost\" or empty on linux", "ERR_INVALID_FILE_URL_HOST");
    const std::string pathname = url.pathname();
    for (std::size_t index = 0; index + 2 < pathname.size(); ++index) {
      if (pathname[index] == '%' && pathname[index + 1] == '2' && (pathname[index + 2] | 0x20) == 'f')
        throw NodeURLError("File URL path must not include encoded / characters", "ERR_INVALID_FILE_URL_PATH");
    }
    std::string decoded = ada::unicode::percent_decode(pathname, pathname.find('%'));
    return isUtf8(decoded) ? std::move(decoded) : std::string{};
  }

  std::string fileURLToPath(std::string_view url) {
    auto parsed = URL::parse(url);
    if (!parsed) throw NodeURLError("The URL must be of scheme file", "ERR_INVALID_URL_SCHEME");
    return fileURLToPath(*parsed);
  }
}
