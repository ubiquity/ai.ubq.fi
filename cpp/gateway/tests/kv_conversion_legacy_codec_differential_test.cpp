// Links: src/kv_conversion/convert.cpp src/kv_conversion/native_schema.cpp
#include "uos/kv_conversion/convert.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"

namespace rt = uos::rt;
namespace json = uos::json;

ASYNC_TEST("snapshot file digest matches recorded hashes for empty, small and chunked files") {
  const std::string root = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "uos-legacy-codec-"));
  const auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  const json::value recorded = rt::jsonParse(R"JSON([["empty", 0, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"], ["small", 6, "35a9838ae94607ca89c07791a9682ba4a7a4ac9a30f262439c067eeb64ae2ad1"], ["chunked", 614407, "f79145de0cd133899c4e4a6cc3e7f610568829a0859d76742769cd8b94591412"]])JSON");
  for (const json::value& row : recorded.as_array()) {
    const std::string label(row.as_array()[0].as_string());
    const auto size = static_cast<std::size_t>(row.as_array()[1].as_int64());
    std::vector<std::uint8_t> bytes(size);
    for (std::size_t index = 0; index < size; ++index) bytes[index] = static_cast<std::uint8_t>((index * 31 + 7) & 255);
    const std::string path = rt::path::join(root, label);
    rt::fs::writeFileSync(path, std::span<const std::uint8_t>(bytes));
    const std::string digest = co_await uos::kv_conversion::convert::fileDigest(path);
    EXPECT_EQ(digest, std::string(row.as_array()[2].as_string()));
  }
}
