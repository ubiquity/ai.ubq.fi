# Install on Apple Silicon

Run `shasum -a 256 -c SHA256SUMS` next to the archive. Extract the verified archive into a new directory. `VERSION`
records the source commit, toolchain and minimum macOS version. The ARM64 executable is under `bin/`, with
third-party licences under `licenses/`.

From the extracted directory, run `./bin/uos-gateway --version`. It prints `uos-gateway` and the full source commit
recorded in `VERSION`, then exits. The version command works offline.

Gateway serving requires the sealed Linux package and its startup confinement. Keep your existing
Mac service on its supported release. To roll back the Mac file installation, select your previous extracted
directory and check its `VERSION`.
