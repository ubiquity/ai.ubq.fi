// node:fs, node:fs/promises and node:path. Expected values and error objects
// were recorded from bun 1.4 (fs1.ts / fs2.ts / pathgen.ts probes); "<base>"
// is the test's temporary directory. The lexical deviations are recorded in
// docs/DEVIATIONS-FROM-BUN.md.
#include "uos/rt/date.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test.hpp"
#include <exception>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Platform-specific literals are recorded by Bun 1.4.0 on each host.
#if defined(__APPLE__)
#define DARWIN_EXPECTATION(linuxValue, darwinValue) darwinValue
#else
#define DARWIN_EXPECTATION(linuxValue, darwinValue) linuxValue
#endif

using namespace uos;
using namespace std::literals;
namespace fs = uos::rt::fs;

namespace {
  const timespec& accessTime(const struct stat& info) {
#if defined(__APPLE__)
    return info.st_atimespec;
#else
    return info.st_atim;
#endif
  }
  const timespec& modificationTime(const struct stat& info) {
#if defined(__APPLE__)
    return info.st_mtimespec;
#else
    return info.st_mtim;
#endif
  }
  const timespec& changeTime(const struct stat& info) {
#if defined(__APPLE__)
    return info.st_ctimespec;
#else
    return info.st_ctim;
#endif
  }
  struct TempDir {
    std::string path = fs::mkdtempSync(rt::os::tmpdir() + "/uos-rt-fs-test-");
    ~TempDir() {
      ::chmod((path + "/noperm").c_str(), 0600);
      fs::rmSync(path, {.recursive = true, .force = true});
    }
  };

  // "name|message|code|errno|syscall|path|dest|fd" of the thrown error.
  template <class F> std::string errorOf(F body) {
    try {
      body();
    } catch (const fs::ErrnoError& error) {
      return error.name + "|" + error.message + "|" + error.code + "|" + (error.errno_ ? std::to_string(*error.errno_) : "-") + "|" + error.syscall + "|" +
             error.path.value_or("-") + "|" + error.dest.value_or("-") + "|" + (error.fd ? std::to_string(*error.fd) : "-");
    } catch (const rt::JsError& error) {
      return error.name + "|" + error.message;
    }
    return "no error";
  }

  std::string errno_(std::string code, int number, std::string description, std::string syscall, std::string path = "", std::string dest = "") {
    std::string message = code + ": " + description + ", " + syscall;
    if (!path.empty()) message += " '" + path + "'";
    if (!dest.empty()) message += " -> '" + dest + "'";
    return "Error|" + message + "|" + code + "|" + std::to_string(number) + "|" + syscall + "|" + (path.empty() ? "-" : path) + "|" + (dest.empty() ? "-" : dest) + "|-";
  }

  std::string modeOf(const std::string& path) {
    char text[8];
    std::snprintf(text, sizeof text, "%o", fs::statSync(path).mode & 07777);
    return text;
  }

  struct Cwd {
    std::string previous = fs::realpathSync(".");
    explicit Cwd(const std::string& target) { ASSERT(::chdir(target.c_str()) == 0); }
    ~Cwd() { if (::chdir(previous.c_str()) != 0) std::terminate(); }
  };

  struct Umask {
    mode_t previous;
    explicit Umask(mode_t mask) : previous(::umask(mask)) {}
    ~Umask() { ::umask(previous); }
  };

  std::string filenameWithSuffixRemoved(std::string_view input, std::string_view suffix) {
    auto filename = std::filesystem::path(input).filename().native();
    if (filename.ends_with(suffix)) filename.resize(filename.size() - suffix.size());
    return filename;
  }

  std::span<const std::uint8_t> bytes(std::string_view text) { return {reinterpret_cast<const std::uint8_t*>(text.data()), text.size()}; }
}

TEST("POSIX lexical path functions (cwd /tmp)") {
  Cwd cwd("/tmp");
  EXPECT_EQ(std::string(rt::path::sep), "/");
  EXPECT_EQ(std::string(rt::path::delimiter), ":");
  EXPECT_EQ(rt::path::join("/raw//\xff\0.name"sv), std::string("/raw/\xff\0.name"sv));
  EXPECT_EQ(std::filesystem::path("/raw/\xff\0.name"sv).filename().native(), std::string("\xff\0.name"sv));
  EXPECT_EQ(rt::path::join(""sv), std::string("."sv));
  EXPECT_EQ(rt::path::join("."sv), std::string("."sv));
  EXPECT_EQ(rt::path::join(".."sv), std::string(".."sv));
  EXPECT_EQ(rt::path::join("/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join("//"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join("///"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join("a"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::join("a/"sv), std::string("a/"sv));
  EXPECT_EQ(rt::path::join("a//"sv), std::string("a/"sv));
  EXPECT_EQ(rt::path::join("/a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::join("/a/"sv), std::string("/a/"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::join("/a/b/"sv), std::string("/a/b/"sv));
  EXPECT_EQ(rt::path::join("a/b/c"sv), std::string("a/b/c"sv));
  EXPECT_EQ(rt::path::join("./a"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::join("../a"sv), std::string("../a"sv));
  EXPECT_EQ(rt::path::join("/a/b/../c"sv), std::string("/a/c"sv));
  EXPECT_EQ(rt::path::join("/a/./b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::join("a/../.."sv), std::string(".."sv));
  EXPECT_EQ(rt::path::join("../../a/.."sv), std::string("../.."sv));
  EXPECT_EQ(rt::path::join("//a//b//"sv), std::string("/a/b/"sv));
  EXPECT_EQ(rt::path::join("///a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::join("/a//b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::join("foo/bar/../baz/"sv), std::string("foo/baz/"sv));
  EXPECT_EQ(rt::path::join("./../a/"sv), std::string("../a/"sv));
  EXPECT_EQ(rt::path::join("/../a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::join("a/b/c/."sv), std::string("a/b/c/"sv));
  EXPECT_EQ(rt::path::join("a/b/c/.."sv), std::string("a/b/"sv));
  EXPECT_EQ(rt::path::join(".a"sv), std::string(".a"sv));
  EXPECT_EQ(rt::path::join("a."sv), std::string("a."sv));
  EXPECT_EQ(rt::path::join("a.b"sv), std::string("a.b"sv));
  EXPECT_EQ(rt::path::join("a.b.c"sv), std::string("a.b.c"sv));
  EXPECT_EQ(rt::path::join("/x/.hidden"sv), std::string("/x/.hidden"sv));
  EXPECT_EQ(rt::path::join("/x/file.tar.gz"sv), std::string("/x/file.tar.gz"sv));
  EXPECT_EQ(rt::path::join("..a"sv), std::string("..a"sv));
  EXPECT_EQ(rt::path::join("a.."sv), std::string("a.."sv));
  EXPECT_EQ(rt::path::join("/a/b.c/"sv), std::string("/a/b.c/"sv));
  EXPECT_EQ(rt::path::join("/a/b.c//"sv), std::string("/a/b.c/"sv));
  EXPECT_EQ(rt::path::join("a/.b"sv), std::string("a/.b"sv));
  EXPECT_EQ(rt::path::join(". ."sv), std::string(". ."sv));
  EXPECT_EQ(rt::path::join("..."sv), std::string("..."sv));
  EXPECT_EQ(rt::path::dirname(""sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("."sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname(".."sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::dirname("//"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::dirname("///"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::dirname("a"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("a/"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::dirname("a//"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::dirname("/a"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::dirname("/a/"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::dirname("/a/b"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::dirname("/a/b/"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::dirname("a/b/c"sv), std::string("a/b"sv));
  EXPECT_EQ(rt::path::dirname("./a"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("../a"sv), std::string(".."sv));
  EXPECT_EQ(rt::path::dirname("/a/b/../c"sv), std::string("/a/b/.."sv));
  EXPECT_EQ(rt::path::dirname("/a/./b"sv), std::string("/a/."sv));
  EXPECT_EQ(rt::path::dirname("a/../.."sv), std::string("a/.."sv));
  EXPECT_EQ(rt::path::dirname("../../a/.."sv), std::string("../../a"sv));
  EXPECT_EQ(rt::path::dirname("//a//b//"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::dirname("///a"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::dirname("/a//b"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::dirname("foo/bar/../baz/"sv), std::string("foo/bar/../baz"sv));
  EXPECT_EQ(rt::path::dirname("./../a/"sv), std::string("./../a"sv));
  EXPECT_EQ(rt::path::dirname("/../a"sv), std::string("/.."sv));
  EXPECT_EQ(rt::path::dirname("a/b/c/."sv), std::string("a/b/c"sv));
  EXPECT_EQ(rt::path::dirname("a/b/c/.."sv), std::string("a/b/c"sv));
  EXPECT_EQ(rt::path::dirname(".a"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("a."sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("a.b"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("a.b.c"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("/x/.hidden"sv), std::string("/x"sv));
  EXPECT_EQ(rt::path::dirname("/x/file.tar.gz"sv), std::string("/x"sv));
  EXPECT_EQ(rt::path::dirname("..a"sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("a.."sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("/a/b.c/"sv), std::string("/a/b.c"sv));
  EXPECT_EQ(rt::path::dirname("/a/b.c//"sv), std::string("/a/b.c"sv));
  EXPECT_EQ(rt::path::dirname("a/.b"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::dirname(". ."sv), std::string("."sv));
  EXPECT_EQ(rt::path::dirname("..."sv), std::string("."sv));
  EXPECT_EQ(std::filesystem::path(""sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("."sv).filename().native(), std::string("."sv));
  EXPECT_EQ(std::filesystem::path(".."sv).filename().native(), std::string(".."sv));
  EXPECT_EQ(std::filesystem::path("/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("//"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("///"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("a/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a//"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("/a/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b"sv).filename().native(), std::string("b"sv));
  EXPECT_EQ(std::filesystem::path("/a/b/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/b/c"sv).filename().native(), std::string("c"sv));
  EXPECT_EQ(std::filesystem::path("./a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("../a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("/a/b/../c"sv).filename().native(), std::string("c"sv));
  EXPECT_EQ(std::filesystem::path("/a/./b"sv).filename().native(), std::string("b"sv));
  EXPECT_EQ(std::filesystem::path("a/../.."sv).filename().native(), std::string(".."sv));
  EXPECT_EQ(std::filesystem::path("../../a/.."sv).filename().native(), std::string(".."sv));
  EXPECT_EQ(std::filesystem::path("//a//b//"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("///a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("/a//b"sv).filename().native(), std::string("b"sv));
  EXPECT_EQ(std::filesystem::path("foo/bar/../baz/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("./../a/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/../a"sv).filename().native(), std::string("a"sv));
  EXPECT_EQ(std::filesystem::path("a/b/c/."sv).filename().native(), std::string("."sv));
  EXPECT_EQ(std::filesystem::path("a/b/c/.."sv).filename().native(), std::string(".."sv));
  EXPECT_EQ(std::filesystem::path(".a"sv).filename().native(), std::string(".a"sv));
  EXPECT_EQ(std::filesystem::path("a."sv).filename().native(), std::string("a."sv));
  EXPECT_EQ(std::filesystem::path("a.b"sv).filename().native(), std::string("a.b"sv));
  EXPECT_EQ(std::filesystem::path("a.b.c"sv).filename().native(), std::string("a.b.c"sv));
  EXPECT_EQ(std::filesystem::path("/x/.hidden"sv).filename().native(), std::string(".hidden"sv));
  EXPECT_EQ(std::filesystem::path("/x/file.tar.gz"sv).filename().native(), std::string("file.tar.gz"sv));
  EXPECT_EQ(std::filesystem::path("..a"sv).filename().native(), std::string("..a"sv));
  EXPECT_EQ(std::filesystem::path("a.."sv).filename().native(), std::string("a.."sv));
  EXPECT_EQ(std::filesystem::path("/a/b.c/"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b.c//"sv).filename().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/.b"sv).filename().native(), std::string(".b"sv));
  EXPECT_EQ(std::filesystem::path(". ."sv).filename().native(), std::string(". ."sv));
  EXPECT_EQ(std::filesystem::path("..."sv).filename().native(), std::string("..."sv));
  EXPECT_EQ(std::filesystem::path(""sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path(".."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("//"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("///"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a//"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/b/c"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("./a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("../a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b/../c"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/./b"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/../.."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("../../a/.."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("//a//b//"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("///a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a//b"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("foo/bar/../baz/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("./../a/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/../a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/b/c/."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/b/c/.."sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path(".a"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a."sv).extension().native(), std::string("."sv));
  EXPECT_EQ(std::filesystem::path("a.b"sv).extension().native(), std::string(".b"sv));
  EXPECT_EQ(std::filesystem::path("a.b.c"sv).extension().native(), std::string(".c"sv));
  EXPECT_EQ(std::filesystem::path("/x/.hidden"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/x/file.tar.gz"sv).extension().native(), std::string(".gz"sv));
  EXPECT_EQ(std::filesystem::path("..a"sv).extension().native(), std::string(".a"sv));
  EXPECT_EQ(std::filesystem::path("a.."sv).extension().native(), std::string("."sv));
  EXPECT_EQ(std::filesystem::path("/a/b.c/"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("/a/b.c//"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path("a/.b"sv).extension().native(), std::string(""sv));
  EXPECT_EQ(std::filesystem::path(". ."sv).extension().native(), std::string("."sv));
  EXPECT_EQ(std::filesystem::path("..."sv).extension().native(), std::string("."sv));
  EXPECT_EQ(rt::path::isAbsolute(""sv), false);
  EXPECT_EQ(rt::path::isAbsolute("."sv), false);
  EXPECT_EQ(rt::path::isAbsolute(".."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("//"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("///"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("a"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a/"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a//"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/a"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a/"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a/b"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a/b/"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("a/b/c"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("./a"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("../a"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/a/b/../c"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a/./b"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("a/../.."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("../../a/.."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("//a//b//"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("///a"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a//b"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("foo/bar/../baz/"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("./../a/"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/../a"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("a/b/c/."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a/b/c/.."sv), false);
  EXPECT_EQ(rt::path::isAbsolute(".a"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a.b"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a.b.c"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/x/.hidden"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/x/file.tar.gz"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("..a"sv), false);
  EXPECT_EQ(rt::path::isAbsolute("a.."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("/a/b.c/"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("/a/b.c//"sv), true);
  EXPECT_EQ(rt::path::isAbsolute("a/.b"sv), false);
  EXPECT_EQ(rt::path::isAbsolute(". ."sv), false);
  EXPECT_EQ(rt::path::isAbsolute("..."sv), false);
  EXPECT_EQ(rt::path::resolve(""sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve("."sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve(".."sv), std::string(DARWIN_EXPECTATION("/"sv, "/private"sv)));
  EXPECT_EQ(rt::path::resolve("/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("//"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("///"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("a"sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve("a/"sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve("a//"sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve("/a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("/a/"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("/a/b/"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("a/b/c"sv), std::string(DARWIN_EXPECTATION("/tmp/a/b/c"sv, "/private/tmp/a/b/c"sv)));
  EXPECT_EQ(rt::path::resolve("./a"sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve("../a"sv), std::string(DARWIN_EXPECTATION("/a"sv, "/private/a"sv)));
  EXPECT_EQ(rt::path::resolve("/a/b/../c"sv), std::string("/a/c"sv));
  EXPECT_EQ(rt::path::resolve("/a/./b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("a/../.."sv), std::string(DARWIN_EXPECTATION("/"sv, "/private"sv)));
  EXPECT_EQ(rt::path::resolve("../../a/.."sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("//a//b//"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("///a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("/a//b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("foo/bar/../baz/"sv), std::string(DARWIN_EXPECTATION("/tmp/foo/baz"sv, "/private/tmp/foo/baz"sv)));
  EXPECT_EQ(rt::path::resolve("./../a/"sv), std::string(DARWIN_EXPECTATION("/a"sv, "/private/a"sv)));
  EXPECT_EQ(rt::path::resolve("/../a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("a/b/c/."sv), std::string(DARWIN_EXPECTATION("/tmp/a/b/c"sv, "/private/tmp/a/b/c"sv)));
  EXPECT_EQ(rt::path::resolve("a/b/c/.."sv), std::string(DARWIN_EXPECTATION("/tmp/a/b"sv, "/private/tmp/a/b"sv)));
  EXPECT_EQ(rt::path::resolve(".a"sv), std::string(DARWIN_EXPECTATION("/tmp/.a"sv, "/private/tmp/.a"sv)));
  EXPECT_EQ(rt::path::resolve("a."sv), std::string(DARWIN_EXPECTATION("/tmp/a."sv, "/private/tmp/a."sv)));
  EXPECT_EQ(rt::path::resolve("a.b"sv), std::string(DARWIN_EXPECTATION("/tmp/a.b"sv, "/private/tmp/a.b"sv)));
  EXPECT_EQ(rt::path::resolve("a.b.c"sv), std::string(DARWIN_EXPECTATION("/tmp/a.b.c"sv, "/private/tmp/a.b.c"sv)));
  EXPECT_EQ(rt::path::resolve("/x/.hidden"sv), std::string("/x/.hidden"sv));
  EXPECT_EQ(rt::path::resolve("/x/file.tar.gz"sv), std::string("/x/file.tar.gz"sv));
  EXPECT_EQ(rt::path::resolve("..a"sv), std::string(DARWIN_EXPECTATION("/tmp/..a"sv, "/private/tmp/..a"sv)));
  EXPECT_EQ(rt::path::resolve("a.."sv), std::string(DARWIN_EXPECTATION("/tmp/a.."sv, "/private/tmp/a.."sv)));
  EXPECT_EQ(rt::path::resolve("/a/b.c/"sv), std::string("/a/b.c"sv));
  EXPECT_EQ(rt::path::resolve("/a/b.c//"sv), std::string("/a/b.c"sv));
  EXPECT_EQ(rt::path::resolve("a/.b"sv), std::string(DARWIN_EXPECTATION("/tmp/a/.b"sv, "/private/tmp/a/.b"sv)));
  EXPECT_EQ(rt::path::resolve(". ."sv), std::string(DARWIN_EXPECTATION("/tmp/. ."sv, "/private/tmp/. ."sv)));
  EXPECT_EQ(rt::path::resolve("..."sv), std::string(DARWIN_EXPECTATION("/tmp/..."sv, "/private/tmp/..."sv)));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b.txt"sv, ".txt"sv), std::string("b"sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b.txt"sv, "txt"sv), std::string("b."sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b.txt"sv, "b.txt"sv), std::string(""sv));
  EXPECT_EQ(filenameWithSuffixRemoved("b.txt"sv, "b.txt"sv), std::string(""sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b.txt/"sv, ".txt"sv), std::string(""sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b.txt"sv, ".TXT"sv), std::string("b.txt"sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/b"sv, ""sv), std::string("b"sv));
  EXPECT_EQ(filenameWithSuffixRemoved("a.js.js"sv, ".js"sv), std::string("a.js"sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/a/.js"sv, ".js"sv), std::string(""sv));
  EXPECT_EQ(filenameWithSuffixRemoved("x"sv, "xx"sv), std::string("x"sv));
  EXPECT_EQ(filenameWithSuffixRemoved("/aaa/"sv, "a"sv), std::string(""sv));
  EXPECT_EQ(filenameWithSuffixRemoved("//"sv, "/"sv), std::string(""sv));
  EXPECT_EQ(rt::path::relative("a"sv, "b"sv), std::string("../b"sv));
  EXPECT_EQ(rt::path::relative("/a"sv, "../b"sv), std::string(DARWIN_EXPECTATION("../b"sv, "../private/b"sv)));
  EXPECT_EQ(rt::path::relative(""sv, ""sv), std::string(""sv));
  EXPECT_EQ(rt::path::relative("a"sv, ""sv), std::string(".."sv));
  EXPECT_EQ(rt::path::relative(""sv, "a"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::relative("/a/"sv, "/b/"sv), std::string("../b"sv));
  EXPECT_EQ(rt::path::relative("a"sv, "/b"sv), std::string(DARWIN_EXPECTATION("../../b"sv, "../../../b"sv)));
  EXPECT_EQ(rt::path::relative("."sv, "."sv), std::string(""sv));
  EXPECT_EQ(rt::path::relative("/"sv, ".."sv), std::string(DARWIN_EXPECTATION(""sv, "private"sv)));
  EXPECT_EQ(rt::path::relative("/a/b"sv, "/a/b"sv), std::string(""sv));
  EXPECT_EQ(rt::path::relative("/a/b"sv, "/a/b/c/d"sv), std::string("c/d"sv));
  EXPECT_EQ(rt::path::relative("/a/b/c"sv, "/a/d"sv), std::string("../../d"sv));
  EXPECT_EQ(rt::path::relative("/a"sv, "/"sv), std::string(".."sv));
  EXPECT_EQ(rt::path::relative("/"sv, "/a"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::relative("/a/b"sv, "/a/bc"sv), std::string("../bc"sv));
  EXPECT_EQ(rt::path::relative("/a/bc"sv, "/a/b"sv), std::string("../b"sv));
  EXPECT_EQ(rt::path::relative("/data"sv, "/data/x/y"sv), std::string("x/y"sv));
  EXPECT_EQ(rt::path::relative("/data"sv, "/other"sv), std::string("../other"sv));
  EXPECT_EQ(rt::path::relative("/data/"sv, "/data/x"sv), std::string("x"sv));
  EXPECT_EQ(rt::path::relative("/a/b"sv, "a/b"sv), std::string(DARWIN_EXPECTATION("../../tmp/a/b"sv, "../../private/tmp/a/b"sv)));
  EXPECT_EQ(rt::path::relative("/home/x/.codex"sv, "app-server-control"sv), std::string(DARWIN_EXPECTATION("../../../tmp/app-server-control"sv, "../../../private/tmp/app-server-control"sv)));
  EXPECT_EQ(rt::path::relative("/r"sv, ".data"sv), std::string(DARWIN_EXPECTATION("../tmp/.data"sv, "../private/tmp/.data"sv)));
  EXPECT_EQ(rt::path::relative("a/"sv, ".."sv), std::string("../.."sv));
  EXPECT_EQ(rt::path::relative("/x/y/"sv, "../.."sv), std::string("../.."sv));
  EXPECT_EQ(rt::path::relative(""sv, "/"sv), std::string(DARWIN_EXPECTATION(".."sv, "../.."sv)));
  EXPECT_EQ(rt::path::relative("/"sv, ""sv), std::string(DARWIN_EXPECTATION("tmp"sv, "private/tmp"sv)));
  EXPECT_EQ(rt::path::join("a"sv, "b"sv), std::string("a/b"sv));
  EXPECT_EQ(rt::path::join("/a"sv, "../b"sv), std::string("/b"sv));
  EXPECT_EQ(rt::path::join(""sv, ""sv), std::string("."sv));
  EXPECT_EQ(rt::path::join("a"sv, ""sv), std::string("a"sv));
  EXPECT_EQ(rt::path::join(""sv, "a"sv), std::string("a"sv));
  EXPECT_EQ(rt::path::join("/a/"sv, "/b/"sv), std::string("/a/b/"sv));
  EXPECT_EQ(rt::path::join("a"sv, "/b"sv), std::string("a/b"sv));
  EXPECT_EQ(rt::path::join("."sv, "."sv), std::string("."sv));
  EXPECT_EQ(rt::path::join("/"sv, ".."sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv, "/a/b"sv), std::string("/a/b/a/b"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv, "/a/b/c/d"sv), std::string("/a/b/a/b/c/d"sv));
  EXPECT_EQ(rt::path::join("/a/b/c"sv, "/a/d"sv), std::string("/a/b/c/a/d"sv));
  EXPECT_EQ(rt::path::join("/a"sv, "/"sv), std::string("/a/"sv));
  EXPECT_EQ(rt::path::join("/"sv, "/a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv, "/a/bc"sv), std::string("/a/b/a/bc"sv));
  EXPECT_EQ(rt::path::join("/a/bc"sv, "/a/b"sv), std::string("/a/bc/a/b"sv));
  EXPECT_EQ(rt::path::join("/data"sv, "/data/x/y"sv), std::string("/data/data/x/y"sv));
  EXPECT_EQ(rt::path::join("/data"sv, "/other"sv), std::string("/data/other"sv));
  EXPECT_EQ(rt::path::join("/data/"sv, "/data/x"sv), std::string("/data/data/x"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv, "a/b"sv), std::string("/a/b/a/b"sv));
  EXPECT_EQ(rt::path::join("/home/x/.codex"sv, "app-server-control"sv), std::string("/home/x/.codex/app-server-control"sv));
  EXPECT_EQ(rt::path::join("/r"sv, ".data"sv), std::string("/r/.data"sv));
  EXPECT_EQ(rt::path::join("a/"sv, ".."sv), std::string("."sv));
  EXPECT_EQ(rt::path::join("/x/y/"sv, "../.."sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join(""sv, "/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join("/"sv, ""sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("a"sv, "b"sv), std::string(DARWIN_EXPECTATION("/tmp/a/b"sv, "/private/tmp/a/b"sv)));
  EXPECT_EQ(rt::path::resolve("/a"sv, "../b"sv), std::string("/b"sv));
  EXPECT_EQ(rt::path::resolve(""sv, ""sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve("a"sv, ""sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve(""sv, "a"sv), std::string(DARWIN_EXPECTATION("/tmp/a"sv, "/private/tmp/a"sv)));
  EXPECT_EQ(rt::path::resolve("/a/"sv, "/b/"sv), std::string("/b"sv));
  EXPECT_EQ(rt::path::resolve("a"sv, "/b"sv), std::string("/b"sv));
  EXPECT_EQ(rt::path::resolve("."sv, "."sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve("/"sv, ".."sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv, "/a/b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv, "/a/b/c/d"sv), std::string("/a/b/c/d"sv));
  EXPECT_EQ(rt::path::resolve("/a/b/c"sv, "/a/d"sv), std::string("/a/d"sv));
  EXPECT_EQ(rt::path::resolve("/a"sv, "/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("/"sv, "/a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv, "/a/bc"sv), std::string("/a/bc"sv));
  EXPECT_EQ(rt::path::resolve("/a/bc"sv, "/a/b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("/data"sv, "/data/x/y"sv), std::string("/data/x/y"sv));
  EXPECT_EQ(rt::path::resolve("/data"sv, "/other"sv), std::string("/other"sv));
  EXPECT_EQ(rt::path::resolve("/data/"sv, "/data/x"sv), std::string("/data/x"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv, "a/b"sv), std::string("/a/b/a/b"sv));
  EXPECT_EQ(rt::path::resolve("/home/x/.codex"sv, "app-server-control"sv), std::string("/home/x/.codex/app-server-control"sv));
  EXPECT_EQ(rt::path::resolve("/r"sv, ".data"sv), std::string("/r/.data"sv));
  EXPECT_EQ(rt::path::resolve("a/"sv, ".."sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve("/x/y/"sv, "../.."sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve(""sv, "/"sv), std::string("/"sv));
  EXPECT_EQ(rt::path::resolve("/"sv, ""sv), std::string("/"sv));
  EXPECT_EQ(rt::path::join(), std::string("."sv));
  EXPECT_EQ(rt::path::join("a"sv, "b"sv, "c"sv), std::string("a/b/c"sv));
  EXPECT_EQ(rt::path::join("/a"sv, "b"sv, "../c"sv, "./d"sv), std::string("/a/c/d"sv));
  EXPECT_EQ(rt::path::join("a/"sv, ".."sv, ".."sv), std::string(".."sv));
  EXPECT_EQ(rt::path::join("/"sv, ".."sv, "a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::join("/r"sv, ".data"sv, "native-kv.binding.json"sv), std::string("/r/.data/native-kv.binding.json"sv));
  EXPECT_EQ(rt::path::join("/a"sv, ""sv, "b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::join("/a//b/"sv, "./c/"sv), std::string("/a/b/c/"sv));
  EXPECT_EQ(rt::path::join("/a/b"sv, ".."sv, "c"sv), std::string("/a/c"sv));
  EXPECT_EQ(rt::path::join("x"sv, "/y"sv, "z"sv), std::string("x/y/z"sv));
  EXPECT_EQ(rt::path::join(""sv, ""sv, ""sv), std::string("."sv));
  EXPECT_EQ(rt::path::resolve(), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
  EXPECT_EQ(rt::path::resolve("a"sv, "b"sv, "c"sv), std::string(DARWIN_EXPECTATION("/tmp/a/b/c"sv, "/private/tmp/a/b/c"sv)));
  EXPECT_EQ(rt::path::resolve("/a"sv, "b"sv, "../c"sv, "./d"sv), std::string("/a/c/d"sv));
  EXPECT_EQ(rt::path::resolve("a/"sv, ".."sv, ".."sv), std::string(DARWIN_EXPECTATION("/"sv, "/private"sv)));
  EXPECT_EQ(rt::path::resolve("/"sv, ".."sv, "a"sv), std::string("/a"sv));
  EXPECT_EQ(rt::path::resolve("/r"sv, ".data"sv, "native-kv.binding.json"sv), std::string("/r/.data/native-kv.binding.json"sv));
  EXPECT_EQ(rt::path::resolve("/a"sv, ""sv, "b"sv), std::string("/a/b"sv));
  EXPECT_EQ(rt::path::resolve("/a//b/"sv, "./c/"sv), std::string("/a/b/c"sv));
  EXPECT_EQ(rt::path::resolve("/a/b"sv, ".."sv, "c"sv), std::string("/a/c"sv));
  EXPECT_EQ(rt::path::resolve("x"sv, "/y"sv, "z"sv), std::string("/y/z"sv));
  EXPECT_EQ(rt::path::resolve(""sv, ""sv, ""sv), std::string(DARWIN_EXPECTATION("/tmp"sv, "/private/tmp"sv)));
}

TEST("lexical paths preserve raw bytes and distinguish containment boundaries") {
  EXPECT_EQ(rt::path::join("/store", "", "//", "/%2F\\part//"), "/store/%2F\\part/");
  EXPECT_EQ(rt::path::resolve("/store", "", "//", "/%2F\\part//"), "/%2F\\part");
  EXPECT_EQ(rt::path::dirname("//store//%2F\\part///file"), "/store/%2F\\part");
  EXPECT(!rt::path::isAbsolute("\\store"));
  EXPECT_EQ(rt::path::relative("/store", "/store//./"), "");
  EXPECT_EQ(rt::path::relative("/store", "/store/.."), "..");
  EXPECT_EQ(rt::path::relative("/store", "/store/../store-old/file"), "../store-old/file");
  EXPECT_EQ(rt::path::relative("/store", "/store/%2e%2e\\file"), "%2e%2e\\file");
}

#if defined(__linux__)
TEST("relative paths fail closed when the current directory disappears") {
  TempDir dir;
  const std::string removed = dir.path + "/removed";
  fs::mkdirSync(removed);
  Cwd cwd(removed);
  ASSERT(::rmdir(removed.c_str()) == 0);
  EXPECT_EQ(rt::path::resolve("discarded", "//store", "file"), "/store/file");
  EXPECT_EQ(rt::path::relative("/store", "/store/file"), "file");
  EXPECT_THROWS(rt::path::relative("./file", "./file"), rt::JsError);
}
#endif

TEST("fs error objects: open, read, stat, lstat, realpath (fs1 probe)") {
  TempDir dir;
  const std::string base = dir.path;
  EXPECT_EQ(errorOf([&] { fs::readFileSync(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::readFileSync(base + "/missing", "utf8"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::readFileSync(base); }), errno_("EISDIR", -21, "illegal operation on a directory", "read"));
  EXPECT_EQ(errorOf([&] { fs::promises::readFile(base + "/missing", "utf8"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::promises::readFile(base, "utf8"); }), errno_("EISDIR", -21, "illegal operation on a directory", "read"));
  EXPECT_EQ(errorOf([&] { fs::lstatSync(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::statSync(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "stat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::promises::stat(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "stat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::realpathSync(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::promises::realpath(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::readFileSync(""); }), errno_("ENOENT", -2, "no such file or directory", "open"));
  EXPECT_EQ(errorOf([&] { fs::promises::readFile(""); }), errno_("ENOENT", -2, "no such file or directory", "open"));
  EXPECT_EQ(errorOf([&] { fs::lstatSync(""); }), errno_("ENOENT", -2, "no such file or directory", "lstat"));
  EXPECT_EQ(errorOf([&] { fs::lstatSync("relative-missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", "relative-missing"));
  const std::string unicode = base + "/\xc3\xbcn\xc3\xaf \"q'";
  EXPECT_EQ(errorOf([&] { fs::readFileSync(unicode); }), errno_("ENOENT", -2, "no such file or directory", "open", unicode));
  const std::string longName = base + "/" + std::string(300, 'x');
  EXPECT_EQ(errorOf([&] { fs::readFileSync(longName); }), errno_("ENAMETOOLONG", DARWIN_EXPECTATION(-36, -63), "name too long", "open", longName));
  fs::writeFileSync(base + "/file1", "x");
  EXPECT_EQ(errorOf([&] { fs::readFileSync(base + "/file1/x"); }), errno_("ENOTDIR", -20, "not a directory", "open", base + "/file1/x"));
  if (::geteuid() != 0) {
    fs::writeFileSync(base + "/noperm", "x");
    fs::chmodSync(base + "/noperm", 0);
    EXPECT_EQ(errorOf([&] { fs::readFileSync(base + "/noperm"); }), errno_("EACCES", -13, "permission denied", "open", base + "/noperm"));
  }
  EXPECT_EQ(errorOf([&] { fs::readdirSync(base + "/missing", fs::withFileTypes); }), errno_("ENOENT", -2, "no such file or directory", "scandir", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::promises::readdir(base + "/file1", fs::withFileTypes); }), errno_("ENOTDIR", -20, "not a directory", "scandir", base + "/file1"));
  EXPECT_EQ(errorOf([&] { fs::promises::open(base + "/missing", "r"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::chmodSync(base + "/missing", 0600); }), errno_("ENOENT", -2, "no such file or directory", "chmod", base + "/missing"));
}

TEST("fs error objects: descriptors, create flags, copy and link (fs1 probe)") {
  TempDir dir;
  const std::string base = dir.path;
  Umask mask(022);
  fs::writeFileSync(base + "/file1", "x");
  const int fd = fs::openSync(base + "/wx1", "wx", 0600);
  EXPECT(fd >= 0);
  fs::closeSync(fd);
  EXPECT_EQ(modeOf(base + "/wx1"), "600");
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/wx1", "wx", 0600); }), errno_("EEXIST", -17, "file already exists", "open", base + "/wx1"));
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/missing", "r"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/missing"));
  ASSERT(::symlink((base + "/file1").c_str(), (base + "/link1").c_str()) == 0);
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/link1", fs::constants::O_RDONLY_ | fs::constants::O_NOFOLLOW_); }),
            errno_("ELOOP", DARWIN_EXPECTATION(-40, -62), "too many symbolic links encountered", "open", base + "/link1"));
  EXPECT_EQ(errorOf([&] { fs::closeSync(99999); }), "Error|EBADF: bad file descriptor, close|EBADF|-9|close|-|-|99999");
  EXPECT_EQ(errorOf([&] { fs::fstatSync(99999); }), errno_("EBADF", -9, "bad file descriptor", "fstat"));
  EXPECT_EQ(errorOf([&] { fs::fsyncSync(99999); }), errno_("EBADF", -9, "bad file descriptor", "fsync"));
  EXPECT_EQ(errorOf([&] { fs::readFileSync(99999, "utf8"); }), errno_("EBADF", -9, "bad file descriptor", "read"));
  EXPECT_EQ(errorOf([&] { fs::writeFileSync(base + "/file1", "y", {.flag = "wx", .mode = 0600}); }), errno_("EEXIST", -17, "file already exists", "open", base + "/file1"));
  EXPECT_EQ(errorOf([&] { fs::writeFileSync(base + "/nodir/x", "y"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/nodir/x"));
  EXPECT_EQ(errorOf([&] { fs::promises::writeFile(base + "/nodir/x", "y"); }), errno_("ENOENT", -2, "no such file or directory", "open", base + "/nodir/x"));
  EXPECT_EQ(errorOf([&] { fs::copyFileSync(base + "/file1", base + "/wx1", fs::constants::COPYFILE_EXCL); }),
            errno_("EEXIST", -17, "file already exists", "copyfile", base + "/file1", base + "/wx1"));
  EXPECT_EQ(errorOf([&] { fs::copyFileSync(base + "/missing", base + "/c2", fs::constants::COPYFILE_EXCL); }),
            errno_("ENOENT", -2, "no such file or directory", "copyfile", base + "/missing", base + "/c2"));
  EXPECT_EQ(errorOf([&] { fs::linkSync(base + "/file1", base + "/wx1"); }), errno_("EEXIST", -17, "file already exists", "link", base + "/file1", base + "/wx1"));
  EXPECT_EQ(errorOf([&] { fs::linkSync(base + "/missing", base + "/l2"); }), errno_("ENOENT", -2, "no such file or directory", "link", base + "/missing", base + "/l2"));
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/file1", "z"); }), "TypeError|The argument 'flags' is invalid. Received 'z'");
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/file1", "q"); }), "TypeError|The argument 'flags' is invalid. Received 'q'");
  EXPECT_EQ(errorOf([&] { fs::openSync(base + "/file1", "rw"); }), "TypeError|The argument 'flags' is invalid. Received 'rw'");
  EXPECT_EQ(errorOf([&] { fs::closeSync(fs::openSync(base + "/file1", "r+")); }), "no error");
  EXPECT_EQ(errorOf([&] { fs::closeSync(fs::openSync(base, "r")); }), "no error");
  EXPECT_EQ(errorOf([&] { fs::openSync(base, "w"); }), errno_("EISDIR", -21, "illegal operation on a directory", "open", base));
  const int twice = fs::openSync(base + "/file1", "r");
  fs::closeSync(twice);
  EXPECT_EQ(errorOf([&] { fs::closeSync(twice); }), "Error|EBADF: bad file descriptor, close|EBADF|-9|close|-|-|" + std::to_string(twice));
  EXPECT_EQ(fs::constants::O_RDONLY_, 0);
#if defined(__APPLE__)
  EXPECT_EQ(fs::constants::O_NOFOLLOW_, 256);
#elif defined(__linux__) && defined(__x86_64__)
  EXPECT_EQ(fs::constants::O_NOFOLLOW_, 131072);
#else
  EXPECT_EQ(fs::constants::O_NOFOLLOW_, 32768);
#endif
  EXPECT_EQ(fs::constants::COPYFILE_EXCL, 1);
  EXPECT_EQ(fs::constants::O_CREAT_, DARWIN_EXPECTATION(64, 512));
  EXPECT_EQ(fs::constants::O_EXCL_, DARWIN_EXPECTATION(128, 2048));
  EXPECT_EQ(fs::constants::O_WRONLY_, 1);
  EXPECT_EQ(fs::constants::O_TRUNC_, DARWIN_EXPECTATION(512, 1024));
  EXPECT_EQ(fs::constants::O_APPEND_, DARWIN_EXPECTATION(1024, 8));
}

TEST("mkdirSync, mkdtempSync and rmSync results and errors (fs1/fs2 probes)") {
  TempDir dir;
  const std::string base = dir.path;
  Umask mask(022);
  EXPECT_EQ(errorOf([&] { fs::mkdirSync(base); }), errno_("EEXIST", -17, "file already exists", "mkdir", base));
  EXPECT_EQ(fs::mkdirSync(base, {.recursive = true}), std::nullopt);
  EXPECT_EQ(fs::mkdirSync(base + "/n1/n2", {.recursive = true}), std::optional<std::string>(base + "/n1"));
  fs::writeFileSync(base + "/file1", "x");
  EXPECT_EQ(errorOf([&] { fs::mkdirSync(base + "/file1/sub", {.recursive = true}); }), errno_("ENOTDIR", -20, "not a directory", "mkdir", base + "/file1/sub"));
  EXPECT_EQ(errorOf([&] { fs::mkdirSync(base + "/nope/sub"); }), errno_("ENOENT", -2, "no such file or directory", "mkdir", base + "/nope/sub"));
  EXPECT_EQ(errorOf([&] { fs::promises::mkdir(base + "/nope/sub"); }), errno_("ENOENT", -2, "no such file or directory", "mkdir", base + "/nope/sub"));
  {
    Cwd cwd(base);
    EXPECT_EQ(fs::mkdirSync("md3/x/y", {.recursive = true}), std::optional<std::string>("md3"));
    EXPECT_EQ(fs::mkdirSync(base + "/ab/c", {.recursive = true}), std::optional<std::string>(base + "/ab"));
    EXPECT_EQ(fs::mkdirSync(base + "/ab/c/d/e", {.recursive = true}), std::optional<std::string>(base + "/ab/c/d"));
    EXPECT_EQ(fs::mkdirSync("./md4/./z", {.recursive = true}), std::optional<std::string>("./md4"));
    EXPECT_EQ(fs::mkdirSync("md5/", {.recursive = true}), std::optional<std::string>("md5/"));
    EXPECT_EQ(fs::promises::mkdir(base + "/pm/q", {.recursive = true}), std::optional<std::string>(base + "/pm"));
    EXPECT_EQ(errorOf([&] { fs::mkdirSync("file1", {.recursive = true}); }), errno_("EEXIST", -17, "file already exists", "mkdir", "file1"));
    EXPECT_EQ(errorOf([&] { fs::mkdirSync("file1/a/b", {.recursive = true}); }), errno_("ENOTDIR", -20, "not a directory", "mkdir", "file1/a/b"));
    EXPECT_EQ(fs::readdirSync("md3", fs::withFileTypes).at(0).parentPath, "md3");
  }
  fs::mkdirSync(base + "/md1", {.recursive = true, .mode = 0777});
  EXPECT_EQ(modeOf(base + "/md1"), "755");
  fs::mkdirSync(base + "/md2/a/b", {.recursive = true, .mode = 0700});
  EXPECT_EQ(modeOf(base + "/md2"), "700");
  EXPECT_EQ(modeOf(base + "/md2/a/b"), "700");

  const std::string made = fs::mkdtempSync(base + "/tmp-");
  EXPECT_EQ(made.size(), (base + "/tmp-").size() + 6);
  EXPECT_EQ(made.substr(0, base.size() + 5), base + "/tmp-");
  for (char c : made.substr(base.size() + 5)) EXPECT(std::isalnum(static_cast<unsigned char>(c)));
  EXPECT_EQ(modeOf(made), "700");
  EXPECT(fs::mkdtempSync(base + "/tmp-") != made);
  EXPECT_EQ(fs::promises::mkdtemp(base + "/t-").size(), (base + "/t-").size() + 6);
  const std::string failed = errorOf([&] { fs::mkdtempSync(base + "/nodir/x-"); });
  const std::string prefix = "Error|ENOENT: no such file or directory, mkdtemp '" + base + "/nodir/x-";
  EXPECT_EQ(failed.substr(0, prefix.size()), prefix);
  EXPECT_EQ(failed.size(), prefix.size() + 6 + ("'|ENOENT|-2|mkdtemp|"s).size() + base.size() + 9 + 6 + 4);
  EXPECT_EQ(errorOf([&] { fs::promises::mkdtemp(base + "/nodir/x-"); }).substr(0, prefix.size()), prefix);

  EXPECT_EQ(errorOf([&] { fs::rmSync(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::promises::rm(base + "/missing"); }), errno_("ENOENT", -2, "no such file or directory", "lstat", base + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::rmSync(base + "/missing", {.force = true}); }), "no error");
  EXPECT_EQ(errorOf([&] { fs::rmSync(base + "/n1"); }),
            "SystemError|Path is a directory: rm returned EISDIR (is a directory) " + base + "/n1|ERR_FS_EISDIR|21|rm|" + base + "/n1|-|-");
  {
    Cwd cwd(base);
    fs::mkdirSync("tree/sub", {.recursive = true});
    fs::writeFileSync("tree/sub/f", "y");
    ASSERT(::symlink((base + "/ab").c_str(), "tree/link") == 0);
    EXPECT_EQ(errorOf([&] { fs::rmSync("tree", {.recursive = true}); }), "no error");
    EXPECT(!fs::existsSync("tree"));
    EXPECT(fs::existsSync(base + "/ab/c"));
    EXPECT_EQ(errorOf([&] { fs::rmSync("file1"); }), "no error");
    EXPECT(!fs::existsSync("file1"));
    EXPECT_EQ(errorOf([&] { fs::rmSync("nope", {.recursive = true}); }), errno_("ENOENT", -2, "no such file or directory", "lstat", "nope"));
    EXPECT_EQ(errorOf([&] { fs::rmSync("nope", {.recursive = true, .force = true}); }), "no error");
    fs::writeFileSync("file2", "z");
    EXPECT_EQ(errorOf([&] { fs::rmSync("file2/x"); }), errno_("EFAULT", -14, "bad address in system call argument", "rm", "file2/x"));
    EXPECT_EQ(errorOf([&] { fs::rmSync("file2/x", {.force = true}); }), errno_("EFAULT", -14, "bad address in system call argument", "rm", "file2/x"));
    EXPECT_EQ(errorOf([&] { fs::realpathSync("file2/x"); }), errno_("ENOTDIR", -20, "not a directory", "lstat", "file2/x"));
    EXPECT_EQ(errorOf([&] { fs::statSync("file2/x"); }), errno_("ENOTDIR", -20, "not a directory", "stat", "file2/x"));
    EXPECT_EQ(fs::realpathSync("file2"), fs::realpathSync(base) + "/file2");
    EXPECT_EQ(fs::realpathSync("."), fs::realpathSync(base));
    EXPECT_EQ(errorOf([&] { fs::promises::rm("md3", {.recursive = true, .force = true}); }), "no error");
  }
}

TEST("file contents, modes, links, copies and existsSync (fs2 probe)") {
  TempDir dir;
  Cwd cwd(dir.path);
  Umask mask(022);
  EXPECT(fs::existsSync(dir.path));
  EXPECT(!fs::existsSync("missing"));
  EXPECT(!fs::existsSync(""));
  ASSERT(::symlink((dir.path + "/missing").c_str(), "dangle") == 0);
  EXPECT(!fs::existsSync("dangle"));
  fs::writeFileSync("file2", "z");
  fs::linkSync("file2", "file2-hard");
  EXPECT_EQ(fs::lstatSync("file2").nlink, std::uint64_t(2));
  fs::chmodSync("file2", 0640);
  EXPECT_EQ(modeOf("file2"), "640");
  fs::writeFileSync("file2", "new", {.mode = 0600});
  EXPECT_EQ(modeOf("file2"), "640");
  fs::writeFileSync("file3", "new", {.mode = 0600});
  EXPECT_EQ(modeOf("file3"), "600");
  fs::writeFileSync("file4", "q", {.flag = "wx", .mode = 0600});
  EXPECT_EQ(modeOf("file4"), "600");
  fs::writeFileSync("w1.txt", "x", {.mode = 0666});
  EXPECT_EQ(modeOf("w1.txt"), "644");
  const fs::Bytes raw = {0, 255, 10};
  fs::writeFileSync("bytes", raw);
  EXPECT_EQ(fs::readFileSync("bytes"), raw);
  fs::writeFileSync("sur", "a\xED\xA0\x80" "b");
  EXPECT_EQ(fs::readFileSync("sur"), fs::Bytes({97, 239, 191, 189, 98}));
  fs::writeFileSync("pair", "\xED\xA0\xBD\xED\xB8\x80");
  EXPECT_EQ(fs::readFileSync("pair", "utf8"), "\xF0\x9F\x98\x80");
  fs::promises::writeFile("file5", "five");
  EXPECT_EQ(fs::promises::readFile("file5", "utf8"), "five");
  EXPECT_EQ(fs::promises::readFile("file5"), fs::Bytes({102, 105, 118, 101}));
  EXPECT_EQ(fs::readFileSync("file5", "utf-8"), "five");
  const int fd = fs::openSync("file5", fs::constants::O_RDONLY_ | fs::constants::O_NOFOLLOW_);
  const auto info = fs::fstatSync(fd);
  EXPECT_EQ(info.size, std::int64_t(4));
  EXPECT(info.isFile());
  fs::fsyncSync(fd);
  EXPECT_EQ(fs::readFileSync(fd, "utf8"), "five");
  fs::closeSync(fd);
  fs::writeFileSync("bom.txt", bytes("\xEF\xBB\xBF" "A\xFF\xC3" "B\xED\xA0\x80" "C"));
  EXPECT_EQ(fs::readFileSync("bom.txt", "utf8"), "\xEF\xBB\xBF" "A\xEF\xBF\xBD\xEF\xBF\xBD" "B\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "C");
  fs::chmodSync("bom.txt", 0644);
  fs::copyFileSync("bom.txt", "bomcopy.txt", fs::constants::COPYFILE_EXCL);
  EXPECT_EQ(modeOf("bomcopy.txt"), "644");
  fs::copyFileSync("file5", "copy5", fs::constants::COPYFILE_EXCL);
  EXPECT_EQ(fs::readFileSync("copy5", "utf8"), "five");
  fs::copyFileSync("file2", "copy5");
  EXPECT_EQ(fs::readFileSync("copy5", "utf8"), "new");
  EXPECT_EQ(errorOf([&] { fs::copyFileSync(".", "copy6"); }), errno_("ENOTSUP", DARWIN_EXPECTATION(-95, -45), "operation not supported on socket", "copyfile", ".", "copy6"));
  EXPECT(!fs::existsSync("copy6"));
  const auto entries = fs::promises::readdir(".", fs::withFileTypes);
  for (const auto& entry : entries) {
    EXPECT_EQ(entry.parentPath, ".");
    EXPECT_EQ(entry.path(), ".");
    EXPECT(entry.name != "." && entry.name != "..");
    EXPECT_EQ(entry.isSymbolicLink(), entry.name == "dangle");
    EXPECT_EQ(entry.isFile(), entry.name != "dangle");
    EXPECT(!entry.isDirectory());
  }
  EXPECT_EQ(fs::readdirSync(".").size(), entries.size());
  EXPECT_EQ(entries.size(), std::size_t(13));
}

TEST("UTF-8 text I/O keeps BOM and NUL bytes and consumes from the descriptor offset") {
  TempDir dir;
  const std::string filePath = dir.path + "/codec";
  const auto raw = "\xEF\xBB\xBF" "A\0\xE2\x82" "B\xED\xA0\x80" "C\xF0\x9F\x98"sv;
  const auto decoded = "\xEF\xBB\xBF" "A\0\xEF\xBF\xBD" "B\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD" "C\xEF\xBF\xBD"sv;
  fs::writeFileSync(filePath, bytes(raw));
  EXPECT_EQ(fs::readFileSync(filePath, "utf8"), decoded);
  auto handle = fs::promises::open(filePath);
  EXPECT_EQ(fs::readFileSync(handle.fd, "UTF-8"), decoded);
  EXPECT_EQ(fs::readFileSync(handle.fd, "utf-8"), "");
  ASSERT_EQ(::lseek(handle.fd, 3, SEEK_SET), off_t{3});
  EXPECT_EQ(fs::readFileSync(handle.fd, "UTF8"), decoded.substr(3));
  EXPECT_EQ(::lseek(handle.fd, 0, SEEK_CUR), static_cast<off_t>(raw.size()));

  const auto text = "\xEF\xBB\xBF" "A\0" "\xED\xA0\xBD\xED\xB8\x80\xED\xA0\x80" "B\xED\xB0\x80"sv;
  const auto encoded = "\xEF\xBB\xBF" "A\0" "\xF0\x9F\x98\x80\xEF\xBF\xBD" "B\xEF\xBF\xBD"sv;
  fs::writeFileSync(filePath, text);
  EXPECT_EQ(fs::readFileSync(filePath), fs::Bytes(bytes(encoded).begin(), bytes(encoded).end()));
}

TEST("readdir keeps the directory's own order (no sorting)") {
  TempDir dir;
  for (const char* name : {"zeta", "alpha", "mid", "10", "2", "Beta"}) fs::writeFileSync(dir.path + "/" + name, "");
  fs::mkdirSync(dir.path + "/dirx");
  ASSERT(::symlink("zeta", (dir.path + "/lnk").c_str()) == 0);
  std::vector<std::string> expected;
  {
    std::unique_ptr<DIR, int (*)(DIR*)> handle(::opendir(dir.path.c_str()), &::closedir);
    while (const dirent* entry = ::readdir(handle.get()))
      if (std::string_view(entry->d_name) != "." && std::string_view(entry->d_name) != "..") expected.emplace_back(entry->d_name);
  }
  EXPECT_EQ(fs::readdirSync(dir.path), expected);
  for (const auto& entry : fs::readdirSync(dir.path, fs::withFileTypes)) {
    if (entry.name == "lnk") EXPECT(entry.isSymbolicLink());
    else if (entry.name == "dirx") EXPECT(entry.isDirectory());
    else EXPECT(entry.isFile());
  }
}

TEST("FileHandle read, stat and close (fs2 probe)") {
  TempDir dir;
  fs::writeFileSync(dir.path + "/file5", "five");
  auto handle = fs::promises::open(dir.path + "/file5", "r");
  EXPECT_EQ(handle.stat().size, std::int64_t(4));
  std::vector<std::uint8_t> buffer(8);
  EXPECT_EQ(handle.read(buffer, 2, 3, 1).bytesRead, std::size_t(3));
  EXPECT_EQ(buffer, std::vector<std::uint8_t>({0, 0, 105, 118, 101, 0, 0, 0}));
  std::vector<std::uint8_t> second(8);
  EXPECT_EQ(handle.read(second, 0, 8, 2).bytesRead, std::size_t(2));
  EXPECT_EQ(second, std::vector<std::uint8_t>({118, 101, 0, 0, 0, 0, 0, 0}));
  EXPECT_EQ(handle.read(second, 0, 8, 100).bytesRead, std::size_t(0));
  handle.close();
  EXPECT_EQ(errorOf([&] { handle.close(); }), "no error");
  EXPECT_EQ(errorOf([&] { handle.read(second, 0, 1, 0); }), "SystemError|Bad file descriptor|EBADF|-|read|-|-|-");
  EXPECT_EQ(errorOf([&] { handle.stat(); }), "SystemError|Bad file descriptor|EBADF|-|fstat|-|-|-");
  auto moved = fs::promises::open(dir.path + "/file5");
  fs::FileHandle target = std::move(moved);
  EXPECT_EQ(moved.fd, -1);
  EXPECT(target.fd >= 0);
}

TEST("Stats fields: Stats numbers and BigIntStats integers (fs2 probe)") {
  TempDir dir;
  const std::string file = dir.path + "/file5";
  fs::writeFileSync(file, "five");
  const auto stat = fs::lstatSync(file);
  struct stat raw;
  ASSERT(::lstat(file.c_str(), &raw) == 0);
  EXPECT_EQ(stat.size, std::int64_t(4));
  EXPECT_EQ(stat.dev, static_cast<std::uint64_t>(raw.st_dev));
  EXPECT_EQ(stat.ino, static_cast<std::uint64_t>(raw.st_ino));
  EXPECT_EQ(stat.uid, static_cast<std::uint32_t>(::getuid()));
  EXPECT_EQ(stat.nlink, std::uint64_t(1));
  EXPECT_EQ(stat.blksize, std::int64_t(raw.st_blksize));
  EXPECT_EQ(stat.blocks, std::int64_t(raw.st_blocks));
  EXPECT_EQ(stat.rdev, std::uint64_t(0));
  EXPECT_EQ(stat.mtimeNs, std::int64_t(modificationTime(raw).tv_sec) * 1000000000 + modificationTime(raw).tv_nsec);
  EXPECT_EQ(stat.mtimeMs, static_cast<double>(modificationTime(raw).tv_sec) * 1e3 + static_cast<double>(modificationTime(raw).tv_nsec) / 1e6);
  EXPECT_EQ(std::floor(stat.mtimeMs), static_cast<double>(stat.mtimeNs / 1000000));
  EXPECT_EQ(stat.ctimeNs, std::int64_t(changeTime(raw).tv_sec) * 1000000000 + changeTime(raw).tv_nsec);
  EXPECT(stat.isFile() && !stat.isDirectory() && !stat.isSymbolicLink() && !stat.isFIFO() && !stat.isSocket() && !stat.isBlockDevice() && !stat.isCharacterDevice());
  EXPECT(fs::statSync(dir.path).isDirectory());
  ASSERT(::symlink(file.c_str(), (dir.path + "/link").c_str()) == 0);
  EXPECT(fs::lstatSync(dir.path + "/link").isSymbolicLink());
  EXPECT(fs::statSync(dir.path + "/link").isFile());
  EXPECT_EQ(fs::promises::stat(file).size, std::int64_t(4));
  EXPECT_EQ(fs::promises::realpath(file), fs::realpathSync(file));
  EXPECT_EQ((fs::statSync(dir.path).mode & 0777), std::uint32_t(0700));
}

namespace {
  // "<atime ns> <mtime ns>" as BigIntStats reports them.
  std::string timesOf(const std::string& path) {
    const auto stat = fs::statSync(path);
    return std::to_string(stat.atimeNs) + " " + std::to_string(stat.mtimeNs);
  }

  // The raw seconds and nanoseconds of both times (values past what Stats' integers hold).
  std::string rawTimesOf(const std::string& path) {
    struct stat raw;
    if (::stat(path.c_str(), &raw) != 0) return "stat failed";
    return std::to_string(accessTime(raw).tv_sec) + "." + std::to_string(accessTime(raw).tv_nsec) + " " + std::to_string(modificationTime(raw).tv_sec) + "." +
           std::to_string(modificationTime(raw).tv_nsec);
  }

  std::string typeErrorCodeOf(const std::function<void()>& body) {
    try {
      body();
    } catch (const rt::TypeError& error) {
      return error.code.value_or("-");
    }
    return "no error";
  }
}

TEST("symlinkSync and renameSync results and errors (bun 1.4 probe)") {
  TempDir dir;
  const std::string base = dir.path;
  fs::writeFileSync(base + "/file", "x");
  fs::mkdirSync(base + "/dir");
  EXPECT_EQ(errorOf([&] { fs::symlinkSync("file", base + "/link"); }), "no error");
  EXPECT(fs::lstatSync(base + "/link").isSymbolicLink());
  char target[16] = {};
  ASSERT(::readlink((base + "/link").c_str(), target, sizeof target - 1) == 4);
  EXPECT_EQ(std::string(target), "file");
  EXPECT_EQ(fs::readFileSync(base + "/link", "utf8"), "x");
  EXPECT_EQ(errorOf([&] { fs::symlinkSync("file", base + "/link"); }), errno_("EEXIST", -17, "file already exists", "symlink", "file", base + "/link"));
  EXPECT_EQ(errorOf([&] { fs::symlinkSync(base + "/missing", base + "/dangling"); }), "no error");
  EXPECT(fs::lstatSync(base + "/dangling").isSymbolicLink());
  EXPECT(!fs::existsSync(base + "/dangling"));
  EXPECT_EQ(errorOf([&] { fs::symlinkSync("file", base + "/nope/link"); }),
            errno_("ENOENT", -2, "no such file or directory", "symlink", "file", base + "/nope/link"));
  EXPECT_EQ(errorOf([&] { fs::symlinkSync("", base + "/empty"); }),
            DARWIN_EXPECTATION("Error|ENOENT: no such file or directory, symlink|ENOENT|-2|symlink|-|" + base + "/empty|-", "no error"));
  EXPECT_EQ(errorOf([&] { fs::symlinkSync("file", ""); }), errno_("ENOENT", -2, "no such file or directory", "symlink", "file"));
  {
    Cwd cwd(base);
    EXPECT_EQ(errorOf([&] { fs::symlinkSync("x", "relative-missing/a"); }),
              errno_("ENOENT", -2, "no such file or directory", "symlink", "x", "relative-missing/a"));
  }

  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/file", base + "/file2"); }), "no error");
  EXPECT(!fs::existsSync(base + "/file"));
  EXPECT_EQ(fs::readFileSync(base + "/file2", "utf8"), "x");
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/missing", base + "/other"); }),
            errno_("ENOENT", -2, "no such file or directory", "rename", base + "/missing", base + "/other"));
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/file2", base + "/nope/file"); }),
            errno_("ENOENT", -2, "no such file or directory", "rename", base + "/file2", base + "/nope/file"));
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/file2", base + "/dir"); }),
            errno_("EISDIR", -21, "illegal operation on a directory", "rename", base + "/file2", base + "/dir"));
  fs::writeFileSync(base + "/dir/inner", "y");
  fs::mkdirSync(base + "/dir2");
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/dir2", base + "/dir"); }),
            errno_("ENOTEMPTY", DARWIN_EXPECTATION(-39, -66), "directory not empty", "rename", base + "/dir2", base + "/dir"));
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/dir2", base + "/file2"); }),
            errno_("ENOTDIR", -20, "not a directory", "rename", base + "/dir2", base + "/file2"));
  fs::writeFileSync(base + "/a", "A");
  fs::writeFileSync(base + "/b", "B");
  fs::renameSync(base + "/a", base + "/b");
  EXPECT_EQ(fs::readFileSync(base + "/b", "utf8"), "A");
  fs::renameSync(base + "/link", base + "/link2");
  EXPECT(fs::lstatSync(base + "/link2").isSymbolicLink());
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/dir", base + "/dir/sub"); }),
            errno_("EINVAL", -22, "invalid argument", "rename", base + "/dir", base + "/dir/sub"));
  EXPECT_EQ(errorOf([&] { fs::renameSync("", base + "/x"); }), "Error|ENOENT: no such file or directory, rename|ENOENT|-2|rename|-|" + base + "/x|-");
  EXPECT_EQ(errorOf([&] { fs::renameSync(base + "/file2", ""); }), errno_("ENOENT", -2, "no such file or directory", "rename", base + "/file2"));
}

TEST("utimesSync sets both times as Bun does: seconds kept to the nanosecond, Dates, negative numbers as now (bun 1.4 probe)") {
  TempDir dir;
  const std::string file = dir.path + "/file";
  fs::writeFileSync(file, "x");
  fs::utimesSync(file, 1, 2);
  EXPECT_EQ(timesOf(file), "1000000000 2000000000");
  fs::utimesSync(file, 1.5, 2.25);
  EXPECT_EQ(timesOf(file), "1500000000 2250000000");
  fs::utimesSync(file, 1.0000000001, 2.123456789);
  EXPECT_EQ(timesOf(file), "1000000000 2123456789");
  fs::utimesSync(file, 0, 0);
  EXPECT_EQ(timesOf(file), "0 0");
  fs::utimesSync(file, 1e9 + 0.5, 1e9 + 0.999999999);
  EXPECT_EQ(timesOf(file), "1000000000500000000 1000000001000000000");
  fs::utimesSync(file, 1700000000.123, 1700000000.987654321);
  EXPECT_EQ(timesOf(file), "1700000000122999906 1700000000987654209");
  fs::utimesSync(file, -0.0, -0.0);
  EXPECT_EQ(timesOf(file), "0 0");
  fs::utimesSync(file, 5e-324, 0.9999999999);
  EXPECT_EQ(timesOf(file), "0 999999999");
  fs::utimesSync(file, rt::Date(1234567), rt::Date(-1500));
  EXPECT_EQ(timesOf(file), "1234567000000 -1500000000");
  fs::utimesSync(file, rt::Date(1.9), rt::Date(-1.9));
  EXPECT_EQ(timesOf(file), "1000000 -1000000");
  fs::utimesSync(file, rt::Date(8.64e15), rt::Date(-8.64e15));
  EXPECT_EQ(rawTimesOf(file), DARWIN_EXPECTATION("8640000000000.0 -8640000000000.0", "9223372036.854775807 -9223372037.145224192"));
#if defined(__APPLE__)
  EXPECT_EQ(fs::statSync(file).atimeNs, std::numeric_limits<std::int64_t>::max());
  EXPECT_EQ(fs::statSync(file).mtimeNs, std::numeric_limits<std::int64_t>::min());
#endif
  fs::utimesSync(file, 9.2e18, 1);
  EXPECT_EQ(rawTimesOf(file), DARWIN_EXPECTATION("9200000000000000000.0 1.0", "9223372036.854775807 1.0"));
  // Seconds past int64 saturate (Bun: 1e19 and 1e300 both store 9223372036854775807 s).
  fs::utimesSync(file, 1e19, 1e300);
  EXPECT_EQ(rawTimesOf(file), DARWIN_EXPECTATION("9223372036854775807.0 9223372036854775807.0", "9223372036.854775807 9223372036.854775807"));

  // A negative number is now, for that time alone; two of them share one kernel timestamp.
  const auto before = static_cast<long long>(std::time(nullptr));
  fs::utimesSync(file, -1, 5);
  struct stat raw;
  ASSERT(::stat(file.c_str(), &raw) == 0);
  EXPECT(accessTime(raw).tv_sec >= before - 1 && accessTime(raw).tv_sec <= static_cast<long long>(std::time(nullptr)) + 1);
  EXPECT_EQ(static_cast<long long>(modificationTime(raw).tv_sec), 5LL);
  EXPECT_EQ(static_cast<long long>(modificationTime(raw).tv_nsec), 0LL);
  fs::utimesSync(file, -1, -2.5);
  ASSERT(::stat(file.c_str(), &raw) == 0);
  EXPECT(accessTime(raw).tv_sec == modificationTime(raw).tv_sec && accessTime(raw).tv_nsec == modificationTime(raw).tv_nsec);
  EXPECT(accessTime(raw).tv_sec >= before - 1);

  // Validation comes first (atime, then mtime), then the call follows symlinks.
  const double nan = std::nan("");
  const double inf = std::numeric_limits<double>::infinity();
  EXPECT_EQ(errorOf([&] { fs::utimesSync(file, nan, 1); }), "TypeError|atime must be a number or a Date");
  EXPECT_EQ(errorOf([&] { fs::utimesSync(file, nan, nan); }), "TypeError|atime must be a number or a Date");
  EXPECT_EQ(errorOf([&] { fs::utimesSync(file, 1, inf); }), "TypeError|mtime must be a number or a Date");
  EXPECT_EQ(errorOf([&] { fs::utimesSync(file, rt::Date(nan), 1); }), "TypeError|atime must be a number or a Date");
  EXPECT_EQ(typeErrorCodeOf([&] { fs::utimesSync(file, -inf, 1); }), "ERR_INVALID_ARG_TYPE");
  EXPECT_EQ(errorOf([&] { fs::utimesSync(dir.path + "/missing", nan, 1); }), "TypeError|atime must be a number or a Date");
  EXPECT_EQ(errorOf([&] { fs::utimesSync(dir.path + "/missing", 1, 1); }), errno_("ENOENT", -2, "no such file or directory", "utime", dir.path + "/missing"));
  EXPECT_EQ(errorOf([&] { fs::utimesSync("", 1, 1); }), errno_("ENOENT", -2, "no such file or directory", "utime"));
  ASSERT(::symlink(file.c_str(), (dir.path + "/link").c_str()) == 0);
  fs::utimesSync(dir.path + "/link", 7, 8);
  EXPECT_EQ(timesOf(file), "7000000000 8000000000");
  EXPECT(fs::lstatSync(dir.path + "/link").mtimeNs != 8000000000);
  ASSERT(::symlink((dir.path + "/missing").c_str(), (dir.path + "/dangling").c_str()) == 0);
  EXPECT_EQ(errorOf([&] { fs::utimesSync(dir.path + "/dangling", 1, 1); }),
            errno_("ENOENT", -2, "no such file or directory", "utime", dir.path + "/dangling"));
}
