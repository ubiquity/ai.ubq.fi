# Resolve trusted resources as filesystem paths

Plan row 34. Target 20–45 net first-party lines. Risk 3. Rebase after the URL and filesystem changes. Keep this lane separate from the native stored-value reader.

Own the Node file URL regions in `src/rt/url.cpp:1337–1384,1792–1828` and `include/uos/rt/url.hpp:24–29,184–190`. In `src/static.cpp:27–43,136–186` own resource resolution, asset paths and the two reads. Review the trusted path construction in `src/runtime/permissions_linux.cpp`.

1. Use native path joins for trusted packaged asset locations and fixed source-relative or executable-relative inputs. Keep `runtime::paths::resourceRoot()` for the packaged assets and retain the current cwd fallback in the unconfigured fixture profile. Map each existing trusted relative string to the same on-disk name before editing the caller. Keep package verification before the listener starts.
2. Reuse the filesystem lane's lexical absolute resolver and `rt::isUtf8` for the remaining file converters after verifying their treatment of malformed input. Keep actual external file URL acceptance, including scheme/host, encoded slash, UTF-8 and percent handling. Keep `PathLike` accepting URL values where that interface is used.
3. Preserve `pathToFileURL` for SQLite in `src/storage/sqlite_readonly.cpp:94–107` and `src/kv_conversion/convert.cpp:162–171`. Verify that path bytes including space, `%`, `?` and `#` address the same file, with immutable and read-only URI flags and no new sidecars. Cross-check source and binary resolution for fixture trees whose parent path includes reserved bytes.

```bash
for test in tests/rt/url_test.cpp tests/static_assets_test.cpp tests/static_differential_test.cpp \
  tests/sqlite_readonly_test.cpp tests/kv_conversion_legacy_codec_differential_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Run the startup fixtures on Linux ARM64 with the same driver.

```bash
for test in tests/permissions_linux_test.cpp tests/runtime_native_startup_test.cpp; do
  bash cpp/gateway/tools/ci-build.sh --config release "$test" || exit 1
done
```

Accept when those trusted resource lookups use paths end to end, the URL adapter delegates shared primitives, and asset bytes, package verification, containment and SQLite URI checks pass. [Isolation and build prerequisites](../CI.md)
