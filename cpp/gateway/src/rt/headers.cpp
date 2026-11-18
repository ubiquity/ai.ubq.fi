#include "uos/rt/headers.hpp"
#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::rt {
  namespace {
    bool isTokenCharacter(char c) {
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return true;
      return std::string_view("!#$%&'*+-.^_`|~").find(c) != std::string_view::npos;
    }

    std::string normalizedName(std::string_view name, bool forHas = false) {
      if (name.empty() || !std::ranges::all_of(name, isTokenCharacter))
        throw TypeError("Invalid header name: '" + std::string(name) + (forHas ? "\"" : "'"));
      std::string lower(name);
      for (char& c : lower)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
      return lower;
    }

    // Strip HTTP whitespace, then check that the value is a ByteString without NUL, LF or CR.
    std::string normalizedValue(std::string_view name, std::string_view value) {
      constexpr std::string_view whitespace = " \t\r\n";
      const auto start = value.find_first_not_of(whitespace);
      if (start == std::string_view::npos) return {};
      const auto stripped = value.substr(start, value.find_last_not_of(whitespace) - start + 1);
      for (std::size_t index = 0; index < stripped.size(); ++index) {
        const auto byte = static_cast<unsigned char>(stripped[index]);
        bool valid = byte != 0x00 && byte != 0x0A && byte != 0x0D;
        if (byte >= 0x80) {
          valid = (byte == 0xC2 || byte == 0xC3) && index + 1 < stripped.size() && (static_cast<unsigned char>(stripped[index + 1]) & 0xC0) == 0x80;
          ++index;
        }
        if (!valid) throw TypeError("Header '" + std::string(name.substr(0, 64)) + "' has invalid value");
      }
      return std::string(stripped);
    }

    constexpr std::string_view kSetCookie = "set-cookie";
    constexpr std::string_view kCookie = "cookie";
  }

  Headers::Headers(std::initializer_list<Entry> init) {
    for (const auto& [name, value] : init) append(name, value);
  }

  Headers::Headers(const std::vector<Entry>& init) {
    for (const auto& [name, value] : init) append(name, value);
  }

  Headers Headers::fromSequence(const std::vector<std::vector<std::string>>& sequence) {
    Headers headers;
    for (const auto& item : sequence) {
      if (item.size() != 2) throw TypeError("Header sub-sequence must contain exactly two items");
      headers.append(item[0], item[1]);
    }
    return headers;
  }

  auto Headers::findField(auto& fields, std::string_view key) {
    const auto position = std::ranges::lower_bound(fields, key, {}, &Field::name);
    return std::pair{position, position != fields.end() && position->name == key};
  }

  void Headers::write(std::string_view name, std::string_view value, bool append) {
    std::string key = normalizedName(name);
    std::string normalized = normalizedValue(key, value);
    if (key == kSetCookie) {
      if (!append) cookies_.clear();
      cookies_.push_back(std::move(normalized));
      return;
    }
    const auto [position, found] = findField(fields_, key);
    if (!found) {
      fields_.insert(position, Field{std::move(key), std::string(name), std::move(normalized)});
    } else if (append) {
      position->value += key == kCookie ? "; " : ", ";
      position->value += normalized;
    } else {
      position->value = std::move(normalized);
    }
  }

  void Headers::append(std::string_view name, std::string_view value) { write(name, value, true); }
  void Headers::set(std::string_view name, std::string_view value) { write(name, value, false); }

  void Headers::delete_(std::string_view name) {
    const std::string key = normalizedName(name);
    if (key == kSetCookie) {
      cookies_.clear();
      return;
    }
    const auto [position, found] = findField(fields_, key);
    if (found) fields_.erase(position);
  }

  std::optional<std::string> Headers::get(std::string_view name) const {
    const std::string key = normalizedName(name);
    if (key == kSetCookie) {
      if (cookies_.empty()) return std::nullopt;
      std::string joined = cookies_.front();
      for (std::size_t index = 1; index < cookies_.size(); ++index) {
        joined += ", ";
        joined += cookies_[index];
      }
      return joined;
    }
    const auto [position, found] = findField(fields_, key);
    if (found) return position->value;
    return std::nullopt;
  }

  std::vector<std::string> Headers::getSetCookie() const { return cookies_; }

  bool Headers::has(std::string_view name) const {
    const std::string key = normalizedName(name, true);
    return key == kSetCookie ? !cookies_.empty() : findField(fields_, key).second;
  }

  Headers::Entry Headers::entryAt(std::size_t index) const {
    if (index < fields_.size()) return {fields_[index].name, fields_[index].value};
    return Entry(std::string(kSetCookie), cookies_[index - fields_.size()]);
  }

  std::vector<Headers::Entry> Headers::entries() const {
    std::vector<Entry> out;
    out.reserve(count());
    for (std::size_t index = 0; index < count(); ++index) out.push_back(entryAt(index));
    return out;
  }

  std::vector<std::string> Headers::keys() const {
    std::vector<std::string> out;
    out.reserve(count());
    for (const auto& field : fields_) out.push_back(field.name);
    out.insert(out.end(), cookies_.size(), std::string(kSetCookie));
    return out;
  }

  std::vector<std::string> Headers::values() const {
    std::vector<std::string> out;
    out.reserve(count());
    for (const auto& field : fields_) out.push_back(field.value);
    out.insert(out.end(), cookies_.begin(), cookies_.end());
    return out;
  }

  namespace detail {
    std::vector<std::pair<std::string, std::string>> headerWireFields(const Headers& headers) {
      std::vector<std::pair<std::string, std::string>> out;
      out.reserve(headers.fields_.size());
      for (const auto& field : headers.fields_) out.emplace_back(field.spelling, field.value);
      return out;
    }
  }

  void Headers::forEach(const std::function<void(const std::string& value, const std::string& name)>& callback) const {
    for (const auto& [name, value] : *this) callback(value, name);
  }
}
