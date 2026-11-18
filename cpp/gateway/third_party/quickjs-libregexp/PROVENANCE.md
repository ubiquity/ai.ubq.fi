# QuickJS-ng libregexp 0.18.0

Source is the QuickJS-ng [v0.18.0 release](https://github.com/quickjs-ng/quickjs/releases/tag/v0.18.0).
On 2026-11-17 the gateway extracted the standalone regular expression compiler, executor, Unicode
tables, utility header, and upstream MIT license, without changing their bytes.

The [GitHub tag archive](https://codeload.github.com/quickjs-ng/quickjs/tar.gz/refs/tags/v0.18.0)
was verified with SHA-256 `28bcd89aa3683fd2a946537e5ce3f11cc46b5cbf3383c7561aaef3df5ff9f1dd`.
`SHA256SUMS` pins each extracted file. The two C translation units are `libregexp.c` and
`libunicode.c`. This tag defines the required utility functions in `cutils.h`. The
standalone compiler and executor link without the QuickJS VM or its number formatter.
