# Task-local Rust Worker build environment (source this; contains no secrets).
T=/var/tmp/rust-worker-prep            # symlink -> /home/codex/rust-worker-prep (ops/rust-worker-build.sh accepts /var/tmp/*)
export RUSTUP_HOME=$T/rustup
export CARGO_HOME=$T/cargo
export CARGO_TARGET_DIR=$T/target
export XDG_CACHE_HOME=$T/cache
export TMPDIR=$T/tmp
export RUSTUP_TOOLCHAIN=1.98.1-aarch64-unknown-linux-gnu
export CARGO_INCREMENTAL=0
export CARGO_BUILD_JOBS=2
export WASM_BINDGEN_BIN=$T/cargo/bin/wasm-bindgen
export WASM_OPT_BIN=$T/tools/binaryen/bin/wasm-opt
export ESBUILD_BIN=$T/tools/esbuild/bin/esbuild
export PATH=$RUSTUP_HOME/toolchains/1.98.1-aarch64-unknown-linux-gnu/bin:$CARGO_HOME/bin:/usr/local/bin:/usr/bin:/bin
unset NODE_OPTIONS
