# Reuse the UTF-8 codec and native lexical paths

Plan rows 50–51. Target 165–245 net first-party lines, with 70–110 from file text I/O and 95–135 from path operations. Risk 3. Take one lane after the filesystem helper owner lands.

Own `src/rt/fs.cpp:127–205,447–473,738–996`, the text I/O declarations at `include/uos/rt/fs.hpp:130–141`, the path namespace at `249–286` and the relevant cases in `tests/rt/fs_test.cpp`. Keep the descriptor, metadata, permissions and atomic writes in their existing POSIX paths. Row 34 owns the changes to callers that synthesize file URLs.

1. Replace the local UTF-8 conversions with `rt::decodeUtf8` using `TextDecoderOptions{.ignoreBOM = true}` and `rt::encodeUtf8` from `text_codec.hpp`. Compare exact decoded bytes, embedded NUL, retained BOM and malformed-subsequence replacement for both path and descriptor reads. Verify writes still join valid surrogate pairs and repair lone surrogates. Keep the binary overloads, fd offsets, read/write loops, flags, and mode/umask behavior.
2. Use `std::filesystem::path` for lexical normalization, relative paths, parent names and absolute-path tests. Preserve the existing join and resolve distinction for later absolute segments, raw percent and backslash bytes, empty segments, root, trailing slashes, and equal paths. Use lexical operations for string processing. Keep canonicalization, symlink and ownership checks at the security boundary.
3. Verify the containment callers in `src/runtime/permissions_linux.cpp:256–260` and `src/runtime/native_kv_bootstrap.cpp:110–121` still reject escapes. Leave caller policy and descriptor validation with those owners. Keep the path differential corpus and the descriptor, symlink and timestamp coverage.

```bash
for test in tests/rt/fs_test.cpp tests/rt/text_codec_test.cpp \
  tests/static_differential_test.cpp tests/native_addon_differential_test.cpp \
  tests/sqlite_readonly_test.cpp tests/kv_conversion_convert_differential_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Also run the next two fixtures on Linux ARM64. On Apple Silicon use the qualified native fixture procedure for filesystem, static assets and read-only SQLite. [Linux driver](../CI.md) · [Apple Silicon qualification](../MACOS.md)

```bash
for test in tests/permissions_linux_test.cpp tests/runtime_native_startup_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when text conversion uses one codec, the handwritten normalization walk is gone, and path, launch and conversion controls still pass for the intended hosts. Record the measured reduction after both steps.
