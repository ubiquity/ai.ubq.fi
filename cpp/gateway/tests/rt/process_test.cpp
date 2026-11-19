// node:process, node:os and Bun.spawn. Expected behaviour recorded from bun 1.4
// (proc1.ts, sig1/sig1b/sig3.ts, exit-code probes, spawn1/spawn2 probes in
// /tmp/claude-1002/platform/).
#include "uos/rt/process.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test.hpp"
#include <signal.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <sys/param.h>
#endif
#include <unistd.h>
#include <climits>
#include <string>
#include <vector>

using namespace uos;
namespace process = uos::rt::process;

namespace {
  // The exit status of `body` run in a forked child.
  int statusOf(void (*body)()) {
    std::fflush(nullptr);
    const pid_t child = ::fork();
    if (child == 0) {
      body();
      std::_Exit(99);
    }
    int status = 0;
    ::waitpid(child, &status, 0);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status);
  }

  bool defaultAction(int number) {
    struct sigaction current;
    ::sigaction(number, nullptr, &current);
    return current.sa_handler == SIG_DFL;
  }

  template <class F> std::string errorOf(F body) {
    try {
      body();
    } catch (const rt::fs::ErrnoError& error) {
      return error.name + "|" + error.message + "|" + error.code + "|" + std::to_string(error.errno_.value_or(0)) + "|" + error.syscall + "|" + error.path.value_or("-");
    } catch (const rt::JsError& error) {
      return error.name + "|" + error.message;
    }
    return "no error";
  }

  rt::Task<std::string> readAll(rt::ReadableStream<std::vector<std::uint8_t>> stream) {
    auto reader = stream.getReader();
    std::string out;
    for (;;) {
      auto step = co_await reader.read();
      if (step.done) break;
      out.append(step.value->begin(), step.value->end());
    }
    co_return out;
  }

  std::string signalOrder;
  void listenerA(std::string_view name, int number) { signalOrder += "a " + std::string(name) + " " + std::to_string(number) + ";"; }
}

TEST("platform, arch, pid, uid, cwd, execPath and Bun constants") {
#if defined(__APPLE__)
  EXPECT_EQ(std::string(process::platform), "darwin");
#else
  EXPECT_EQ(std::string(process::platform), "linux");
#endif
#if defined(__aarch64__)
  EXPECT_EQ(std::string(process::arch), "arm64");
#else
  EXPECT_EQ(std::string(process::arch), "x64");
#endif
  EXPECT_EQ(process::pid(), static_cast<int>(::getpid()));
  EXPECT_EQ(process::getuid(), static_cast<std::uint32_t>(::getuid()));
  EXPECT_EQ(process::cwd(), rt::fs::realpathSync("."));
#if defined(__APPLE__)
  std::vector<char> executable(PATH_MAX);
  auto size = static_cast<std::uint32_t>(executable.size());
  if (::_NSGetExecutablePath(executable.data(), &size) != 0) executable.resize(size);
  ASSERT_EQ(::_NSGetExecutablePath(executable.data(), &size), 0);
  EXPECT_EQ(process::execPath(), rt::fs::realpathSync(executable.data()));
#else
  // Under tools/unit.sh the binary runs through the SDK's dynamic loader, which
  // is then what /proc/self/exe names.
  EXPECT_EQ(process::execPath(), rt::fs::realpathSync("/proc/self/exe"));
#endif
  EXPECT(rt::path::isAbsolute(process::execPath()));
  EXPECT_EQ(std::string(rt::bun::version), "1.4.0");
  EXPECT_EQ(std::string(rt::bun::revision), "34cbb9a40b4bd1bd767d134a7065e66c2432a676");
}

TEST("argv is [execPath, script, ...arguments] so argv.slice(2) is the arguments") {
  const char* arguments[] = {"./bin/serve", "--port", "b c"};
  {
    process::ScopedArgv scope({});
    process::setArgv(3, arguments);
    const auto& argv = process::argv();
    ASSERT_EQ(argv.size(), std::size_t(4));
    EXPECT_EQ(argv[0], process::execPath());
    EXPECT_EQ(argv[1], process::cwd() + "/./bin/serve");
    EXPECT_EQ(std::vector<std::string>(argv.begin() + 2, argv.end()), std::vector<std::string>({"--port", "b c"}));
    const char* bare[] = {"serve"};
    process::setArgv(1, bare);
    EXPECT_EQ(process::argv(), std::vector<std::string>({process::execPath(), process::execPath()}));
  }
  process::ScopedArgv fixed({"/usr/local/bin/bun", "/srv/cli.ts", "--source", "x"});
  EXPECT_EQ(process::argv().at(2), "--source");
}

TEST("exitCode and exit statuses match Bun (masked to 8 bits)") {
  EXPECT_EQ(process::exitCode(), std::nullopt);
  EXPECT_EQ(process::exitStatus(), 0);
  process::setExitCode(5);
  EXPECT_EQ(process::exitCode(), std::optional<int>(5));
  EXPECT_EQ(process::exitStatus(), 5);
  process::setExitCode(259);
  EXPECT_EQ(process::exitStatus(), 3);
  process::setExitCode(-1);
  EXPECT_EQ(process::exitStatus(), 255);
  process::setExitCode(std::nullopt);
  EXPECT_EQ(statusOf([] {
              process::setExitCode(5);
              process::exit();
            }),
            5);
  EXPECT_EQ(statusOf([] {
              process::setExitCode(5);
              process::exit(2);
            }),
            2);
  EXPECT_EQ(statusOf([] { process::exit(256 + 3); }), 3);
  EXPECT_EQ(statusOf([] { process::exit(-1); }), 255);
  EXPECT_EQ(statusOf([] { process::exit(); }), 0);
}

TEST("signal names and numbers as Bun spells them") {
  EXPECT_EQ(process::signalNumber("SIGINT"), std::optional<int>(2));
  EXPECT_EQ(process::signalNumber("SIGTERM"), std::optional<int>(15));
#if defined(__APPLE__)
  EXPECT_EQ(process::signalNumber("SIG16"), std::nullopt);
  EXPECT_EQ(process::signalNumber("SIGSYS"), std::optional<int>(12));
#else
  EXPECT_EQ(process::signalNumber("SIG16"), std::optional<int>(16));
  EXPECT_EQ(process::signalNumber("SIGSYS"), std::optional<int>(31));
#endif
  EXPECT_EQ(process::signalNumber("SIGFOO"), std::nullopt);
  EXPECT_EQ(process::signalName(9), std::optional<std::string_view>("SIGKILL"));
  EXPECT_EQ(process::signalName(32), std::nullopt);
}

TEST("node:os hostname, release and tmpdir") {
#if defined(__APPLE__)
  char host[MAXHOSTNAMELEN + 1] = {};
#else
  char host[HOST_NAME_MAX + 1] = {};
#endif
  ::gethostname(host, sizeof host - 1);
  EXPECT_EQ(rt::os::hostname(), std::string(host));
  struct utsname info;
  ::uname(&info);
  EXPECT_EQ(rt::os::release(), std::string(info.release));
  rt::env::ScopedEnvironment scope({});
  EXPECT_EQ(rt::os::tmpdir(), "/tmp");
  rt::env::set("TMPDIR", "/from-js/");
  EXPECT_EQ(rt::os::tmpdir(), "/from-js");
  rt::env::set("TMPDIR", "/from-js");
  EXPECT_EQ(rt::os::tmpdir(), "/from-js");
  rt::env::set("TMPDIR", "");
  EXPECT_EQ(rt::os::tmpdir(), "/tmp");
  rt::env::remove("TMPDIR");
  rt::env::set("TMP", "/tmpvar");
  EXPECT_EQ(rt::os::tmpdir(), "/tmpvar");
  rt::env::set("TEMP", "/tempvar");
  rt::env::remove("TMP");
  EXPECT_EQ(rt::os::tmpdir(), "/tempvar");
  rt::env::set("TMPDIR", "/");
  EXPECT_EQ(rt::os::tmpdir(), "/");
}

TEST("signal listeners: dedupe, once, order, (name, number), default action restored") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    signalOrder.clear();
    const process::Listener a = listenerA;
    const process::Listener b = [](std::string_view, int) { signalOrder += "b;"; };
    process::once("SIGUSR2", a);
    process::on("SIGUSR2", b);
    process::on("SIGUSR2", b);
    EXPECT_EQ(process::listenerCount("SIGUSR2"), std::size_t(2));
    EXPECT(!defaultAction(SIGUSR2));
    ::kill(::getpid(), SIGUSR2);
    while (signalOrder.find("b;") == std::string::npos) co_await rt::sleep(1);
#if defined(__APPLE__)
    EXPECT_EQ(signalOrder, "a SIGUSR2 31;b;");
#else
    EXPECT_EQ(signalOrder, "a SIGUSR2 12;b;");
#endif
    EXPECT_EQ(process::listenerCount("SIGUSR2"), std::size_t(1));
    process::removeListener("SIGUSR2", b);
    EXPECT_EQ(process::listenerCount("SIGUSR2"), std::size_t(0));
    EXPECT(defaultAction(SIGUSR2));

    signalOrder.clear();
    const process::Listener c = [] { signalOrder += "c;"; };
    process::on("SIGUSR1", b);
    process::on("SIGUSR1", b);
    process::on("SIGUSR1", c);
    EXPECT_EQ(process::listenerCount("SIGUSR1"), std::size_t(2));
    ::kill(::getpid(), SIGUSR1);
    while (signalOrder.find("c;") == std::string::npos) co_await rt::sleep(1);
    EXPECT_EQ(signalOrder, "b;c;");
    process::off("SIGUSR1", b);
    EXPECT_EQ(process::listenerCount("SIGUSR1"), std::size_t(1));
    ::kill(::getpid(), SIGUSR1);
    while (signalOrder.size() < 6) co_await rt::sleep(1);
    EXPECT_EQ(signalOrder, "b;c;c;");
    process::removeListener("SIGUSR1", c);
    EXPECT(defaultAction(SIGUSR1));
    EXPECT_EQ(process::listenerCount("SIGINT"), std::size_t(0));
  }());
  EXPECT_THROWS(process::on("exit", process::Listener([] {})), std::invalid_argument);
}

TEST("destroying a loop removes its signal registrations before the Asio service dies") {
  const process::Listener listener = [] {};
  {
    rt::Loop local;
    rt::LoopScope scope(local);
    process::on("SIGUSR1", listener);
    EXPECT(!defaultAction(SIGUSR1));
  }
  EXPECT(defaultAction(SIGUSR1));
  rt::Loop later;
  rt::LoopScope scope(later);
  EXPECT_EQ(process::listenerCount("SIGUSR1"), 0u);
  process::off("SIGUSR1", listener);
}

TEST("Bun.spawn exit codes, signals, kill and killed (spawn1 probe)") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    auto three = process::spawn({"sh", "-c", "exit 3"}, {.stdout = process::Stdio::ignore});
    EXPECT(three.pid() > 0);
    EXPECT(!three.killed());
    EXPECT_EQ(three.exitCode(), std::nullopt);
    auto awaited1 = co_await three.exited().wait();
    EXPECT_EQ(awaited1, 3);
    EXPECT_EQ(three.exitCode(), std::optional<int>(3));
    EXPECT_EQ(three.signalCode(), std::nullopt);
    EXPECT(three.killed());

    auto term = process::spawn({"sleep", "5"});
    term.kill();
    EXPECT(!term.killed());
    auto awaited2 = co_await term.exited().wait();
    EXPECT_EQ(awaited2, 143);
    EXPECT_EQ(term.exitCode(), std::nullopt);
    EXPECT_EQ(term.signalCode(), std::optional<std::string>("SIGTERM"));
    EXPECT(term.killed());

    auto interrupt = process::spawn({"sleep", "5"});
    interrupt.kill("SIGINT");
    auto awaited3 = co_await interrupt.exited().wait();
    EXPECT_EQ(awaited3, 130);
    EXPECT_EQ(interrupt.signalCode(), std::optional<std::string>("SIGINT"));

    auto nine = process::spawn({"sleep", "5"});
    nine.kill(9);
    auto awaited4 = co_await nine.exited().wait();
    EXPECT_EQ(awaited4, 137);
    EXPECT_EQ(nine.signalCode(), std::optional<std::string>("SIGKILL"));

    auto self = process::spawn({"sh", "-c", "kill -USR1 $$"});
    auto awaited5 = co_await self.exited().wait();
#if defined(__APPLE__)
    // The native shell sends Darwin SIGUSR1 (30); Bun.spawn uses Linux names.
    EXPECT_EQ(awaited5, 158);
    EXPECT_EQ(self.signalCode(), std::optional<std::string>("SIGPWR"));
    auto namedSignal = process::spawn({"sleep", "5"});
    namedSignal.kill("SIGUSR1");
    const int namedExit = co_await namedSignal.exited().wait();
    EXPECT_EQ(namedExit, 138);
    EXPECT_EQ(namedSignal.signalCode(), std::optional<std::string>("SIGUSR1"));
#else
    EXPECT_EQ(awaited5, 138);
    EXPECT_EQ(self.signalCode(), std::optional<std::string>("SIGUSR1"));
#endif

    auto done = process::spawn({"true"});
    auto awaited6 = co_await done.exited().wait();
    EXPECT_EQ(awaited6, 0);
    done.kill();
    EXPECT_EQ(done.exitCode(), std::optional<int>(0));
    EXPECT_EQ(done.signalCode(), std::nullopt);

    auto bad = process::spawn({"sleep", "1"});
    EXPECT_EQ(errorOf([&] { bad.kill("SIGFOO"); }),
              "TypeError|signal must be one of 'SIGHUP', 'SIGINT', 'SIGQUIT', 'SIGILL', 'SIGTRAP', 'SIGABRT', 'SIGBUS', 'SIGFPE', 'SIGKILL', 'SIGUSR1', 'SIGSEGV', "
              "'SIGUSR2', 'SIGPIPE', 'SIGALRM', 'SIGTERM', 'SIG16', 'SIGCHLD', 'SIGCONT', 'SIGSTOP', 'SIGTSTP', 'SIGTTIN', 'SIGTTOU', 'SIGURG', 'SIGXCPU', "
              "'SIGXFSZ', 'SIGVTALRM', 'SIGPROF', 'SIGWINCH', 'SIGIO', 'SIGPWR' or 'SIGSYS'");
    EXPECT_EQ(errorOf([&] { bad.kill(99); }), "TypeError|Invalid signal: must be < 32");
    bad.kill(9);
    auto awaited7 = co_await bad.exited().wait();
    EXPECT_EQ(awaited7, 137);
  }());
}

TEST("Bun.spawn pipes: stdin FileSink, stdout and stderr streams (spawn1 probe)") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    auto cat = process::spawn({"cat"}, {.stdin = process::Stdio::pipe, .stdout = process::Stdio::pipe});
    auto sink = cat.stdin();
    EXPECT_EQ(sink.write("hello\n"), std::size_t(6));
    EXPECT_EQ(sink.write(std::vector<std::uint8_t>{'a', 'b'}), std::size_t(2));
    co_await sink.flush().wait();
    co_await sink.end().wait();
    auto awaited8 = co_await readAll(cat.stdout());
    EXPECT_EQ(awaited8, "hello\nab");
    auto awaited9 = co_await cat.exited().wait();
    EXPECT_EQ(awaited9, 0);

    auto both = process::spawn({"sh", "-c", "echo err >&2; echo out"}, {.stdout = process::Stdio::pipe, .stderr = process::Stdio::pipe});
    auto awaited10 = co_await readAll(both.stdout());
    EXPECT_EQ(awaited10, "out\n");
    auto awaited11 = co_await readAll(both.stderr());
    EXPECT_EQ(awaited11, "err\n");
    auto awaited12 = co_await both.exited().wait();
    EXPECT_EQ(awaited12, 0);

    auto large = process::spawn({"sh", "-c", "head -c 300000 /dev/zero"});
    const std::string large19 = co_await readAll(large.stdout());
    EXPECT_EQ(large19.size(), std::size_t(300000));
    auto awaited13 = co_await large.exited().wait();
    EXPECT_EQ(awaited13, 0);

    auto quiet = process::spawn({"true"}, {.stdout = process::Stdio::ignore});
    EXPECT_THROWS(quiet.stdin(), std::logic_error);
    EXPECT_THROWS(quiet.stdout(), std::logic_error);
    EXPECT_THROWS(quiet.stderr(), std::logic_error);
    co_await quiet.exited().wait();
  }());
}

#if defined(__APPLE__)
TEST("Darwin SIGCHLD watcher reaps concurrent immediate exits without zombie children") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    std::vector<process::Subprocess> children;
    for (int index = 0; index < 64; ++index)
      children.push_back(process::spawn({"sh", "-c", "exit " + std::to_string(index % 16)},
                                        {.stdout = process::Stdio::ignore, .stderr = process::Stdio::ignore}));
    for (std::size_t index = 0; index < children.size(); ++index) {
      auto& child = children[index];
      const int status = co_await child.exited().wait();
      EXPECT_EQ(status, static_cast<int>(index % 16));
      EXPECT_EQ(child.exitCode(), std::optional<int>(status));
      EXPECT(child.killed());
      int raw = 0;
      errno = 0;
      EXPECT_EQ(::waitpid(child.pid(), &raw, WNOHANG), -1);
      EXPECT_EQ(errno, ECHILD);
    }
  }());
}

TEST("Darwin child stdin returns EPIPE after the reader exits") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    auto child = process::spawn({"true"}, {.stdin = process::Stdio::pipe, .stdout = process::Stdio::ignore});
    const int status = co_await child.exited().wait();
    EXPECT_EQ(status, 0);
    auto sink = child.stdin();
    sink.write("after exit");
    bool rejected = false;
    try {
      co_await sink.flush().wait();
    } catch (const rt::fs::ErrnoError& error) {
      rejected = true;
      EXPECT_EQ(error.code, "EPIPE");
      EXPECT_EQ(error.errno_, std::optional<int>(-32));
    }
    EXPECT(rejected);
  }());
}
#endif

TEST("Bun.spawn environment, PATH lookup, cwd and spawn errors (spawn1/spawn2 probes)") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    auto empty = process::spawn({"env"}, {.env = std::vector<std::pair<std::string, std::string>>{}});
    auto awaited14 = co_await readAll(empty.stdout());
    EXPECT_EQ(awaited14, "");
    auto awaited15 = co_await empty.exited().wait();
    EXPECT_EQ(awaited15, 0);

    auto given = process::spawn({"env"}, {.env = std::vector<std::pair<std::string, std::string>>{{"B", "2"}, {"A", "1"}}});
    auto awaited16 = co_await readAll(given.stdout());
    EXPECT_EQ(awaited16, "B=2\nA=1\n");
    co_await given.exited().wait();

    // Without `env` the child gets the OS environment, not rt::env edits.
    rt::env::set("UOS_RT_SPAWN_JS_ONLY", "1");
    ::setenv("UOS_RT_SPAWN_OS_ONLY", "1", 1);
    auto inherited = process::spawn({"/usr/bin/env"});
    const std::string listing = co_await readAll(inherited.stdout());
    EXPECT(listing.find("UOS_RT_SPAWN_JS_ONLY=") == std::string::npos);
    EXPECT(listing.find("UOS_RT_SPAWN_OS_ONLY=1\n") != std::string::npos);
    co_await inherited.exited().wait();
    rt::env::remove("UOS_RT_SPAWN_JS_ONLY");
    ::unsetenv("UOS_RT_SPAWN_OS_ONLY");

    EXPECT_EQ(errorOf([] { process::spawn({"env"}, {.env = std::vector<std::pair<std::string, std::string>>{{"PATH", "/nonexistent"}}}); }),
              "Error|Executable not found in $PATH: \"env\"|ENOENT|-2||env");
    EXPECT_EQ(errorOf([] { process::spawn({"definitely-not-a-cmd-xyz"}); }), "Error|Executable not found in $PATH: \"definitely-not-a-cmd-xyz\"|ENOENT|-2||definitely-not-a-cmd-xyz");
    EXPECT_EQ(errorOf([] { process::spawn({"/nonexistent/bin"}); }),
              "Error|ENOENT: no such file or directory, posix_spawn '/nonexistent/bin'|ENOENT|-2|posix_spawn|/nonexistent/bin");
    EXPECT_EQ(errorOf([] { process::spawn({"/usr/bin/true"}, {.cwd = "/nonexistent-dir"}); }),
              "Error|ENOENT: no such file or directory, posix_spawn '/usr/bin/true'|ENOENT|-2|posix_spawn|/usr/bin/true");
#if defined(__APPLE__)
    EXPECT_EQ(errorOf([] { process::spawn({"/etc/hostname"}); }), "Error|ENOENT: no such file or directory, posix_spawn '/etc/hostname'|ENOENT|-2|posix_spawn|/etc/hostname");
#else
    EXPECT_EQ(errorOf([] { process::spawn({"/etc/hostname"}); }), "Error|EACCES: permission denied, posix_spawn '/etc/hostname'|EACCES|-13|posix_spawn|/etc/hostname");
#endif
    EXPECT_EQ(errorOf([] { process::spawn({}); }), "TypeError|cmd must not be empty");
    EXPECT_EQ(errorOf([] { process::spawn({"./nonexist"}, {.env = std::vector<std::pair<std::string, std::string>>{{"PATH", "/usr/bin"}}}); }),
              "Error|ENOENT: no such file or directory, posix_spawn './nonexist'|ENOENT|-2|posix_spawn|./nonexist");

    const std::string dir = rt::fs::mkdtempSync("/tmp/uos-rt-process-test-");
    rt::fs::writeFileSync(dir + "/x.sh", "#!/bin/sh\necho in-sp\n", {.mode = 0755});
    auto relative = process::spawn({"./x.sh"}, {.cwd = dir, .env = std::vector<std::pair<std::string, std::string>>{{"PATH", "/usr/bin"}}});
    auto awaited17 = co_await readAll(relative.stdout());
    EXPECT_EQ(awaited17, "in-sp\n");
    auto awaited18 = co_await relative.exited().wait();
    EXPECT_EQ(awaited18, 0);
    rt::fs::rmSync(dir, {.recursive = true, .force = true});
  }());
}

TEST("Bun.file(path).stream(): 256 KiB chunks and Bun's errors (file1 probe)") {
  rt::Loop loop;
  rt::blockOn(loop, []() -> rt::Task<void> {
    const std::string dir = rt::fs::mkdtempSync("/tmp/uos-rt-process-test-");
    rt::fs::writeFileSync(dir + "/big.bin", std::vector<std::uint8_t>(300000, 7));
    auto reader = rt::bun::file(dir + "/big.bin").stream().getReader();
    std::vector<std::size_t> sizes;
    for (;;) {
      auto step = co_await reader.read();
      if (step.done) break;
      sizes.push_back(step.value->size());
    }
    EXPECT_EQ(sizes, std::vector<std::size_t>({262144, 37856}));
    EXPECT_EQ(rt::bun::file(dir + "/big.bin").name(), dir + "/big.bin");
    auto awaited19 = co_await readAll(rt::bun::file("/dev/null").stream());
    EXPECT_EQ(awaited19, "");
    std::string missing = "no error", directory = "no error";
    try {
      co_await readAll(rt::bun::file(dir + "/nope.bin").stream());
    } catch (const rt::fs::ErrnoError& error) {
      missing = error.message + "|" + error.syscall + "|" + error.path.value_or("-");
    }
    EXPECT_EQ(missing, "ENOENT: no such file or directory, open '" + dir + "/nope.bin'|open|" + dir + "/nope.bin");
    try {
      co_await readAll(rt::bun::file(dir).stream());
    } catch (const rt::fs::ErrnoError& error) {
      directory = error.message + "|" + error.syscall + "|" + error.path.value_or("-");
    }
    EXPECT_EQ(directory, "EISDIR: illegal operation on a directory, fstat|fstat|-");
    rt::fs::rmSync(dir, {.recursive = true, .force = true});
  }());
}
