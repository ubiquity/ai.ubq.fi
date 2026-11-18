// The parts of uos/rt/process.hpp that run on the rt event loop: signal
// listeners and Bun.spawn. Kept apart from process.cpp so the loop-free
// functions link without the async runtime.
#include "uos/rt/process.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/stream.hpp"
#include <boost/asio/connect_pipe.hpp>
#include <boost/asio/readable_pipe.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/signal_set.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <boost/asio/writable_pipe.hpp>
#include <boost/asio/write.hpp>
// The child only execs. It never runs or mutates the parent's Asio context.
#define BOOST_PROCESS_V2_DISABLE_NOTIFY_FORK
#include <boost/process/v2/process.hpp>
#include <boost/process/v2/start_dir.hpp>
#include <boost/process/v2/stdio.hpp>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace uos::rt::process {
  namespace {
    namespace asio = boost::asio;
    namespace bp = boost::process::v2;
    using Bytes = std::vector<std::uint8_t>;

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

    Signals& signals() {
      static Signals state;
      return state;
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

    void connectPipe(asio::readable_pipe& read, asio::writable_pipe& write) {
      boost::system::error_code error;
      asio::connect_pipe(read, write, error);
      if (error) throw fs::errnoError(error.value(), "pipe");
      for (int fd : {read.native_handle(), write.native_handle()}) {
        if (::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) throw fs::errnoError(errno, "fcntl");
      }
    }

    class NullDevice {
    public:
      NullDevice(bool needed, int flags) {
        if (!needed) return;
        do fd = ::open("/dev/null", flags | O_CLOEXEC);
        while (fd < 0 && errno == EINTR);
        if (fd < 0) throw fs::errnoError(errno, "open", "/dev/null");
      }
      ~NullDevice() {
        if (fd >= 0) ::close(fd);
      }
      NullDevice(const NullDevice&) = delete;
      NullDevice& operator=(const NullDevice&) = delete;
      int fd = -1;
    };

    template <class Binding, class Pipe>
    Binding stdioBinding(Stdio mode, Pipe& pipe, const NullDevice& nullDevice) {
      if (mode == Stdio::pipe) return pipe;
      if (mode == Stdio::ignore) return nullDevice.fd;
      return {};
    }

    struct ResetChildSignals {
      boost::system::error_code on_exec_setup(bp::posix::default_launcher&, const bp::filesystem::path&, const char* const*) const {
        struct sigaction action{};
        action.sa_handler = SIG_DFL;
        ::sigemptyset(&action.sa_mask);
        for (int number = 1; number < NSIG; ++number) {
          if (::sigaction(number, &action, nullptr) < 0 && errno != EINVAL)
            return {errno, boost::system::system_category()};
        }
        if (::sigprocmask(SIG_SETMASK, &action.sa_mask, nullptr) < 0) return {errno, boost::system::system_category()};
        return {};
      }
    };

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

    Task<void> readChunk(std::shared_ptr<asio::readable_pipe> pipe, ReadableStreamDefaultController<Bytes> controller) {
      Bytes buffer(65536);
      boost::system::error_code error;
      const auto view = asio::buffer(buffer);
      auto token = asio::redirect_error(asio::use_awaitable, error);
      const std::size_t count = co_await pipe->async_read_some(view, std::move(token));
      if (error == asio::error::eof || (!error && count == 0)) {
        boost::system::error_code ignored;
        pipe->close(ignored);
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

    ReadableStream<Bytes> pipeStream(asio::writable_pipe& childEnd) {
      auto pipe = std::make_shared<asio::readable_pipe>(childEnd.get_executor());
      connectPipe(*pipe, childEnd);
      UnderlyingSource<Bytes> source;
      source.pull = [pipe](ReadableStreamDefaultController<Bytes> controller) { return readChunk(pipe, std::move(controller)); };
      source.cancel = [pipe](Reason) {
        boost::system::error_code ignored;
        pipe->close(ignored);
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
    explicit State(Executor executor) : pipe(executor) {}

    asio::writable_pipe pipe;
    Bytes pending;
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
        co_await asio::async_write(state->pipe, view, std::move(token));
        if (error) throw fs::errnoError(error.value(), "write");
      }
    }

    Task<void> endSink(std::shared_ptr<FileSink::State> state) {
      Task<void> flushing = flushSink(state);
      co_await std::move(flushing);
      boost::system::error_code ignored;
      state->pipe.close(ignored);
    }
  }

  Promise<void> FileSink::flush() { return Promise<void>(flushSink(state_)); }
  Promise<void> FileSink::end() { return Promise<void>(endSink(state_)); }

  struct Subprocess::State {
    int pid = -1;
    Promise<int> exited;
    std::optional<bp::process> child;
    std::optional<int> exitCode;
    std::optional<std::string> signalCode;
    std::optional<ReadableStream<Bytes>> stdoutStream;
    std::optional<ReadableStream<Bytes>> stderrStream;
    std::shared_ptr<FileSink::State> stdinSink;
  };

  namespace {
    Task<int> watchExit(std::shared_ptr<Subprocess::State> state) {
      boost::system::error_code error;
      int code;
      do {
        auto token = asio::redirect_error(asio::use_awaitable, error);
        code = co_await state->child->async_wait(std::move(token));
      } while (error == boost::system::errc::interrupted);
      const int status = state->child->native_exit_code();
      state->child.reset();
      if (error) throw fs::errnoError(error.value(), "wait");
      if (WIFSIGNALED(status)) {
        const int number = WTERMSIG(status);
        const auto name = signalName(number);
        state->signalCode = name ? std::string(*name) : "SIG" + std::to_string(number);
        co_return 128 + number;
      }
      state->exitCode = code;
      co_return code;
    }

    // The launcher can fork successfully and then fail to acquire a process handle.
    // In that case there is no Boost object to clean up the child.
    void reapUnownedChild(pid_t child) {
      if (child <= 0) return;
      pid_t observed;
      do observed = ::waitpid(child, nullptr, WNOHANG);
      while (observed < 0 && errno == EINTR);
      if (observed != 0) return;
      ::kill(child, SIGKILL);
      while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {}
    }

    bp::process launch(Executor executor, const std::string& file, const std::vector<std::string>& cmd,
                       const SpawnOptions& options, bp::process_stdio& stdio) {
      // Pass a complete argv to preserve the caller's argv[0] after PATH lookup.
      std::vector<const char*> argv;
      argv.reserve(cmd.size() + 1);
      for (const auto& argument : cmd) argv.push_back(argument.c_str());
      argv.push_back(nullptr);

      bp::default_process_launcher launcher;
      std::vector<std::string> environment;
      std::vector<const char*> envp;
      if (options.env) {
        environment.reserve(options.env->size());
        for (const auto& [name, value] : *options.env) environment.push_back(name + "=" + value);
        envp.reserve(environment.size() + 1);
        for (const auto& entry : environment) envp.push_back(entry.c_str());
        envp.push_back(nullptr);
        launcher.env = envp.data();
      }
      const bp::filesystem::path executable(file);

      // An inherited handler could otherwise run in the child before exec setup.
      sigset_t all, previous;
      ::sigfillset(&all);
      const int maskError = ::pthread_sigmask(SIG_BLOCK, &all, &previous);
      if (maskError != 0) throw fs::errnoError(maskError, "pthread_sigmask");
      const auto restoreMask = scopeExit([&previous] { ::pthread_sigmask(SIG_SETMASK, &previous, nullptr); });
      auto cleanup = scopeExit([&launcher] { reapUnownedChild(launcher.pid); });
      try {
        boost::system::error_code error;
        bp::process child = options.cwd
            ? launcher(executor, error, executable, argv.data(), stdio, bp::process_start_dir(*options.cwd), ResetChildSignals())
            : launcher(executor, error, executable, argv.data(), stdio, ResetChildSignals());
        cleanup.release();
        if (error) throw fs::errnoError(error.value(), "spawn", file);
        return child;
      } catch (const boost::system::system_error& failure) {
        throw fs::errnoError(failure.code().value(), "spawn", file);
      }
    }
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
  bool Subprocess::killed() const noexcept { return !state_->child; }

  void Subprocess::kill(std::variant<std::string_view, int> signal) const {
    int number;
    if (const auto* name = std::get_if<std::string_view>(&signal)) {
      const auto found = signalNumber(*name);
      if (!found) throw TypeError("Unknown signal: " + std::string(*name));
      number = *found;
    } else {
      number = std::get<int>(signal);
      if (number < 0 || number >= 32) throw TypeError("Invalid signal number");
    }
    // Boost may already have reaped the PID while the completion is queued.
    if (state_->child && bp::process_is_running(state_->child->native_exit_code())) ::kill(state_->pid, number);
  }

  Subprocess spawn(std::vector<std::string> cmd, SpawnOptions options) {
    if (cmd.empty()) throw TypeError("cmd must not be empty");
    std::string file = cmd.front();
    if (file.find('/') == std::string::npos) {
      auto found = findExecutable(file, lookupPath(options));
      if (!found) throw fs::ErrnoError("Executable not found in $PATH: \"" + file + "\"", "ENOENT", -2, "", file);
      file = std::move(*found);
    }
    // Asio writes through POSIX pipe descriptors. A closed reader must yield EPIPE.
    ::signal(SIGPIPE, SIG_IGN);

    auto state = std::make_shared<Subprocess::State>();
    const auto executor = rt::executor();
    asio::readable_pipe childIn(executor);
    asio::writable_pipe childOut(executor), childErr(executor);
    if (options.stdin == Stdio::pipe) {
      state->stdinSink = std::make_shared<FileSink::State>(executor);
      connectPipe(childIn, state->stdinSink->pipe);
    }
    if (options.stdout == Stdio::pipe) state->stdoutStream.emplace(pipeStream(childOut));
    if (options.stderr == Stdio::pipe) state->stderrStream.emplace(pipeStream(childErr));
    const NullDevice nullIn(options.stdin == Stdio::ignore, O_RDONLY);
    const NullDevice nullOut(options.stdout == Stdio::ignore || options.stderr == Stdio::ignore, O_WRONLY);
    bp::process_stdio stdio{
      .in = stdioBinding<decltype(stdio.in)>(options.stdin, childIn, nullIn),
      .out = stdioBinding<decltype(stdio.out)>(options.stdout, childOut, nullOut),
      .err = stdioBinding<decltype(stdio.err)>(options.stderr, childErr, nullOut),
    };
    state->child.emplace(launch(executor, file, cmd, options, stdio));
    state->pid = state->child->id();
    auto observation = watchExit(state);
    state->exited = Promise<int>(std::move(observation));
    return Subprocess(std::move(state));
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
