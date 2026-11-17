# libcbor 0.15.0

The upstream `v0.15.0` release was published on 2026-11-15. The vendored files are the contents of upstream `src/` and
its MIT `LICENSE.md`. The source was downloaded from GitHub on 2026-11-17. Both upstream code and the license are
copied byte for byte. [Upstream release](https://github.com/PJK/libcbor/releases/tag/v0.15.0)

```sh
curl -fsSL https://codeload.github.com/PJK/libcbor/tar.gz/refs/tags/v0.15.0 -o libcbor-v0.15.0.tar.gz
printf '%s\n' 'c94145d3c22e67cff9a3325dc036b4545a49a42231553995be4947d52cf2ef57  libcbor-v0.15.0.tar.gz' |
  sha256sum --check
tar -xf libcbor-v0.15.0.tar.gz
cmake -S libcbor-0.15.0 -B libcbor-generated -DCMAKE_C_COMPILER=clang-21 -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_STANDARD=99 -DBUILD_SHARED_LIBS=OFF -DWITH_EXAMPLES=OFF -DCBOR_PRETTY_PRINTER=OFF \
  -DCBOR_MAX_STACK_SIZE=64 -DCBOR_MAX_UNCHECKED_DECLARED_SIZE=0
```

`cbor/configuration.h` and `cbor/cbor_export.h` come from that static configuration on Linux ARM64. The export
header's trailing whitespace was stripped. Both are retained for direct C builds on supported targets. The decoder
stack is limited to 64 open items and all declared container sizes are checked against the input before preallocation.
`SHA256SUMS` covers the complete copied source tree, license and the two generated headers.
