#pragma once

#include "uos/rt/http.hpp"
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::test::image_multipart {
  struct FixtureFile {
    rt::Bytes bytes;
    std::string filename;
    std::string contentType;
  };
  using FixtureValue = std::variant<std::string, FixtureFile>;
  using MultipartFixture = std::vector<std::pair<std::string, FixtureValue>>;
  inline constexpr std::string_view boundary = "cppgw-image-fixture";

  inline FixtureFile file(rt::Bytes bytes, std::string filename = "source.png", std::string contentType = "image/png") {
    return {std::move(bytes), std::move(filename), std::move(contentType)};
  }

  inline MultipartFixture editForm() { return {{"prompt", "edit the source"}, {"image", file({1})}}; }

  inline void putPart(MultipartFixture& parts, std::string_view name, FixtureValue value) {
    const auto found = std::ranges::find(parts, name, &MultipartFixture::value_type::first);
    if (found == parts.end()) parts.emplace_back(name, std::move(value));
    else found->second = std::move(value);
  }

  inline void removePart(MultipartFixture& parts, std::string_view name) {
    std::erase_if(parts, [name](const auto& part) { return part.first == name; });
  }

  inline rt::Bytes multipartBody(const MultipartFixture& parts) {
    rt::Bytes body;
    const auto append = [&body](std::string_view bytes) { body.insert(body.end(), bytes.begin(), bytes.end()); };
    for (const auto& [name, value] : parts) {
      append("--");
      append(boundary);
      append("\r\nContent-Disposition: form-data; name=\"");
      append(name);
      append("\"");
      if (const auto* uploaded = std::get_if<FixtureFile>(&value)) {
        append("; filename=\"");
        append(uploaded->filename);
        append("\"\r\nContent-Type: ");
        append(uploaded->contentType.empty() ? std::string_view("application/octet-stream") : std::string_view(uploaded->contentType));
        append("\r\n\r\n");
        body.insert(body.end(), uploaded->bytes.begin(), uploaded->bytes.end());
      } else {
        append("\r\n\r\n");
        append(std::get<std::string>(value));
      }
      append("\r\n");
    }
    append("--");
    append(boundary);
    append("--\r\n");
    return body;
  }

  inline rt::Headers multipartHeaders() {
    return {{"content-type", "multipart/form-data; boundary=" + std::string(boundary)}};
  }

  inline rt::Request multipartRequest(const MultipartFixture& parts) {
    rt::RequestInit init{.method = "POST", .headers = multipartHeaders(), .body = multipartBody(parts)};
    return rt::Request("http://127.0.0.1/v1/images/edits", std::move(init));
  }
}
