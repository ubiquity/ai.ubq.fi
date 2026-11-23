# Install on ARM64 Linux

Verify the archive with `sha256sum -c SHA256SUMS` before extracting it. Use 64-bit Linux with glibc 2.36 or newer,
and seccomp thread synchronization. The default requires Landlock ABI 8 or newer. On ABI 3–7, set
`UOS_GATEWAY_ALLOW_REDUCED_CONFINEMENT=1` in the gateway's launch environment to use the reduced policy. Install the
host's OpenSSL 3, zlib, Brotli and Zstd shared libraries, CA certificates, Python 3 and `readelf` (binutils).

Extract into an empty directory. From that directory run `python3 -B seal.py /absolute/path/to/new-release`. Choose a
new directory under an existing canonical parent. Sealing checks the payload and records your host's library paths and
hashes. Repeat sealing into a new directory if the host libraries change.

Use the sealed release's `runtime/launch-schema.json` and the values printed by `seal.py` to configure a private launch
manifest. Set `stateRoot` to a separate private directory with a supported `.data/kv-bun.sqlite3`, then pass the absolute
manifest path to `ops/launch-native.sh` in the sealed release. See `README.md` for the Pi prerequisites and the release
guide for the platform commands and rollback procedure.
