# Install on Apple Silicon

Run `shasum -a 256 -c SHA256SUMS` next to the archive. Extract the verified archive into a new directory. `VERSION`
records the source commit, toolchain and minimum macOS version. The three arm64 executables are under `bin/`, with
their third-party licences under `licenses/`.

This is a development package. The current gateway and KV executables require the Linux package and startup
confinement. Keep your existing Mac service on its supported release until the native macOS launcher is available.
To roll back the file installation, select your previous extracted directory.
