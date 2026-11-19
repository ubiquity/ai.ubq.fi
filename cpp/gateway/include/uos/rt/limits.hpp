#pragma once

#include <chrono>
#include <cstddef>

namespace uos::rt::limits {
  // Use a smaller limit where the route or protocol already requires one.
  inline constexpr std::size_t kHttpHeaderBlockBytes = 32 * 1024;
  inline constexpr std::size_t kHttpHeaderFields = 256;
  inline constexpr std::size_t kHttpChunkLineBytes = 4 * 1024;
  inline constexpr std::size_t kHttpBodyBytes = 16 * 1024 * 1024;
  inline constexpr std::size_t kDecompressionRatio = 1024;
  inline constexpr std::size_t kDecompressionRatioSlackBytes = 1024 * 1024;
  inline constexpr int kZstdWindowLog = 24;

  // Live sideband already caps messages at 4 MiB.
  inline constexpr std::size_t kWebSocketMessageBytes = 16 * 1024 * 1024;
  inline constexpr std::size_t kWebSocketFragments = 4096;
  inline constexpr auto kWebSocketIdleTimeout = std::chrono::seconds(60);

  inline constexpr std::size_t kRegexPatternBytes = 4 * 1024;
  inline constexpr std::size_t kRegexSubjectBytes = 1024 * 1024;
  inline constexpr auto kRegexDeadline = std::chrono::milliseconds(1500);

  inline constexpr std::size_t kJsonDocumentBytes = 16 * 1024 * 1024;
  inline constexpr std::size_t kJsonDepth = 128;

  // Image edits allow a 50 MiB decoded input. JSON needs base64 plus metadata.
  inline constexpr std::size_t kImageEditInputBytes = 50 * 1024 * 1024;
  inline constexpr std::size_t kImageEditJsonBytes = ((kImageEditInputBytes + 2) / 3) * 4 + 1024 * 1024;
  inline constexpr std::size_t kImageEditMultipartBytes = 64 * 1024 * 1024;
  inline constexpr std::size_t kMultipartBoundaryBytes = 72;
  inline constexpr std::size_t kMultipartParts = 256;
  inline constexpr std::size_t kMultipartPartBytes = kImageEditInputBytes;
  // Stored replay envelopes can exceed the ordinary HTTP body limit.
  inline constexpr std::size_t kStoredGzipBytes = 64 * 1024 * 1024;

  // Image edits accept remote references of up to 20 Mi UTF-16 code units.
  inline constexpr std::size_t kUrlBytes = 60 * 1024 * 1024;
  inline constexpr std::size_t kUrlAuthorityBytes = kHttpHeaderBlockBytes;
  inline constexpr std::size_t kUrlQueryBytes = kHttpBodyBytes;
  inline constexpr std::size_t kUrlQueryPairs = 4096;

  // Covers image data and the smaller base64 envelopes used by stored captures.
  inline constexpr std::size_t kBase64DecodedBytes = kImageEditInputBytes;
  inline constexpr std::size_t kBase64InputBytes = ((kBase64DecodedBytes + 2) / 3) * 4;

  // Only the fixed application patterns for these formats use the larger subject budgets.
  inline constexpr std::size_t kImageRegexSubjectBytes = kImageEditJsonBytes;
  inline constexpr std::size_t kCompactionRegexSubjectBytes = kHttpBodyBytes;
  inline constexpr std::size_t kSseRegexSubjectBytes = kHttpBodyBytes;

  inline constexpr std::size_t kCborBytes = 1024 * 1024;
  inline constexpr std::size_t kCborDepth = 8;
  inline constexpr std::size_t kAsn1Bytes = kHttpBodyBytes;
  inline constexpr int kAsn1Depth = 100;
  inline constexpr int kAsn1Elements = 10'000;
  inline constexpr std::size_t kCertificateBytes = 64 * 1024;
  inline constexpr std::size_t kCertificateChainBytes = 256 * 1024;
  inline constexpr std::size_t kCertificateBase64Bytes = ((kCertificateBytes + 2) / 3) * 4;
  inline constexpr std::size_t kCertificatePemBytes = kCertificateBase64Bytes + ((kCertificateBase64Bytes + 63) / 64) * 2 + 64;
  inline constexpr std::size_t kTrustStoreBytes = 1024 * 1024;
}
