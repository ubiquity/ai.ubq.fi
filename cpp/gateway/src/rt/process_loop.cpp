// The parts of uos/rt/process.hpp that run on the rt event loop: signal
// listeners and Bun.spawn. Kept apart from process.cpp so the loop-free
// functions link without the async runtime.
#include "uos/rt/process.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/stream.hpp"
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/write.hpp>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#if defined(__linux__) && !defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
#include <sys/syscall.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <stdexcept>
#include <thread>

#if defined(__APPLE__)
#include <crt_externs.h>
#else
extern char** environ;
#endif

namespace uos::rt::process {
  namespace {
    namespace asio = boost::asio;
    using Bytes = std::vector<std::uint8_t>;
    using Descriptor = asio::posix::stream_descriptor;

#if defined(__APPLE__) || defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
#if defined(__APPLE__)
    // Bun 1.4.0's subprocess API uses its Linux signal table even on Darwin.
    // process.on listeners, unlike Bun.spawn, use the native signal constants.
    constexpr std::string_view SPAWN_SIGNALS[] = {
      "SIGHUP", "SIGINT", "SIGQUIT", "SIGILL", "SIGTRAP", "SIGABRT", "SIGBUS", "SIGFPE", "SIGKILL", "SIGUSR1", "SIGSEGV",
      "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM", "SIG16", "SIGCHLD", "SIGCONT", "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU",
      "SIGURG", "SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO", "SIGPWR", "SIGSYS"
    };

    std::optional<std::string_view> spawnSignalName(int number) {
      if (number < 1 || number > static_cast<int>(std::size(SPAWN_SIGNALS))) return std::nullopt;
      return SPAWN_SIGNALS[number - 1];
    }

    std::optional<int> spawnSignalNumber(std::string_view name) {
      for (int index = 0; index < static_cast<int>(std::size(SPAWN_SIGNALS)); ++index)
        if (name == SPAWN_SIGNALS[index]) return index + 1;
      return std::nullopt;
    }
#endif

    int closeOnExecPipe(int fds[2]) {
      if (::pipe(fds) != 0) return -1;
      if (::fcntl(fds[0], F_SETFD, FD_CLOEXEC) == 0 && ::fcntl(fds[1], F_SETFD, FD_CLOEXEC) == 0) return 0;
      const int failure = errno;
      ::close(fds[0]);
      ::close(fds[1]);
      errno = failure;
      return -1;
    }

    Task<int> waitForChildExit(int child, std::shared_ptr<asio::signal_set> notifications) {
      for (;;) {
        boost::system::error_code error;
        auto token = asio::redirect_error(asio::use_awaitable, error);
        // Registration precedes spawn; Asio queues a signal until a wait is armed.
        // Await before reaping so even an immediate exit is observed asynchronously.
        co_await notifications->async_wait(std::move(token));
        if (error) throw fs::errnoError(error.value(), "signal");
        int status = 0;
        pid_t result;
        do {
          result = ::waitpid(child, &status, WNOHANG);
        } while (result < 0 && errno == EINTR);
        if (result < 0) throw fs::errnoError(errno, "waitpid");
        if (result == child) co_return status;
        // Another child, a stop or a continue can signal; await the next event.
      }
    }
#endif

    // ---- signal listeners

    struct Registration {
      Listener listener;
      bool once = false;
    };

    struct Signals {
      std::map<int, std::vector<Registration>> listeners;
      std::unique_ptr<asio::signal_set> set;
      bool waiting = false;
      std::uint64_t generation = 0;
    };

    class SignalRegistry final : public asio::execution_context::service {
    public:
      inline static asio::execution_context::id id;
      explicit SignalRegistry(asio::execution_context& context) : service(context) {}
      Signals state;

    private:
      void shutdown() override {
        state.set.reset();
        state.listeners.clear();
      }
    };

    Signals& signals() {
      return asio::use_service<SignalRegistry>(rt::loop().context()).state;
    }

    int requireSignal(std::string_view event) {
      if (auto number = signalNumber(event)) return *number;
      throw std::invalid_argument("uos::rt::process: only signal events are supported, got \"" + std::string(event) + "\"");
    }

    void deliver(int number);

    void arm() {
      auto& state = signals();
      if (!state.set || state.waiting || state.listeners.empty()) return;
      state.waiting = true;
      const std::uint64_t generation = state.generation;
      state.set->async_wait([generation](const boost::system::error_code& error, int number) {
        auto& current = signals();
        if (current.generation != generation) return;
        current.waiting = false;
        if (error) return;
        deliver(number);
        arm();
      });
    }

    void addSignal(int number) {
      auto& state = signals();
      if (!state.set) state.set = std::make_unique<asio::signal_set>(rt::executor());
      state.set->add(number);
      arm();
    }

    // Removing a signal from the set restores its default action.
    void removeSignal(int number) {
      auto& state = signals();
      if (!state.set) return;
      boost::system::error_code ignored;
      state.set->remove(number, ignored);
      if (state.listeners.empty()) {
        state.set.reset();
        state.waiting = false;
        ++state.generation;
      }
    }

    void deliver(int number) {
      auto& state = signals();
      auto found = state.listeners.find(number);
      if (found == state.listeners.end()) return;
      const std::vector<Registration> snapshot = found->second;
      std::erase_if(found->second, [](const Registration& registration) { return registration.once; });
      if (found->second.empty()) {
        state.listeners.erase(found);
        removeSignal(number);
      }
      const std::string_view name = *signalName(number);
      for (const auto& registration : snapshot) registration.listener(name, number);
    }

    void addListener(std::string_view event, const Listener& listener, bool once) {
      const int number = requireSignal(event);
      auto& entries = signals().listeners[number];
      for (const auto& registration : entries)
        if (registration.listener == listener) return;
      const bool first = entries.empty();
      entries.push_back(Registration{listener, once});
      if (first) addSignal(number);
    }

    // ---- Bun.spawn

    struct Pipe {
      int read = -1;
      int write = -1;
    };

    void closeFd(int& fd) noexcept {
      if (fd >= 0) ::close(fd);
      fd = -1;
    }

    std::string signalList() {
      std::string out = "signal must be one of ";
      for (int number = 1; number <= 31; ++number) {
        if (number > 1) out += number == 31 ? " or " : ", ";
#if defined(__APPLE__)
        out += "'" + std::string(*spawnSignalName(number)) + "'";
#else
        out += "'" + std::string(*signalName(number)) + "'";
#endif
      }
      return out;
    }

    std::optional<std::string> findExecutable(std::string_view name, std::string_view path) {
      std::size_t start = 0;
      while (start <= path.size()) {
        const std::size_t end = std::min(path.find(':', start), path.size());
        const std::string_view directory = path.substr(start, end - start);
        if (!directory.empty()) {
          const std::string candidate = std::string(directory) + "/" + std::string(name);
          struct stat info;
          if (::stat(candidate.c_str(), &info) == 0 && S_ISREG(info.st_mode) && ::access(candidate.c_str(), X_OK) == 0) return candidate;
        }
        start = end + 1;
      }
      return std::nullopt;
    }

    std::string lookupPath(const SpawnOptions& options) {
      constexpr const char* DEFAULT_PATH = "/usr/bin:/bin";
      if (options.env) {
        for (const auto& [name, value] : *options.env)
          if (name == "PATH") return value;
        return DEFAULT_PATH;
      }
      const char* inherited = std::getenv("PATH");
      return inherited ? inherited : DEFAULT_PATH;
    }

    // GCC 12.2 destroys temporaries created inside a co_await full-expression
    // twice: every co_await below awaits a named operation built from named locals.
    Task<void> readChunk(std::shared_ptr<Descriptor> descriptor, ReadableStreamDefaultController<Bytes> controller) {
      Bytes buffer(65536);
      boost::system::error_code error;
      const auto view = asio::buffer(buffer);
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const std::size_t count = co_await descriptor->async_read_some(view, std::move(token));
      if (error == asio::error::eof || (!error && count == 0)) {
        boost::system::error_code ignored;
        descriptor->close(ignored);
        controller.close();
        co_return;
      }
      if (error) {
        controller.error(std::make_exception_ptr(fs::errnoError(error.value(), "read")));
        co_return;
      }
      buffer.resize(count);
      controller.enqueue(std::move(buffer));
    }

    ReadableStream<Bytes> pipeStream(int fd) {
      auto descriptor = std::make_shared<Descriptor>(rt::executor(), fd);
      UnderlyingSource<Bytes> source;
      source.pull = [descriptor](ReadableStreamDefaultController<Bytes> controller) { return readChunk(descriptor, std::move(controller)); };
      source.cancel = [descriptor](Reason) {
        boost::system::error_code ignored;
        descriptor->close(ignored);
      };
      return ReadableStream<Bytes>(std::move(source));
    }
  }

  void on(std::string_view event, const Listener& listener) { addListener(event, listener, false); }
  void once(std::string_view event, const Listener& listener) { addListener(event, listener, true); }

  void removeListener(std::string_view event, const Listener& listener) {
    const int number = requireSignal(event);
    auto& state = signals();
    auto found = state.listeners.find(number);
    if (found == state.listeners.end()) return;
    auto& entries = found->second;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Registration& registration) { return registration.listener == listener; }), entries.end());
    if (entries.empty()) {
      state.listeners.erase(found);
      removeSignal(number);
    }
  }

  void off(std::string_view event, const Listener& listener) { removeListener(event, listener); }

  std::size_t listenerCount(std::string_view event) {
    const int number = requireSignal(event);
    auto found = signals().listeners.find(number);
    return found == signals().listeners.end() ? 0 : found->second.size();
  }

  struct FileSink::State {
    std::shared_ptr<Descriptor> descriptor;
    Bytes pending;
    bool ended = false;
  };

  FileSink::FileSink(std::shared_ptr<State> state) : state_(std::move(state)) {}

  std::size_t FileSink::write(std::string_view data) {
    return write(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(data.data()), data.size()));
  }

  std::size_t FileSink::write(std::span<const std::uint8_t> data) {
    state_->pending.insert(state_->pending.end(), data.begin(), data.end());
    return data.size();
  }

  namespace {
    Task<void> flushSink(std::shared_ptr<FileSink::State> state) {
      while (!state->pending.empty()) {
        Bytes chunk = std::move(state->pending);
        state->pending.clear();
        boost::system::error_code error;
        const auto view = asio::buffer(chunk);
        auto token = asio::redirect_error(asio::use_awaitable, error);
        Descriptor& descriptor = *state->descriptor;
        co_await asio::async_write(descriptor, view, std::move(token));
        if (error) throw fs::errnoError(error.value(), "write");
      }
    }

    Task<void> endSink(std::shared_ptr<FileSink::State> state) {
      Task<void> flushing = flushSink(state);
      co_await std::move(flushing);
      state->ended = true;
      boost::system::error_code ignored;
      state->descriptor->close(ignored);
    }
  }

  Promise<void> FileSink::flush() { return Promise<void>(flushSink(state_)); }
  Promise<void> FileSink::end() { return Promise<void>(endSink(state_)); }
  void FileSink::close() noexcept {
    state_->pending.clear();
    state_->ended = true;
    boost::system::error_code ignored;
    state_->descriptor->close(ignored);
  }

  struct Subprocess::State {
    int pid = -1;
    bool ownProcessGroup = false;
    Promise<int> exited;
    std::optional<Resolve<int>> resolveExited;
    std::optional<int> exitCode;
    std::optional<std::string> signalCode;
    bool observed = false;
    std::optional<ReadableStream<Bytes>> stdoutStream;
    std::optional<ReadableStream<Bytes>> stderrStream;
    std::shared_ptr<FileSink::State> stdinSink;
  };

  namespace {
#if defined(__APPLE__) || defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
    Task<void> watchExit(std::shared_ptr<Subprocess::State> state, std::shared_ptr<asio::signal_set> notifications) {
      auto observation = waitForChildExit(state->pid, notifications);
      const int status = co_await std::move(observation);
      int code = 0;
      if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
#if defined(__APPLE__)
        const auto name = spawnSignalName(number);
#else
        const auto name = signalName(number);
#endif
        state->signalCode = name ? std::string(*name) : "SIG" + std::to_string(number);
        code = 128 + number;
      } else {
        state->exitCode = WEXITSTATUS(status);
        code = *state->exitCode;
      }
      state->observed = true;
      (*state->resolveExited)(code);
      state->resolveExited.reset();
    }
#else
    Task<void> watchExit(std::shared_ptr<Subprocess::State> state, int pidfd) {
      Descriptor descriptor(rt::executor(), pidfd);
      boost::system::error_code error;
      auto token = asio::redirect_error(asio::use_awaitable, error);
      co_await descriptor.async_wait(Descriptor::wait_read, std::move(token));
      int status = 0;
      while (::waitpid(state->pid, &status, 0) < 0 && errno == EINTR) {
      }
      int code = 0;
      if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        const auto name = signalName(number);
        state->signalCode = name ? std::string(*name) : "SIG" + std::to_string(number);
        code = 128 + number;
      } else {
        state->exitCode = WEXITSTATUS(status);
        code = *state->exitCode;
      }
      state->observed = true;
      (*state->resolveExited)(code);
      state->resolveExited.reset();
    }
#endif
  }

  Subprocess::Subprocess(std::shared_ptr<State> state) : state_(std::move(state)) {}

  int Subprocess::pid() const noexcept { return state_->pid; }

  FileSink Subprocess::stdin() const {
    if (!state_->stdinSink) throw std::logic_error("uos::rt::process::Subprocess::stdin: the child was spawned without stdin: \"pipe\"");
    return FileSink(state_->stdinSink);
  }

  ReadableStream<std::vector<std::uint8_t>> Subprocess::stdout() const {
    if (!state_->stdoutStream) throw std::logic_error("uos::rt::process::Subprocess::stdout: the child was spawned without stdout: \"pipe\"");
    return *state_->stdoutStream;
  }

  ReadableStream<std::vector<std::uint8_t>> Subprocess::stderr() const {
    if (!state_->stderrStream) throw std::logic_error("uos::rt::process::Subprocess::stderr: the child was spawned without stderr: \"pipe\"");
    return *state_->stderrStream;
  }

  Promise<int> Subprocess::exited() const { return state_->exited; }
  std::optional<int> Subprocess::exitCode() const noexcept { return state_->exitCode; }
  std::optional<std::string> Subprocess::signalCode() const noexcept { return state_->signalCode; }
  bool Subprocess::killed() const noexcept { return state_->observed; }

  void Subprocess::kill(std::variant<std::string_view, int> signal) const {
    int number;
    if (const auto* name = std::get_if<std::string_view>(&signal)) {
#if defined(__APPLE__)
      const auto found = spawnSignalNumber(*name);
#else
      const auto found = signalNumber(*name);
#endif
      if (!found) throw TypeError(signalList());
      number = *found;
    } else {
      number = std::get<int>(signal);
      if (number >= 32) throw TypeError("Invalid signal: must be < 32");
    }
    if (state_->observed) return;
    ::kill(state_->ownProcessGroup ? -state_->pid : state_->pid, number);
  }

  Subprocess spawn(std::vector<std::string> cmd, SpawnOptions options) {
    if (cmd.empty()) throw TypeError("cmd must not be empty");
#if !defined(__linux__)
    if (options.confine) throw TypeError("Confined subprocesses require Linux");
#endif
    std::string file = cmd.front();
    if (file.find('/') == std::string::npos) {
      auto found = findExecutable(file, lookupPath(options));
      if (!found) throw fs::ErrnoError("Executable not found in $PATH: \"" + file + "\"", "ENOENT", -2, "", file);
      file = std::move(*found);
    }
    // Bun ignores SIGPIPE: a write to a pipe whose reader exited fails with EPIPE.
    ::signal(SIGPIPE, SIG_IGN);

    std::vector<std::string> environment;
    if (options.env)
      for (const auto& [name, value] : *options.env) environment.push_back(name + "=" + value);
    std::vector<char*> envp;
    for (auto& entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    std::vector<char*> args;
    for (auto& argument : cmd) args.push_back(argument.data());
    args.push_back(nullptr);

#if defined(__APPLE__) || defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
    // The signal handler is installed before the child can exit.
    auto childExit = std::make_shared<asio::signal_set>(rt::executor(), SIGCHLD);
#endif
    Pipe in, out, err;
    auto release = rt::scopeExit([&] {
      for (Pipe* pipe : {&in, &out, &err}) {
        closeFd(pipe->read);
        closeFd(pipe->write);
      }
    });
    auto makePipe = [&](Pipe& pipe) {
      int fds[2];
#if defined(__APPLE__) || defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
      const int pipeStatus = closeOnExecPipe(fds);
#else
      const int pipeStatus = ::pipe2(fds, O_CLOEXEC);
#endif
      if (pipeStatus != 0) throw fs::errnoError(errno, "pipe");
      pipe.read = fds[0];
      pipe.write = fds[1];
    };
    if (options.stdin == Stdio::pipe) makePipe(in);
    if (options.stdout == Stdio::pipe) makePipe(out);
    if (options.stderr == Stdio::pipe) makePipe(err);

    const auto checked = [&](int status) {
      if (status != 0) throw fs::errnoError(status, "posix_spawn", file);
    };
    posix_spawn_file_actions_t actions;
    checked(posix_spawn_file_actions_init(&actions));
    auto releaseActions = rt::scopeExit([&] { posix_spawn_file_actions_destroy(&actions); });
    const auto setupStdio = [&](Stdio mode, int fd, int standard) {
      if (mode == Stdio::pipe) checked(posix_spawn_file_actions_adddup2(&actions, fd, standard));
      if (mode == Stdio::ignore) checked(posix_spawn_file_actions_addopen(&actions, standard, "/dev/null", standard == 0 ? O_RDONLY : O_WRONLY, 0));
    };
    setupStdio(options.stdin, in.read, 0);
    setupStdio(options.stdout, out.write, 1);
    setupStdio(options.stderr, err.write, 2);
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
    if (options.cwd) checked(posix_spawn_file_actions_addchdir(&actions, options.cwd->c_str()));
#else
    if (options.cwd) checked(posix_spawn_file_actions_addchdir_np(&actions, options.cwd->c_str()));
#endif
#if defined(__linux__)
    if (options.confine) checked(posix_spawn_file_actions_addclosefrom_np(&actions, 3));
#endif

    posix_spawnattr_t attributes;
    checked(posix_spawnattr_init(&attributes));
    auto releaseAttributes = rt::scopeExit([&] { posix_spawnattr_destroy(&attributes); });
    sigset_t empty, all;
    sigemptyset(&empty);
    sigfillset(&all);
    checked(posix_spawnattr_setsigmask(&attributes, &empty));
    checked(posix_spawnattr_setsigdefault(&attributes, &all));
    short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
    if (options.confine) {
      checked(posix_spawnattr_setpgroup(&attributes, 0));
      flags |= POSIX_SPAWN_SETPGROUP;
    }
    checked(posix_spawnattr_setflags(&attributes, flags));

    pid_t child = -1;
#if defined(__APPLE__)
    char** inheritedEnvironment = *_NSGetEnviron();
#else
    char** inheritedEnvironment = environ;
#endif
    const auto launch = [&] {
      checked(::posix_spawn(&child, file.c_str(), &actions, &attributes, args.data(), options.env ? envp.data() : inheritedEnvironment));
    };
    if (options.confine) {
      // Landlock restrictions survive exec and apply only to this thread and its child.
      std::exception_ptr failure;
      std::thread launcher([&] {
        try {
          options.confine();
          launch();
        } catch (...) {
          failure = std::current_exception();
        }
      });
      launcher.join();
      if (failure) std::rethrow_exception(failure);
    } else {
      launch();
    }
    auto reapOnFailure = rt::scopeExit([&] {
      if (child <= 0) return;
      ::kill(options.confine ? -child : child, SIGKILL);
      while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    });
    closeFd(in.read);
    closeFd(out.write);
    closeFd(err.write);

    auto state = std::make_shared<Subprocess::State>();
    state->pid = static_cast<int>(child);
    state->ownProcessGroup = static_cast<bool>(options.confine);
    auto resolvers = withResolvers<int>();
    state->exited = resolvers.promise;
    state->resolveExited.emplace(resolvers.resolve);
    if (options.stdin == Stdio::pipe) {
      state->stdinSink = std::make_shared<FileSink::State>();
      state->stdinSink->descriptor = std::make_shared<Descriptor>(rt::executor(), std::exchange(in.write, -1));
    }
    if (options.stdout == Stdio::pipe) state->stdoutStream.emplace(pipeStream(std::exchange(out.read, -1)));
    if (options.stderr == Stdio::pipe) state->stderrStream.emplace(pipeStream(std::exchange(err.read, -1)));

#if defined(__APPLE__) || defined(UOS_GATEWAY_TEST_POSIX_CHILD_WATCHER)
    auto observation = watchExit(state, childExit);
    rt::spawn(std::move(observation));
#else
    const int pidfd = static_cast<int>(::syscall(SYS_pidfd_open, child, 0));
    if (pidfd < 0) throw fs::errnoError(errno, "pidfd_open");
    ::fcntl(pidfd, F_SETFD, FD_CLOEXEC);
    rt::spawn(watchExit(state, pidfd));
#endif
    child = -1;
    return Subprocess(state);
  }
}

namespace uos::rt::bun {
  namespace {
    struct FileSource {
      std::string path;
      int fd = -1;
      bool opened = false;
      ~FileSource() {
        if (fd >= 0) ::close(fd);
      }
    };

    void pullFile(const std::shared_ptr<FileSource>& source, ReadableStreamDefaultController<std::vector<std::uint8_t>> controller) {
      if (!source->opened) {
        source->opened = true;
        int fd;
        do fd = ::open(source->path.c_str(), O_RDONLY | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0) {
          controller.error(std::make_exception_ptr(fs::errnoError(errno, "open", source->path)));
          return;
        }
        source->fd = fd;
        struct stat info;
        if (::fstat(fd, &info) != 0 || S_ISDIR(info.st_mode)) {
          const int error = S_ISDIR(info.st_mode) ? EISDIR : errno;
          controller.error(std::make_exception_ptr(fs::errnoError(error, "fstat")));
          return;
        }
      }
      std::vector<std::uint8_t> chunk(262144);
      std::size_t filled = 0;
      while (filled < chunk.size()) {
        const ssize_t count = ::read(source->fd, chunk.data() + filled, chunk.size() - filled);
        if (count < 0) {
          if (errno == EINTR) continue;
          controller.error(std::make_exception_ptr(fs::errnoError(errno, "read")));
          return;
        }
        if (count == 0) break;
        filled += static_cast<std::size_t>(count);
      }
      if (filled == 0) {
        ::close(std::exchange(source->fd, -1));
        controller.close();
        return;
      }
      chunk.resize(filled);
      controller.enqueue(std::move(chunk));
    }
  }

  ReadableStream<std::vector<std::uint8_t>> BunFile::stream() const {
    auto source = std::make_shared<FileSource>();
    source->path = path_;
    UnderlyingSource<std::vector<std::uint8_t>> underlying;
    underlying.pull = [source](ReadableStreamDefaultController<std::vector<std::uint8_t>> controller) { pullFile(source, std::move(controller)); };
    underlying.cancel = [source](Reason) {
      if (source->fd >= 0) ::close(std::exchange(source->fd, -1));
    };
    return ReadableStream<std::vector<std::uint8_t>>(std::move(underlying));
  }
}
