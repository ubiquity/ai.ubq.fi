// OpenAI-compatible image endpoints, extracted from src/openai.ts.
#include "uos/images.hpp"
#include "uos/bounded_response_body.hpp"
#include "uos/http.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/openai.hpp"
#include "uos/request.hpp"
#include "uos/request_policy.hpp"
#include "uos/responses_handler.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"
#include <openssl/crypto.h>
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <variant>

namespace uos::images {
namespace {
  namespace tel = openai_telemetry;
  constexpr std::string_view IMAGE_TOOL_TYPE = "image_generation";
  constexpr std::string_view IMAGE_EDIT_DEFAULT_MODEL = "gpt-image-1.5";
  constexpr double IMAGE_MAX_COUNT = 10;
  constexpr double IMAGE_MAX_EDIT_INPUTS = 16;
  constexpr double IMAGE_MAX_PROMPT_CHARS = 32'000;
  constexpr double IMAGE_MAX_REFERENCE_URL_CHARS = 20'971'520;
  constexpr double IMAGE_MAX_FILE_BYTES = static_cast<double>(rt::limits::kImageEditInputBytes);
  constexpr double IMAGE_MAX_MASK_FILE_BYTES = IMAGE_MAX_FILE_BYTES;
  constexpr double IMAGE_MAX_MULTIPART_INPUT_BYTES = IMAGE_MAX_FILE_BYTES;
  constexpr double IMAGE_MAX_MULTIPART_BODY_BYTES = static_cast<double>(rt::limits::kImageEditMultipartBytes);
  constexpr double IMAGE_MAX_FANOUT_INPUT_BYTES = IMAGE_MAX_FILE_BYTES;
  constexpr double IMAGE_MAX_JSON_BODY_BYTES = static_cast<double>(rt::limits::kImageEditJsonBytes);
  const std::array<std::pair<std::string_view, std::string_view>, 4> IMAGE_MEDIA_TYPE_BY_EXTENSION{{
    {".jpeg", "image/jpeg"}, {".jpg", "image/jpeg"}, {".png", "image/png"}, {".webp", "image/webp"}}};
  const std::array<std::pair<std::string_view, std::string_view>, 5> IMAGE_MEDIA_TYPE_ALIASES{{
    {"image/jpeg", "image/jpeg"}, {"image/jpg", "image/jpeg"}, {"image/png", "image/png"}, {"image/webp", "image/webp"}, {"image/x-png", "image/png"}}};
  const std::array<std::string_view, 13> IMAGE_NULLABLE_OPTION_KEYS{"model", "n", "size", "quality", "background", "output_format",
    "output_compression", "input_fidelity", "moderation", "response_format", "stream", "partial_images", "style"};
  const rt::OrderedSet<std::string> IMAGE_GENERATION_QUALITIES{"low", "medium", "high", "auto"};
  const rt::OrderedSet<std::string> IMAGE_EDIT_JSON_QUALITIES{"low", "medium", "high", "auto"};
  const rt::OrderedSet<std::string> IMAGE_EDIT_MULTIPART_QUALITIES{"low", "medium", "high", "auto"};
  const rt::OrderedSet<std::string> IMAGE_BACKGROUNDS{"transparent", "opaque", "auto"};
  const rt::OrderedSet<std::string> IMAGE_OUTPUT_FORMATS{"png", "jpeg", "webp"};
  const rt::OrderedSet<std::string> IMAGE_MODERATION_LEVELS{"low", "auto"};
  const rt::OrderedSet<std::string> IMAGE_INPUT_FIDELITIES{"low", "high"};
  std::optional<std::string> imageBaseModelForTest;
  const rt::OrderedSet<std::string> IMAGE_GENERATION_REQUEST_KEYS{"model", "prompt", "n", "size", "quality", "background", "output_format",
    "output_compression", "stream", "partial_images", "user", "moderation", "response_format", "style"};
  const rt::OrderedSet<std::string> IMAGE_EDIT_JSON_REQUEST_KEYS{"model", "prompt", "n", "size", "quality", "background", "output_format",
    "output_compression", "stream", "partial_images", "user", "images", "mask", "input_fidelity", "moderation"};
  const rt::OrderedSet<std::string> IMAGE_EDIT_MULTIPART_REQUEST_KEYS{"model", "prompt", "n", "size", "quality", "background", "output_format",
    "output_compression", "stream", "partial_images", "user", "image", "image[]", "mask", "input_fidelity", "response_format"};
  struct ImageRequestFailure { rt::Response response; };
  struct ParsedImageRequestOk { json::object body; double count; double inputBytes; };
  using ParsedImageRequest = std::variant<ParsedImageRequestOk, ImageRequestFailure>;
  bool isPlainRecord(const json::value* value) { return value && value->is_object(); }
  ImageRequestFailure imageRequestError(const std::string& message, std::optional<std::string> param = std::nullopt) {
    http::OpenaiErrorOptions options; options.param = param;
    return {http::openaiError(400, message, "invalid_request_error", options)};
  }
  struct ImageDataUrl { std::string url; double bytes; std::string mediaType; };
  std::optional<ImageDataUrl> normalizeImageDataUrl(const std::string& value);
  std::optional<json::object> normalizeImageReference(const json::value* value) {
    if (!isPlainRecord(value)) return std::nullopt;
    const auto& object = value->as_object();
    for (const auto& key : rt::objectKeys(object)) if (key != "image_url") return std::nullopt;
    const auto reference = rt::getString(object, "image_url");
    if (!reference || static_cast<double>(rt::length(*reference)) > IMAGE_MAX_REFERENCE_URL_CHARS) return std::nullopt;
    const auto imageUrl = rt::trim(*reference); if (imageUrl.empty()) return std::nullopt;
    static const rt::Regex data(R"(^data:)", "iu"); static const rt::Regex remote(R"(^https?://)", "iu");
    if (!data.test(imageUrl)) {
      if (!remote.test(imageUrl)) return std::nullopt;
      try { const rt::URL remoteUrl(imageUrl); if (remoteUrl.protocol() == "http:" || remoteUrl.protocol() == "https:") return json::object{{"image_url", imageUrl}}; }
      catch (...) {} return std::nullopt;
    }
    const auto inlineData = normalizeImageDataUrl(imageUrl);
    return inlineData ? std::optional(json::object{{"image_url", inlineData->url}}) : std::nullopt;
  }
  std::optional<json::object> normalizeImageMaskReference(const json::value* value) {
    const auto reference = normalizeImageReference(value); if (!reference) return std::nullopt;
    const auto inlineData = normalizeImageDataUrl(std::string(*rt::getString(*reference, "image_url")));
    if (inlineData && inlineData->mediaType == "image/png" && inlineData->bytes < IMAGE_MAX_MASK_FILE_BYTES) return json::object{{"image_url", inlineData->url}};
    return std::nullopt;
  }
  std::optional<std::string> normalizeImageMediaType(const std::string& value) {
    const auto trimmed = rt::trim(std::string_view(value).substr(0, value.find(';')));
    if (trimmed.size() > rt::limits::kIcuInputBytes) return std::nullopt;
    const auto mediaType = rt::toLowerCase(trimmed);
    for (const auto& [alias, normalized] : IMAGE_MEDIA_TYPE_ALIASES) if (mediaType == alias) return std::string(normalized);
    return std::nullopt;
  }
  std::optional<ImageDataUrl> normalizeImageDataUrl(const std::string& value) {
    static const rt::Regex pattern(R"(^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$)", "iu");
    const auto match = pattern.exec(value); if (!match) return std::nullopt;
    const auto mediaType = normalizeImageMediaType(*(*match)[1]); const auto encoded = *(*match)[2];
    const auto length = static_cast<double>(rt::length(encoded));
    if (!mediaType || encoded.empty() || rt::mod(length, 4) != 0) return std::nullopt;
    const double padding = rt::endsWith(encoded, "==") ? 2 : rt::endsWith(encoded, "=") ? 1 : 0;
    const auto bytes = length / 4 * 3 - padding;
    if (bytes <= 0 || bytes >= IMAGE_MAX_FILE_BYTES) return std::nullopt;
    return ImageDataUrl{"data:" + *mediaType + ";base64," + encoded, bytes, *mediaType};
  }
  double imageReferenceInputBytes(const json::object& body) {
    json::array references; if (const auto* images = rt::getArray(body, "images")) references = *images;
    const auto* mask = rt::get(body, "mask"); if (isPlainRecord(mask)) references.push_back(*mask);
    double total = 0;
    for (const auto& reference : references) {
      if (!isPlainRecord(&reference)) continue;
      if (const auto imageUrl = rt::getString(reference, "image_url")) {
        const auto inlineData = normalizeImageDataUrl(std::string(*imageUrl));
        total += inlineData ? inlineData->bytes : static_cast<double>(rt::utf8ByteLength(*imageUrl));
      }
    }
    return total;
  }
  std::optional<std::string> imageFileMediaType(const rt::FormFile& file) {
    const auto declared = rt::toLowerCase(rt::trim(file.mediaType));
    if (!declared.empty() && declared != "application/octet-stream") return normalizeImageMediaType(declared);
    if (!file.filename) throw rt::TypeError("undefined is not an object (evaluating 'file.name.trim')");
    const auto normalizedName = rt::toLowerCase(rt::trim(*file.filename));
    for (const auto& [extension, mediaType] : IMAGE_MEDIA_TYPE_BY_EXTENSION) if (rt::endsWith(normalizedName, extension)) return std::string(mediaType);
    return std::nullopt;
  }
  std::string fileDataUrl(const rt::FormFile& file, const std::string& mediaType) {
    return "data:" + mediaType + ";base64," + utils::bytesToBase64(file.bytes);
  }
  json::value multipartValue(const rt::FormValue& value) {
    if (const auto* string = std::get_if<std::string>(&value)) return json::value(*string);
    return json::object{};
  }
  json::value parseMultipartInteger(const rt::FormValue& value) {
    if (const auto* string = std::get_if<std::string>(&value)) {
      const auto normalized = rt::trim(*string); static const rt::Regex pattern(R"(^(?:0|[1-9]\d*)$)", "u");
      if (normalized.size() <= rt::limits::kRegexSubjectBytes && pattern.test(normalized)) return rt::makeNumber(rt::stringToNumber(normalized));
    }
    return multipartValue(value);
  }
  json::value parseMultipartNumber(const rt::FormValue& value) {
    if (const auto* string = std::get_if<std::string>(&value)) {
      const auto normalized = rt::trim(*string); static const rt::Regex pattern(R"(^-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?$)", "u");
      if (normalized.size() <= rt::limits::kRegexSubjectBytes && pattern.test(normalized)) { const auto parsed = rt::stringToNumber(normalized); if (rt::Number::isFinite(parsed)) return rt::makeNumber(parsed); }
    }
    return multipartValue(value);
  }
  bool exceedsUnicodeCodePointLimit(const std::string& value, double limit) {
    double count = 0; for ([[maybe_unused]] const auto cp : rt::codePoints(value)) if (++count > limit) return true; return false;
  }
  json::value parseMultipartBoolean(const rt::FormValue& value) {
    if (const auto* string = std::get_if<std::string>(&value)) {
      const auto normalized = rt::trim(*string); if (normalized == "true") return true; if (normalized == "false") return false;
    }
    return multipartValue(value);
  }
  std::optional<ImageRequestFailure> imageEnumOptionError(const json::object& raw, const std::string& key,
    const rt::OrderedSet<std::string>& allowed, const std::string& message) {
    const auto* value = rt::get(raw, key); if (!value) return std::nullopt;
    if (value->is_string() && allowed.has(std::string(value->as_string()))) return std::nullopt;
    return imageRequestError(message, key);
  }
  std::optional<ImageRequestFailure> imageScalarIdentityFailure(const json::object& raw) {
    if (rt::hasOwn(raw, "style")) return imageRequestError("style is not supported because this gateway uses the Responses image-generation tool.", "style");
    const auto prompt = rt::getString(raw, "prompt");
    if (!prompt || rt::trim(*prompt).empty()) return imageRequestError("Image requests must include a prompt.", "prompt");
    if (exceedsUnicodeCodePointLimit(std::string(*prompt), IMAGE_MAX_PROMPT_CHARS)) return imageRequestError("prompt must contain no more than 32000 characters.", "prompt");
    const auto model = rt::getString(raw, "model");
    if (rt::hasOwn(raw, "model") && (!model || rt::trim(*model).empty())) return imageRequestError("model must be a non-empty string.", "model");
    const auto* user = rt::get(raw, "user"); if (user && !user->is_string()) return imageRequestError("user must be a string.", "user");
    return std::nullopt;
  }
  std::optional<ImageRequestFailure> imageScalarPrecisionFailure(const json::object& raw) {
    if (const auto* compression = rt::get(raw, "output_compression"); compression &&
      (!rt::Number::isInteger(*compression) || rt::toNumber(compression) < 0 || rt::toNumber(compression) > 100))
      return imageRequestError("output_compression must be an integer from 0 to 100.", "output_compression");
    const auto* size = rt::get(raw, "size"); if (size && (!size->is_string() || rt::trim(size->as_string()).empty())) return imageRequestError("size must be a non-empty string.", "size");
    return std::nullopt;
  }
  const rt::OrderedSet<std::string>& imageEditQualities(bool multipart) { return multipart ? IMAGE_EDIT_MULTIPART_QUALITIES : IMAGE_EDIT_JSON_QUALITIES; }
  const rt::OrderedSet<std::string>& imageQualities(const ImageRouteKind& kind, bool multipart) { return kind == "generations" ? IMAGE_GENERATION_QUALITIES : imageEditQualities(multipart); }
  std::optional<ImageRequestFailure> imageScalarEnumerationFailure(const json::object& raw, const ImageRouteKind& kind, bool multipart) {
    auto failure = imageEnumOptionError(raw, "quality", imageQualities(kind, multipart), "quality must be low, medium, high, or auto" + std::string(kind == "edits" ? " for image edits" : "") + "."); if (failure) return failure;
    failure = imageEnumOptionError(raw, "background", IMAGE_BACKGROUNDS, "background must be transparent, opaque, or auto."); if (failure) return failure;
    failure = imageEnumOptionError(raw, "output_format", IMAGE_OUTPUT_FORMATS, "output_format must be png, jpeg, or webp."); if (failure) return failure;
    failure = imageEnumOptionError(raw, "moderation", IMAGE_MODERATION_LEVELS, "moderation must be low or auto."); if (failure) return failure;
    return imageEnumOptionError(raw, "input_fidelity", IMAGE_INPUT_FIDELITIES, "input_fidelity must be low or high.");
  }
  std::optional<ImageRequestFailure> imageScalarStreamingFailure(const json::object& raw) {
    const auto* stream = rt::get(raw, "stream");
    if (stream && !stream->is_bool()) return imageRequestError("stream must be a boolean.", "stream");
    if (rt::getBool(raw, "stream").value_or(false) || rt::hasOwn(raw, "partial_images"))
      return imageRequestError("Streaming image responses are not supported by this gateway.", rt::getBool(raw, "stream").value_or(false) ? "stream" : "partial_images");
    const auto* format = rt::get(raw, "response_format");
    if (format && rt::getString(raw, "response_format") != "b64_json") return imageRequestError("response_format must be b64_json.", "response_format");
    return std::nullopt;
  }
  using ImageScalarValidationResult = std::variant<double, ImageRequestFailure>;
  ImageScalarValidationResult validateImageScalarOptions(const json::object& raw, const ImageRouteKind& kind, bool multipart = false) {
    if (const auto failure = imageScalarIdentityFailure(raw)) return *failure;
    const auto* n = rt::get(raw, "n"); const auto count = n ? rt::toNumber(n) : 1;
    if ((n && !rt::Number::isInteger(*n)) || count < 1 || count > IMAGE_MAX_COUNT) return imageRequestError("n must be an integer from 1 to 10.", "n");
    if (const auto failure = imageScalarPrecisionFailure(raw)) return *failure;
    if (const auto failure = imageScalarEnumerationFailure(raw, kind, multipart)) return *failure;
    if (const auto failure = imageScalarStreamingFailure(raw)) return *failure;
    return count;
  }
  json::object withoutNullImageOptions(const json::object& raw) {
    json::object body;
    for (const auto* entry : rt::objectEntries(raw)) {
      const auto key = std::string(entry->key()); const auto* value = &entry->value();
      if (value->is_null() && std::find(IMAGE_NULLABLE_OPTION_KEYS.begin(), IMAGE_NULLABLE_OPTION_KEYS.end(), key) != IMAGE_NULLABLE_OPTION_KEYS.end()) continue;
      rt::objectDefine(body, key, *value);
    }
    return body;
  }
  rt::Task<std::optional<ImageRequestFailure>> rejectInvalidImageEditContentLength(rt::Request req, std::string declaredLength) {
    const auto normalizedLength = rt::trim(declaredLength); static const rt::Regex pattern(R"(^(?:0|[1-9]\d*)$)", "u");
    if (!pattern.test(normalizedLength)) { request::rejectRawBodyOnce(req, "body_unavailable"); co_return imageRequestError("Multipart Content-Length is invalid."); }
    const auto parsedLength = rt::stringToNumber(normalizedLength);
    if (!rt::Number::isSafeInteger(parsedLength) || parsedLength > IMAGE_MAX_MULTIPART_BODY_BYTES) {
      request::rejectRawBodyOnce(req, "body_over_limit");
      if (const auto body = req.body()) { try { auto task = body->cancel(); rt::Promise<void> pending(std::move(task)); co_await pending.wait(); } catch (...) {} }
      else co_await rt::microtask();
      co_return imageRequestError("Multipart image edits must be no larger than 64 MiB.", "image");
    }
    co_return std::nullopt;
  }
  std::optional<ImageRequestFailure> collectImageEditFields(const rt::FormParts& form, json::object& body) {
    for (const auto& [key, value] : form) {
      if (IMAGE_EDIT_MULTIPART_REQUEST_KEYS.has(key)) continue;
      if (!key.empty()) return imageRequestError("Unsupported image edit field: " + key + ".", key);
      break;
    }
    for (const auto key : {"model", "prompt", "size", "quality", "background", "output_format", "input_fidelity", "response_format", "user"}) {
      const auto* value = rt::findFormPart(form, key); if (!value) continue;
      if (!std::holds_alternative<std::string>(*value)) return imageRequestError(std::string(key) + " must be a string.", key);
      rt::objectSet(body, key, json::value(std::get<std::string>(*value)));
    }
    if (const auto* stream = rt::findFormPart(form, "stream")) rt::objectSet(body, "stream", parseMultipartBoolean(*stream));
    for (const auto key : {"n", "partial_images"}) if (const auto* value = rt::findFormPart(form, key)) rt::objectSet(body, key, parseMultipartInteger(*value));
    if (const auto* value = rt::findFormPart(form, "output_compression")) rt::objectSet(body, "output_compression", parseMultipartNumber(*value));
    return std::nullopt;
  }
  struct ImageEditFiles { std::vector<const rt::FormFile*> imageFiles; const rt::FormFile* maskFile = nullptr; };
  std::variant<ImageEditFiles, ImageRequestFailure> selectImageEditFiles(const rt::FormParts& form) {
    ImageEditFiles result;
    std::size_t maskCount = 0;
    for (const auto& [name, value] : form) {
      if (name == "image" || name == "image[]") {
        if (static_cast<double>(result.imageFiles.size()) == IMAGE_MAX_EDIT_INPUTS)
          return imageRequestError("image must contain from 1 to 16 files.", "image");
        result.imageFiles.push_back(std::get_if<rt::FormFile>(&value));
      } else if (name == "mask") {
        ++maskCount;
        result.maskFile = std::get_if<rt::FormFile>(&value);
      }
    }
    if (result.imageFiles.empty()) return imageRequestError("image must contain from 1 to 16 files.", "image");
    if (std::ranges::find(result.imageFiles, nullptr) != result.imageFiles.end()) return imageRequestError("Each multipart image entry must be a file.", "image");
    if (maskCount > 1 || (maskCount == 1 && !result.maskFile)) return imageRequestError("mask must be one image file.", "mask");
    return result;
  }
  using ImageEditMask = std::variant<std::optional<std::string>, ImageRequestFailure>;
  ImageEditMask validateImageEditMask(const rt::FormFile* maskFile) {
    if (!maskFile) return std::optional<std::string>();
    const auto mediaType = imageFileMediaType(*maskFile);
    if (maskFile->bytes.empty() || static_cast<double>(maskFile->bytes.size()) >= IMAGE_MAX_MASK_FILE_BYTES || mediaType != "image/png") return imageRequestError("The multipart mask must be a non-empty PNG file smaller than 50 MiB.", "mask");
    return mediaType;
  }
  ParsedImageRequest finalizeImageEditBody(const rt::FormParts& form, json::object body, double count) {
    const auto selection = selectImageEditFiles(form); if (const auto* failure = std::get_if<ImageRequestFailure>(&selection)) return *failure;
    const auto& selected = std::get<ImageEditFiles>(selection); const auto& imageFiles = selected.imageFiles; const auto& maskFile = selected.maskFile;
    for (const auto* file : imageFiles) if (file->bytes.empty() || static_cast<double>(file->bytes.size()) >= IMAGE_MAX_FILE_BYTES)
      return imageRequestError("Each multipart image file must be non-empty and smaller than 50 MiB.", "image");
    std::vector<std::pair<const rt::FormFile*, std::string>> imageInputs;
    for (const auto* file : imageFiles) if (const auto mediaType = imageFileMediaType(*file)) imageInputs.emplace_back(file, *mediaType);
    if (imageInputs.size() != imageFiles.size()) return imageRequestError("Each multipart image must be a PNG, JPEG, or WebP file.", "image");
    const auto mask = validateImageEditMask(maskFile); if (const auto* failure = std::get_if<ImageRequestFailure>(&mask)) return *failure;
    const auto& maskMediaType = std::get<std::optional<std::string>>(mask);
    double totalInputBytes = maskFile ? static_cast<double>(maskFile->bytes.size()) : 0;
    for (const auto* file : imageFiles) totalInputBytes += static_cast<double>(file->bytes.size());
    if (totalInputBytes > IMAGE_MAX_MULTIPART_INPUT_BYTES) return imageRequestError("Multipart image files must total no more than 50 MiB.", "image");
    if (totalInputBytes > rt::Math::floor(IMAGE_MAX_FANOUT_INPUT_BYTES / count)) return imageRequestError("n and multipart image inputs exceed the 50 MiB request work limit.", "n");
    json::array images;
    for (const auto& [file, mediaType] : imageInputs) images.push_back(json::object{{"image_url", fileDataUrl(*file, mediaType)}});
    rt::objectSet(body, "images", images);
    if (maskFile && maskMediaType) rt::objectSet(body, "mask", json::object{{"image_url", fileDataUrl(*maskFile, *maskMediaType)}});
    return ParsedImageRequestOk{std::move(body), count, totalInputBytes};
  }
  rt::Task<ParsedImageRequest> parseMultipartImageEdit(rt::Request req) {
    if (const auto length = req.headers().get("content-length")) {
      auto task = rejectInvalidImageEditContentLength(req, *length); rt::Promise<std::optional<ImageRequestFailure>> pending(std::move(task));
      const auto failure = co_await pending.wait(); if (failure) co_return *failure;
    }
    std::shared_ptr<rt::Bytes> bytes; bool captured = false;
    const auto cleanup = rt::scopeExit([&] { request::discardRawBodyObserverOnce(req); if (bytes && !captured && !bytes->empty()) OPENSSL_cleanse(bytes->data(), bytes->size()); });
    const rt::Response source(rt::bodyInit(req.body()));
    bounded_response_body::ReadBoundedResponseBodyOptions options;
    options.maxBytes = IMAGE_MAX_MULTIPART_BODY_BYTES + 1; options.timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS;
    options.signal = req.signal(); options.cancellationReason = "Multipart image request exceeded its read limit";
    auto readTask = bounded_response_body::readBoundedResponseBody(source, options); rt::Promise<bounded_response_body::BoundedResponseBody> readPending(std::move(readTask));
    auto bounded = co_await readPending.wait(); bytes = std::make_shared<rt::Bytes>(std::move(bounded.bytes));
    const bool overLimit = static_cast<double>(bytes->size()) > IMAGE_MAX_MULTIPART_BODY_BYTES;
    if (!bounded.complete || overLimit) {
      request::rejectRawBodyOnce(req, overLimit ? "body_over_limit" : bytes->empty() ? "body_unavailable" : "body_partial");
      co_return imageRequestError("Multipart image edits must be no larger than 64 MiB.", "image");
    }
    const auto reportReceivedStatus = request::receivedRawBodyRejection(req);
    captured = request::captureRawBodyOnce(req, bytes, request::RawBodyStage::received);
    rt::FormParts form;
    try {
      const rt::InboundFormDecoder decoder(req.headers().get("content-type").value_or(""));
      form = decoder.decode(*bytes);
    } catch (...) {
      if (captured && reportReceivedStatus) reportReceivedStatus(bytes->empty() ? "body_empty" : "body_invalid");
      co_return imageRequestError("Image edits must include valid multipart form data.");
    }
    if (!captured) captured = request::captureRawBodyOnce(req, bytes);
    json::object body;
    if (const auto failure = collectImageEditFields(form, body)) co_return *failure;
    auto sanitizedBody = withoutNullImageOptions(body); const auto validation = validateImageScalarOptions(sanitizedBody, "edits", true);
    if (const auto* failure = std::get_if<ImageRequestFailure>(&validation)) co_return *failure;
    // GPT Image responses are always base64. The validated multipart compatibility field has no Responses-tool equivalent.
    rt::objectDelete(sanitizedBody, "response_format");
    co_return finalizeImageEditBody(form, std::move(sanitizedBody), std::get<double>(validation));
  }
  const rt::OrderedSet<std::string>& imageJsonAllowedKeys(const ImageRouteKind& kind) { return kind == "edits" ? IMAGE_EDIT_JSON_REQUEST_KEYS : IMAGE_GENERATION_REQUEST_KEYS; }
  std::optional<ImageRequestFailure> normalizeImageEditReferences(json::object& body, bool isMultipart) {
    const auto* inputs = rt::getArray(body, "images");
    if (!inputs || inputs->empty() || static_cast<double>(inputs->size()) > IMAGE_MAX_EDIT_INPUTS) return imageRequestError("images must contain from 1 to 16 image references.", "images");
    if (isMultipart) return std::nullopt;
    json::array images;
    for (const auto& input : *inputs) {
      const auto image = normalizeImageReference(&input); if (!image) return imageRequestError("Each image must contain one image_url with a fully qualified HTTP(S) URL or supported base64 data URL; file_id is unsupported by this gateway.", "images");
      images.push_back(*image);
    }
    rt::objectSet(body, "images", images);
    if (rt::hasOwn(body, "mask")) {
      const auto mask = normalizeImageMaskReference(rt::get(body, "mask"));
      if (!mask) return imageRequestError("mask.image_url must be a supported base64 PNG data URL; remote URLs and file_id are unsupported by this gateway.", "mask");
      rt::objectSet(body, "mask", *mask);
    }
    return std::nullopt;
  }
  ParsedImageRequest finalizeImageRequest(const json::value& raw, const ImageRouteKind& kind, double multipartInputBytes, bool isMultipart) {
    if (!isPlainRecord(&raw)) return imageRequestError("Image requests must use an object body.");
    const auto unsupported = openai::findUnknownKey(raw.as_object(), imageJsonAllowedKeys(kind));
    if (unsupported && !unsupported->empty()) return imageRequestError("Unsupported image " + kind + " field: " + *unsupported + ".", *unsupported);
    auto body = withoutNullImageOptions(raw.as_object()); const auto validation = validateImageScalarOptions(body, kind, isMultipart);
    if (const auto* failure = std::get_if<ImageRequestFailure>(&validation)) return *failure;
    const auto count = std::get<double>(validation);
    if (kind == "edits") if (const auto failure = normalizeImageEditReferences(body, isMultipart)) return *failure;
    const auto inputBytes = rt::Math::max(multipartInputBytes, imageReferenceInputBytes(body));
    if (inputBytes > rt::Math::floor(IMAGE_MAX_FANOUT_INPUT_BYTES / count)) return imageRequestError("n and inline image inputs exceed the 50 MiB request work limit.", "n");
    return ParsedImageRequestOk{body, count, inputBytes};
  }
  rt::Task<ParsedImageRequest> parseImageRequest(rt::Request req, ImageRouteKind kind) {
    const auto mediaType = rt::toLowerCase(rt::trim(rt::split(req.headers().get("content-type").value_or(""), ";", 1).front()));
    if (kind == "edits" && mediaType == "multipart/form-data") {
      rt::Promise<ParsedImageRequest> pending(parseMultipartImageEdit(req)); const auto multipart = co_await pending.wait();
      if (const auto* failure = std::get_if<ImageRequestFailure>(&multipart)) co_return *failure;
      const auto& value = std::get<ParsedImageRequestOk>(multipart); co_return finalizeImageRequest(value.body, kind, value.inputBytes, true);
    }
    auto task = kind == "edits"
        ? request::readJsonBody(req, IMAGE_MAX_JSON_BODY_BYTES, rt::limits::kImageEditJsonBytes)
        : request::readJsonBody(req);
    rt::Promise<json::value> pending(std::move(task)); const auto raw = co_await pending.wait();
    if (raw.is_null()) co_return imageRequestError(kind == "edits" ? "Image edits must use a JSON object or multipart form-data body." : "Image generation requests must use a JSON object body.");
    co_return finalizeImageRequest(raw, kind, 0, false);
  }
  rt::Headers imageResponseHeaders(const std::vector<rt::Response>& responses) {
    rt::Headers headers; rt::OrderedSet<std::string> warnings;
    for (const auto& response : responses) for (const auto& warning : request_policy::responseWarnings(response)) warnings.add(warning);
    if (warnings.size() > 0) { const auto values = rt::arrayFrom(warnings); headers.set(request_policy::UOS_WARNING_HEADER, rt::join(values, ", ")); }
    std::vector<std::string_view> names{"x-uos-upstream", "Retry-After"}; for (const auto name : http::STANDARD_RATE_LIMIT_HEADERS) names.push_back(name);
    for (const auto name : names) {
      if (responses.empty()) continue;
      const auto first = responses.front().headers().get(name); if (!first) continue;
      const auto agreed = std::all_of(responses.begin(), responses.end(), [&](const auto& response) { return response.headers().get(name) == first; });
      if (agreed) headers.set(name, *first);
    }
    return headers;
  }
  struct FanoutDispatchState {
    double callCount; api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeProviderDispatch;
    rt::OrderedSet<double> settledCalls; bool transportStarted = false;
    std::optional<api_key_policy::ApiKeyProviderDispatch> providerDispatch;
    std::optional<rt::Promise<void>> cancellation;
    rt::Resolvers<void> allCallsSettled = rt::withResolvers<void>();
    void settled(double index) { if (settledCalls.has(index)) return; settledCalls.add(index); if (static_cast<double>(settledCalls.size()) == callCount) allCallsSettled.resolve(); }
  };
  rt::Task<void> cancelBeforeTransport(std::shared_ptr<FanoutDispatchState> state) {
    co_await state->allCallsSettled.promise.wait(); if (state->transportStarted || !state->providerDispatch) co_return;
    if (!state->cancellation) state->cancellation.emplace(state->providerDispatch->cancelBeforeTransport());
    co_await state->cancellation->wait();
  }
  rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> beforeProviderDispatchFor(std::shared_ptr<FanoutDispatchState> state, double index, std::string provider) {
    auto task = state->beforeProviderDispatch(provider); rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> pending(std::move(task)); const auto dispatch = co_await pending.wait();
    if (dispatch && !state->providerDispatch) { state->providerDispatch = dispatch; if (state->transportStarted) state->providerDispatch->markTransportStarted(); }
    api_key_policy::ApiKeyProviderDispatch result;
    result.markTransportStarted = [state, index] { state->transportStarted = true; state->settled(index); if (state->providerDispatch) state->providerDispatch->markTransportStarted(); };
    // A child owns only its settlement signal. The outer Images handler owns shared cancellation.
    result.cancelBeforeTransport = [state, index] { state->settled(index); return rt::fulfilledTask(); }; co_return result;
  }
  std::optional<tel::UsageContext> imageCallUsageContext(const std::optional<tel::UsageContext>& context, double index, const ImageRouteKind& kind) {
    if (!context) return std::nullopt;
    auto result = *context;
    if (index != 0 && context->requestId && !context->requestId->empty()) result.requestId = *context->requestId + ":image:" + kind + ":" + rt::numberToString(index + 1);
    // Reusing the hidden host model's paid roster would authorize and settle the wrong model.
    result.paidFallbackEnabled = false; result.onTerminalUsage = {}; return result;
  }
  std::optional<tel::UsageTokens> usageTokensFromTelemetry(const tel::ResponseTelemetryState& state) {
    if (!state.usageObserved) return std::nullopt;
    return tel::UsageTokens{.inputTokens = state.inputTokens, .cachedInputTokens = state.cachedInputTokens, .cacheWriteInputTokens = state.cacheWriteInputTokens,
      .outputTokens = state.outputTokens, .totalTokens = state.totalTokens, .status = state.usageTelemetryStatus};
  }
  rt::Response finalizeImageResponse(const std::vector<rt::Response>& sources, rt::Response target, const std::optional<tel::UsageContext>& context) {
    const auto response = tel::aggregateResponseTelemetry(sources, target); const auto aggregate = tel::responseTelemetry.get(response);
    if (aggregate && context && context->responseTelemetry && context->responseTelemetry != *aggregate) { *context->responseTelemetry = **aggregate; tel::responseTelemetry.set(response, context->responseTelemetry); }
    if (aggregate && context && context->onTerminalUsage) { try { context->onTerminalUsage(usageTokensFromTelemetry(**aggregate), (*aggregate)->completed); } catch (...) {} }
    return response;
  }
  std::optional<std::string> extractImageOutputFormatFromResponses(const json::value& payload) {
    if (!isPlainRecord(&payload)) return std::nullopt;
    const auto* output = rt::getArray(payload, "output"); if (!output) return std::nullopt;
    for (const auto& item : *output) {
      if (!isPlainRecord(&item) || rt::getString(item, "type") != "image_generation_call") continue;
      const auto result = rt::getString(item, "result"); if (!result || result->empty()) continue;
      const auto format = rt::getString(item, "output_format"); return format && IMAGE_OUTPUT_FORMATS.has(std::string(*format)) ? std::optional(std::string(*format)) : std::nullopt;
    }
    return std::nullopt;
  }
  struct ImageFanoutFailureResponse { rt::Response response; };
  struct ImageFanoutFailureThrow { rt::Reason error; };
  using ImageFanoutFailure = std::variant<ImageFanoutFailureResponse, ImageFanoutFailureThrow>;
  struct ImageFanoutRuntime {
    rt::Request req; ImageRouteKind kind; std::optional<tel::UsageContext> context; ImageHandlerOptions options;
    json::value responsesBody; std::optional<std::string> serializedResponsesBody;
    std::optional<ImageFanoutDispatchCoordinator> fanoutDispatch; std::optional<rt::AbortController> fanoutAbort;
    rt::AbortSignal childSignal; std::optional<ImageFanoutFailure> firstFailure;
    void recordFirstFailure(ImageFanoutFailure failure) {
      if (firstFailure) return;
      firstFailure = std::move(failure);
      if (fanoutAbort) fanoutAbort->abort(rt::makeReason(rt::DOMException("A sibling image generation call failed.", "AbortError")));
    }
    void settled(double index) { if (fanoutDispatch) fanoutDispatch->settled(index); }
  };
  std::shared_ptr<ImageFanoutRuntime> createImageFanoutRuntime(rt::Request req, const ImageRouteKind& kind, const std::optional<tel::UsageContext>& context,
    const ImageHandlerOptions& options, const json::object& responsesBody, const std::optional<std::string>& serializedResponsesBody, double count) {
    auto runtime = std::make_shared<ImageFanoutRuntime>(ImageFanoutRuntime{req, kind, context, options, responsesBody, serializedResponsesBody, {}, {}, req.signal(), {}});
    if (count > 1 && !options.dispatch && context && context->beforeProviderDispatch) runtime->fanoutDispatch = createImageFanoutDispatchCoordinator(count, context->beforeProviderDispatch);
    if (count > 1) {
      runtime->fanoutAbort.emplace(); std::vector<rt::AbortSignal> signals{req.signal()}; if (context && context->downstreamSignal) signals.push_back(*context->downstreamSignal);
      signals.push_back(runtime->fanoutAbort->signal()); runtime->childSignal = rt::AbortSignal::any(signals);
    }
    return runtime;
  }
  rt::Task<rt::Response> runChild(std::shared_ptr<ImageFanoutRuntime> runtime, double index) {
    rt::Headers headers{{"content-type", "application/json"}};
    auto childContext = imageCallUsageContext(runtime->context, index, runtime->kind);
    if (childContext) {
      if (runtime->fanoutAbort) childContext->downstreamSignal = runtime->childSignal;
      if (runtime->fanoutDispatch) childContext->beforeProviderDispatch = runtime->fanoutDispatch->beforeProviderDispatchFor(index);
    }
    rt::RequestInit init; init.method = "POST"; init.headers = headers; if (runtime->serializedResponsesBody) init.body = *runtime->serializedResponsesBody; init.signal = runtime->childSignal;
    const rt::Request child(rt::URL("/v1/responses", runtime->req.url()), init);
    const auto cleanup = rt::scopeExit([runtime, index] { runtime->settled(index); });
    try {
      auto task = runtime->options.dispatch ? runtime->options.dispatch(child) : responses_handler::runResponsesHandler(child, childContext ? &*childContext : nullptr, &runtime->responsesBody);
      rt::Promise<rt::Response> pending(std::move(task)); const auto upstream = co_await pending.wait();
      if (!upstream.ok()) runtime->recordFirstFailure(ImageFanoutFailureResponse{upstream});
      co_return upstream;
    } catch (...) { runtime->recordFirstFailure(ImageFanoutFailureThrow{std::current_exception()}); throw; }
  }
  rt::Task<void> cancelBeforeTransport(std::shared_ptr<ImageFanoutRuntime> runtime) {
    if (runtime->fanoutDispatch) { try { rt::Promise<void> pending(runtime->fanoutDispatch->cancelBeforeTransport()); co_await pending.wait(); } catch (...) {} }
    else co_await rt::microtask();
  }
  rt::Task<std::vector<rt::Response>> runImageFanoutUpstreams(double count, std::shared_ptr<ImageFanoutRuntime> runtime) {
    std::vector<rt::Promise<rt::Response>> children; for (double index = 0; index < count; ++index) children.emplace_back(runChild(runtime, index));
    const auto settledUpstreams = co_await rt::allSettled(std::move(children)).wait();
    const auto nextChildIndex = count; for (auto index = nextChildIndex; index < count; ++index) runtime->settled(index);
    if (runtime->firstFailure) { rt::Promise<void> pending(cancelBeforeTransport(runtime)); co_await pending.wait(); if (const auto* failure = std::get_if<ImageFanoutFailureThrow>(&*runtime->firstFailure)) rt::rethrow(failure->error); }
    const auto* failure = runtime->firstFailure ? std::get_if<ImageFanoutFailureResponse>(&*runtime->firstFailure) : nullptr;
    std::vector<rt::Response> upstreams;
    for (const auto& result : settledUpstreams) {
      if (result.status == "fulfilled") upstreams.push_back(*result.value); else if (!failure) rt::rethrow(result.reason);
    }
    if (failure) { const auto leader = std::find(upstreams.begin(), upstreams.end(), failure->response); if (leader != upstreams.end() && leader != upstreams.begin()) { const auto response = *leader; upstreams.erase(leader); upstreams.insert(upstreams.begin(), response); } }
    co_return upstreams;
  }
  rt::Task<std::variant<rt::Response, json::value>> readImageUpstreamPayload(std::vector<rt::Response> upstreams, rt::Response upstream, std::optional<tel::UsageContext> context) {
    rt::Promise<std::string> pending(upstream.text()); const auto text = co_await pending.wait(); json::value payload;
    try { if (!text.empty()) payload = rt::jsonParse(text); }
    catch (...) {
      http::OpenaiErrorOptions options; options.headers = imageResponseHeaders({upstream});
      const auto response = http::openaiError(502, "Image upstream returned a non-JSON response.", "upstream_invalid", options);
      co_return finalizeImageResponse(upstreams, response, context);
    }
    if (!upstream.ok()) { const auto response = http::json(upstream.status(), payload, imageResponseHeaders({upstream})); co_return finalizeImageResponse(upstreams, response, context); }
    co_return payload;
  }
  struct ImageCallMetadata { std::optional<std::string> outputFormat; bool outputFormatConsistent = true; std::optional<double> created; };
  void mergeImageCallMetadata(ImageCallMetadata& calls, const json::value& payload) {
    const auto format = extractImageOutputFormatFromResponses(payload);
    if (!format) calls.outputFormatConsistent = false; else if (!calls.outputFormat) calls.outputFormat = format; else if (calls.outputFormat != format) calls.outputFormatConsistent = false;
    if (!calls.created && isPlainRecord(&payload)) if (const auto created = rt::getNumber(payload, "created_at")) calls.created = *created;
  }
  rt::Task<rt::Response> buildImageAggregateResponse(std::vector<rt::Response> upstreams, std::optional<tel::UsageContext> context) {
    json::array images; ImageCallMetadata calls;
    for (const auto& upstream : upstreams) {
      auto task = readImageUpstreamPayload(upstreams, upstream, context); rt::Promise<std::variant<rt::Response, json::value>> pending(std::move(task)); const auto result = co_await pending.wait();
      if (const auto* response = std::get_if<rt::Response>(&result)) co_return *response;
      const auto& payload = std::get<json::value>(result);
      const auto callImages = extractImagesFromResponses(payload);
      if (callImages.empty()) { http::OpenaiErrorOptions options; options.headers = imageResponseHeaders(upstreams); const auto response = http::openaiError(502, "The model did not return an image for this request.", "image_generation_failed", options); co_return finalizeImageResponse(upstreams, response, context); }
      images.push_back(callImages[0]); mergeImageCallMetadata(calls, payload);
    }
    const auto created = calls.created ? *calls.created : rt::Math::floor(rt::dateNow() / 1000);
    json::object body{{"created", rt::makeNumber(created)}, {"data", images}};
    if (calls.outputFormatConsistent && calls.outputFormat && !calls.outputFormat->empty()) rt::objectSet(body, "output_format", json::value(*calls.outputFormat));
    const auto response = http::json(200, body, imageResponseHeaders(upstreams)); co_return finalizeImageResponse(upstreams, response, context);
  }
  rt::Task<rt::Response> handleImagesOwned(rt::Request req, ImageRouteKind kind, std::optional<tel::UsageContext> context, ImageHandlerOptions options) {
    rt::Promise<ParsedImageRequest> parsedPending(parseImageRequest(req, kind)); const auto parsed = co_await parsedPending.wait();
    if (const auto* failure = std::get_if<ImageRequestFailure>(&parsed)) co_return failure->response;
    const auto& value = std::get<ParsedImageRequestOk>(parsed);
    rt::Promise<std::optional<std::string>> basePending(resolveImageBaseModel()); const auto baseModel = co_await basePending.wait();
    if (!baseModel || baseModel->empty()) co_return http::openaiError(503, "No base model is available to host image generation.", "model_unavailable");
    const auto responsesBody = buildImageResponsesRequest(value.body, *baseModel, kind); const auto encoded = rt::jsonStringify(responsesBody);
    if (value.count > 1 && static_cast<double>(rt::utf8ByteLength(encoded)) > rt::Math::floor(IMAGE_MAX_JSON_BODY_BYTES / value.count)) co_return imageRequestError("n and the translated image request exceed the fan-out work limit.", "n").response;
    const auto serialized = options.dispatch ? std::optional(encoded) : std::nullopt;
    const auto runtime = createImageFanoutRuntime(req, kind, context, options, responsesBody, serialized, value.count);
    rt::Promise<std::vector<rt::Response>> upstreamPending(runImageFanoutUpstreams(value.count, runtime)); const auto upstreams = co_await upstreamPending.wait();
    auto task = buildImageAggregateResponse(upstreams, context); rt::Promise<rt::Response> pending(std::move(task)); co_await rt::microtask(); const auto response = co_await pending.wait(); co_return response;
  }
}
void setImageBaseModelForTest(std::optional<std::string> model) { imageBaseModelForTest = std::move(model); }
rt::Task<std::optional<std::string>> resolveImageBaseModel() {
  if (imageBaseModelForTest) co_return imageBaseModelForTest;
  rt::Promise<std::optional<std::string>> pending(openai::getDefaultModel()); const auto model = co_await pending.wait(); co_return model;
}
json::object buildImageResponsesRequest(const json::object& body, const std::string& baseModel, const ImageRouteKind& kind) {
  const auto prompt = rt::getString(body, "prompt").value_or(""); json::object tool{{"type", IMAGE_TOOL_TYPE}, {"action", kind == "edits" ? "edit" : "generate"}};
  const auto requestedModel = rt::trim(rt::getString(body, "model").value_or(""));
  if (!requestedModel.empty()) tool["model"] = requestedModel; else if (kind == "edits") tool["model"] = IMAGE_EDIT_DEFAULT_MODEL;
  for (const auto key : {"size", "quality", "background", "output_format", "output_compression", "moderation"}) if (const auto* value = rt::get(body, key)) rt::objectSet(tool, key, *value);
  json::object result{{"model", baseModel}}; if (const auto user = rt::getString(body, "user")) rt::objectSet(result, "user", json::value(*user));
  if (kind == "edits") {
    if (const auto* value = rt::get(body, "input_fidelity")) rt::objectSet(tool, "input_fidelity", *value);
    const auto* mask = rt::get(body, "mask"); if (isPlainRecord(mask)) rt::objectSet(tool, "input_image_mask", *mask);
    json::array content{json::object{{"type", "input_text"}, {"text", prompt}}};
    if (const auto* images = rt::getArray(body, "images")) for (const auto& entry : *images) if (isPlainRecord(&entry)) if (const auto url = rt::getString(entry, "image_url")) content.push_back(json::object{{"type", "input_image"}, {"image_url", *url}});
    result["input"] = json::array{json::object{{"role", "user"}, {"content", content}}};
  } else result["input"] = prompt;
  result["tools"] = json::array{tool}; result["tool_choice"] = json::object{{"type", IMAGE_TOOL_TYPE}}; return result;
}
ImageFanoutDispatchCoordinator createImageFanoutDispatchCoordinator(double callCount, api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch beforeProviderDispatch) {
  auto state = std::make_shared<FanoutDispatchState>(); state->callCount = callCount; state->beforeProviderDispatch = std::move(beforeProviderDispatch);
  ImageFanoutDispatchCoordinator result;
  result.beforeProviderDispatchFor = [state](double index) -> api_key_policy::ApiKeyUsageReservation::BeforeProviderDispatch {
    return [state, index](std::string provider) { return beforeProviderDispatchFor(state, index, std::move(provider)); };
  };
  result.cancelBeforeTransport = [state] { return cancelBeforeTransport(state); }; result.settled = [state](double index) { state->settled(index); }; return result;
}
std::vector<json::object> extractImagesFromResponses(const json::value* payload) {
  std::vector<json::object> images; if (!isPlainRecord(payload)) return images; const auto* output = rt::getArray(*payload, "output"); if (!output) return images;
  for (const auto& item : *output) {
    if (!isPlainRecord(&item) || rt::getString(item, "type") != "image_generation_call") continue;
    const auto result = rt::getString(item, "result"); if (!result || result->empty()) continue;
    json::object image{{"b64_json", *result}}; if (const auto revised = rt::getString(item, "revised_prompt")) image["revised_prompt"] = *revised; images.push_back(std::move(image));
  }
  return images;
}
std::vector<json::object> extractImagesFromResponses(const json::value& payload) { return extractImagesFromResponses(&payload); }
rt::Task<rt::Response> handleImages(rt::Request req, ImageRouteKind kind, const tel::UsageContext* context) {
  return handleImagesOwned(std::move(req), std::move(kind), context ? std::optional(*context) : std::nullopt, ImageHandlerOptions{});
}
rt::Task<rt::Response> handleImages(rt::Request req, ImageRouteKind kind, const tel::UsageContext* context, ImageHandlerOptions options) {
  return handleImagesOwned(std::move(req), std::move(kind), context ? std::optional(*context) : std::nullopt, std::move(options));
}
}
