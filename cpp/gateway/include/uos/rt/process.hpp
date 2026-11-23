#pragma once
// node:process, node:os and the Bun globals the gateway uses: platform, arch,
// argv, cwd, execPath, pid, getuid, exit/exitCode, signal listeners,
// Bun.version and Bun.spawn. Signal listeners and subprocesses run on the
// rt event loop: include uos/rt/async.hpp and uos/rt/stream.hpp to await
// `exited`/`flush()` or read a pipe. Bun.sleep(ms) is rt::sleep (uos/rt/clock.hpp).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::rt {
  template <class T> class Promise;
  template <class T> class ReadableStream;
}

namespace uos::rt::process {
  // `process.platform` / `process.arch` of the build target.
#if defined(__linux__)
  inline constexpr std::string_view platform = "linux";
#elif defined(__APPLE__)
  inline constexpr std::string_view platform = "darwin";
#else
#error "uos::rt::process: unsupported platform"
#endif
#if defined(__aarch64__)
  inline constexpr std::string_view arch = "arm64";
#elif defined(__x86_64__)
  inline constexpr std::string_view arch = "x64";
#else
#error "uos::rt::process: unsupported architecture"
#endif

  // `process.argv` / `Bun.argv`: [execPath, script, ...arguments]. A ported
  // entry point calls setArgv(argc, argv) first; the binary (as an absolute
  // path) stands in for Bun's script path, so `argv.slice(2)` is the arguments.
  const std::vector<std::string>& argv();
  void setArgv(int argc, const char* const* argv);
  // Test hook: replaces argv() for the scope.
  class ScopedArgv {
  public:
    explicit ScopedArgv(std::vector<std::string> argv);
    ~ScopedArgv();
    ScopedArgv(const ScopedArgv&) = delete;
    ScopedArgv& operator=(const ScopedArgv&) = delete;

  private:
    std::vector<std::string> previous_;
  };

  // `process.execPath`: the running executable's real path.
  std::string execPath();
  // `process.cwd()`
  std::string cwd();
  // `process.pid`
  int pid();
  // `process.getuid()` (always present on POSIX, so `process.getuid?.()` is this).
  std::uint32_t getuid();

  // `process.exitCode` (undefined until set). The status is taken modulo 256
  // when the process exits.
  std::optional<int> exitCode();
  void setExitCode(std::optional<int> code);
  // The status the process ends with when its event loop drains: exitCode
  // modulo 256, else 0. A ported main returns this (or 1 after an uncaught error).
  int exitStatus();
  // `process.exit(code)`: exits now with code (else exitCode, else 0) modulo
  // 256 (exit(259) is 3, exit(-1) is 255). Pending timers and IO never run;
  // C stdio buffers are flushed, destructors of statics are not run.
  [[noreturn]] void exit(std::optional<int> code = std::nullopt);

  // A listener with identity, the C++ form of the named JS function passed to
  // process.on: adding the same listener twice registers it once (Bun dedupes),
  // and off/removeListener remove exactly that listener. Copies share the identity.
  // Callable as `()` or `(std::string_view signal, int number)`.
  class Listener {
  public:
    Listener() = default;
    template <class F>
      requires(!std::is_same_v<std::decay_t<F>, Listener> &&
               (std::is_invocable_v<std::decay_t<F>&, std::string_view, int> || std::is_invocable_v<std::decay_t<F>&>))
    Listener(F&& callback) {
      using Callback = std::decay_t<F>;
      if constexpr (std::is_invocable_v<Callback&, std::string_view, int>) callback_ = std::make_shared<std::function<void(std::string_view, int)>>(std::forward<F>(callback));
      else callback_ = std::make_shared<std::function<void(std::string_view, int)>>([inner = Callback(std::forward<F>(callback))](std::string_view, int) mutable { inner(); });
    }
    void operator()(std::string_view signal, int number) const { (*callback_)(signal, number); }
    explicit operator bool() const noexcept { return callback_ != nullptr; }
    friend bool operator==(const Listener& left, const Listener& right) noexcept { return left.callback_ == right.callback_; }

  private:
    std::shared_ptr<std::function<void(std::string_view, int)>> callback_;
  };

  // `process.on/once/off/removeListener(signal, listener)` for signal events
  // ("SIGINT", "SIGTERM", ...; other events throw std::invalid_argument).
  // While a signal has a listener it is caught; each delivery runs the
  // listeners on the current rt loop, in registration order, with
  // (name, number); a `once` listener is removed before it runs. Removing the
  // last listener restores the default action (SIGINT then ends the process
  // with status 130).
  void on(std::string_view event, const Listener& listener);
  void once(std::string_view event, const Listener& listener);
  void off(std::string_view event, const Listener& listener);
  void removeListener(std::string_view event, const Listener& listener);
  std::size_t listenerCount(std::string_view event);

  // Linux signal names as Bun spells them ("SIGINT"; 16 is "SIG16").
  std::optional<int> signalNumber(std::string_view name);
  std::optional<std::string_view> signalName(int number);

  // ---- Bun.spawn

  enum class Stdio { pipe, inherit, ignore };

  // The Bun.spawn options the gateway passes. Bun's defaults: stdin "ignore",
  // stdout "pipe", stderr "inherit".
  struct SpawnOptions {
    // Working directory of the child (default: ours).
    std::optional<std::string> cwd = std::nullopt;
    // The child's whole environment, in order. std::nullopt passes the
    // original OS environment (not rt::env edits), exactly as Bun does.
    std::optional<std::vector<std::pair<std::string, std::string>>> env = std::nullopt;
    Stdio stdin = Stdio::ignore;
    Stdio stdout = Stdio::pipe;
    Stdio stderr = Stdio::inherit;
    // On Linux, apply thread-local restrictions in a dedicated spawning thread.
    // The child inherits them, its own process group, and only the three stdio descriptors.
    std::function<void()> confine = {};
  };

  // `subprocess.stdin` for stdin: "pipe" (Bun's FileSink).
  class FileSink {
  public:
    struct State;
    explicit FileSink(std::shared_ptr<State> state);
    // `sink.write(data)`: buffers the bytes and returns their count.
    std::size_t write(std::string_view data);
    std::size_t write(std::span<const std::uint8_t> data);
    // `await sink.flush()`: writes everything buffered to the pipe.
    Promise<void> flush();
    // `await sink.end()`: flushes, then closes the pipe (the child sees EOF).
    Promise<void> end();
    // Discard buffered input and cancel a pending write when terminating a child.
    void close() noexcept;

  private:
    std::shared_ptr<State> state_;
  };

  // A Bun.Subprocess. Copies refer to the same child.
  class Subprocess {
  public:
    struct State;
    explicit Subprocess(std::shared_ptr<State> state);

    int pid() const noexcept;
    // `subprocess.stdin` (stdin: "pipe"; std::logic_error otherwise, where Bun has undefined).
    FileSink stdin() const;
    // `subprocess.stdout` / `.stderr` (Stdio::pipe): the bytes as a ReadableStream.
    ReadableStream<std::vector<std::uint8_t>> stdout() const;
    ReadableStream<std::vector<std::uint8_t>> stderr() const;
    // `subprocess.exited`: the exit code, or 128 + the signal number when a
    // signal ended the child.
    Promise<int> exited() const;
    // null until the child exits; exitCode stays null when a signal ended it.
    std::optional<int> exitCode() const noexcept;
    std::optional<std::string> signalCode() const noexcept;
    // Bun reports `killed` true once the exit has been observed (whatever
    // ended the child), false before.
    bool killed() const noexcept;
    // `subprocess.kill(signal = "SIGTERM")`: no-op once exited. Unknown names:
    // TypeError "signal must be one of 'SIGHUP', ... or 'SIGSYS'"; numbers
    // above 31: TypeError "Invalid signal: must be < 32".
    void kill(std::variant<std::string_view, int> signal = std::string_view("SIGTERM")) const;

  private:
    std::shared_ptr<State> state_;
  };

  // `Bun.spawn(cmd, options)`: starts the child now on the current rt loop.
  // A bare name is looked up in options.env's PATH (env given), else the OS
  // environment's PATH, else "/usr/bin:/bin"; a missing one throws Error
  // `Executable not found in $PATH: "<cmd>"` (code ENOENT, errno -2, path cmd).
  // Exec or cwd failures throw rt::fs::ErrnoError "posix_spawn" with the
  // resolved path; an empty cmd throws TypeError "cmd must not be empty".
  Subprocess spawn(std::vector<std::string> cmd, SpawnOptions options = {});
}

// node:os
namespace uos::rt::os {
  // `os.hostname()`
  std::string hostname();
  // `os.release()`: the kernel release (uname -r).
  std::string release();
  // `os.tmpdir()`: process.env TMPDIR, TMP or TEMP (the first non-empty, read
  // through rt::env), else "/tmp"; one trailing "/" removed.
  std::string tmpdir();
}

// The `Bun` global: constants and Bun.file. Bun.version is stored in KV
// metadata ("created_bun_version"), so the port reports the oracle's value.
// Bun.spawn is rt::process::spawn.
namespace uos::rt::bun {
  inline constexpr std::string_view version = "1.4.0";
  inline constexpr std::string_view revision = "34cbb9a40b4bd1bd767d134a7065e66c2432a676";

  // `Bun.file(path)` (lazy: nothing is opened until read).
  class BunFile {
  public:
    explicit BunFile(std::string path) : path_(std::move(path)) {}
    // `file.name`: the path as given.
    const std::string& name() const noexcept { return path_; }
    // `file.stream()`: the bytes in 256 KiB chunks on the current rt loop. The
    // first read rejects with readFileSync's open error (ENOENT "open" with the
    // path), or EISDIR "fstat" for a directory.
    ReadableStream<std::vector<std::uint8_t>> stream() const;

  private:
    std::string path_;
  };
  inline BunFile file(std::string path) { return BunFile(std::move(path)); }
}
