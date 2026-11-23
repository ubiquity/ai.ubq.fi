#include "uos/rt/process.hpp"

#include "uos/rt/env.hpp"
#include <sys/utsname.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <signal.h>
#include <sys/param.h>
#endif
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace uos::rt::process {
  namespace {
    std::vector<std::string>& argvStore() {
      static std::vector<std::string> store;
      return store;
    }

    std::optional<int>& exitCodeStore() {
      static std::optional<int> code;
      return code;
    }

#if defined(__APPLE__)
    struct NativeSignal {
      std::string_view name;
      int number;
    };
    constexpr NativeSignal NATIVE_SIGNALS[] = {
      {"SIGHUP", SIGHUP}, {"SIGINT", SIGINT}, {"SIGQUIT", SIGQUIT}, {"SIGILL", SIGILL}, {"SIGTRAP", SIGTRAP}, {"SIGABRT", SIGABRT},
      {"SIGEMT", SIGEMT}, {"SIGFPE", SIGFPE}, {"SIGKILL", SIGKILL}, {"SIGBUS", SIGBUS}, {"SIGSEGV", SIGSEGV}, {"SIGSYS", SIGSYS},
      {"SIGPIPE", SIGPIPE}, {"SIGALRM", SIGALRM}, {"SIGTERM", SIGTERM}, {"SIGURG", SIGURG}, {"SIGSTOP", SIGSTOP}, {"SIGTSTP", SIGTSTP},
      {"SIGCONT", SIGCONT}, {"SIGCHLD", SIGCHLD}, {"SIGTTIN", SIGTTIN}, {"SIGTTOU", SIGTTOU}, {"SIGIO", SIGIO}, {"SIGXCPU", SIGXCPU},
      {"SIGXFSZ", SIGXFSZ}, {"SIGVTALRM", SIGVTALRM}, {"SIGPROF", SIGPROF}, {"SIGWINCH", SIGWINCH}, {"SIGINFO", SIGINFO},
      {"SIGUSR1", SIGUSR1}, {"SIGUSR2", SIGUSR2}
    };
#else
    // Bun's names for the Linux signals 1..31.
    constexpr const char* SIGNAL_NAMES[] = {"SIGHUP",  "SIGINT",  "SIGQUIT", "SIGILL",  "SIGTRAP", "SIGABRT", "SIGBUS",    "SIGFPE",  "SIGKILL", "SIGUSR1", "SIGSEGV",
                                            "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGTERM", "SIG16",   "SIGCHLD", "SIGCONT",   "SIGSTOP", "SIGTSTP", "SIGTTIN", "SIGTTOU",
                                            "SIGURG",  "SIGXCPU", "SIGXFSZ", "SIGVTALRM", "SIGPROF", "SIGWINCH", "SIGIO", "SIGPWR",  "SIGSYS"};
#endif
  }

  const std::vector<std::string>& argv() { return argvStore(); }

  void setArgv(int argc, const char* const* arguments) {
    std::vector<std::string> out;
    out.push_back(execPath());
    if (argc > 0) {
      std::string script = arguments[0];
      if (script.find('/') == std::string::npos) script = out.front();
      else if (script.front() != '/') script = cwd() + "/" + script;
      out.push_back(std::move(script));
    }
    for (int index = 1; index < argc; ++index) out.emplace_back(arguments[index]);
    argvStore() = std::move(out);
  }

  ScopedArgv::ScopedArgv(std::vector<std::string> replacement) : previous_(std::exchange(argvStore(), std::move(replacement))) {}
  ScopedArgv::~ScopedArgv() { argvStore() = std::move(previous_); }

  std::string execPath() {
    std::string buffer(PATH_MAX, '\0');
#if defined(__APPLE__)
    auto size = static_cast<std::uint32_t>(buffer.size());
    if (::_NSGetExecutablePath(buffer.data(), &size) != 0) {
      buffer.resize(size);
      if (::_NSGetExecutablePath(buffer.data(), &size) != 0) return std::string();
    }
    // dyld may return a path containing symlinks; execPath is the real path.
    std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(buffer.c_str(), nullptr), &std::free);
    return resolved ? std::string(resolved.get()) : std::string();
#else
    const ssize_t length = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (length <= 0) return std::string();
    buffer.resize(static_cast<std::size_t>(length));
    return buffer;
#endif
  }

  std::string cwd() {
    std::string buffer(PATH_MAX, '\0');
    while (::getcwd(buffer.data(), buffer.size()) == nullptr) {
      if (errno != ERANGE) return std::string();
      buffer.resize(buffer.size() * 2);
    }
    buffer.resize(std::strlen(buffer.c_str()));
    return buffer;
  }

  int pid() { return static_cast<int>(::getpid()); }

  std::uint32_t getuid() { return static_cast<std::uint32_t>(::getuid()); }

  std::optional<int> exitCode() { return exitCodeStore(); }
  void setExitCode(std::optional<int> code) { exitCodeStore() = code; }

  int exitStatus() { return exitCodeStore().value_or(0) & 0xFF; }

  void exit(std::optional<int> code) {
    const int status = (code ? *code : exitCodeStore().value_or(0)) & 0xFF;
    std::fflush(nullptr);
    std::_Exit(status);
  }

  std::optional<int> signalNumber(std::string_view name) {
#if defined(__APPLE__)
    for (const auto& signal : NATIVE_SIGNALS)
      if (name == signal.name) return signal.number;
#else
    for (int index = 0; index < static_cast<int>(std::size(SIGNAL_NAMES)); ++index)
      if (name == SIGNAL_NAMES[index]) return index + 1;
#endif
    return std::nullopt;
  }

  std::optional<std::string_view> signalName(int number) {
#if defined(__APPLE__)
    for (const auto& signal : NATIVE_SIGNALS)
      if (number == signal.number) return signal.name;
    return std::nullopt;
#else
    if (number < 1 || number > static_cast<int>(std::size(SIGNAL_NAMES))) return std::nullopt;
    return SIGNAL_NAMES[number - 1];
#endif
  }
}

namespace uos::rt::os {
  std::string hostname() {
#if defined(__APPLE__)
    char buffer[MAXHOSTNAMELEN + 1] = {};
#else
    char buffer[HOST_NAME_MAX + 1] = {};
#endif
    if (::gethostname(buffer, sizeof buffer - 1) != 0) return std::string();
    return buffer;
  }

  std::string release() {
    struct utsname info;
    if (::uname(&info) != 0) return std::string();
    return info.release;
  }

  std::string tmpdir() {
    std::string path;
    for (const char* name : {"TMPDIR", "TMP", "TEMP"}) {
      auto value = env::get(name);
      if (value && !value->empty()) {
        path = std::move(*value);
        break;
      }
    }
    if (path.empty()) path = "/tmp";
    if (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
  }
}
