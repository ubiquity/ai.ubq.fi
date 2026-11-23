#pragma once
// node:fs, node:fs/promises and node:path (POSIX) as the gateway uses them, with
// Bun 1.4's results and error objects. Paths are byte strings used as given:
// relative paths resolve against the current directory and appear in error
// messages exactly as passed. node:url fileURLToPath/pathToFileURL live in
// uos/rt/url.hpp. The node:fs/promises functions are synchronous cores (see
// docs/rt/platform.md): the TS `await` stays a suspension point only where the
// porter needs it.
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include <fcntl.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace uos::rt::fs {
  using Bytes = std::vector<std::uint8_t>;

  // A failed system call, as Bun reports it: an `Error` whose message is
  // "<code>: <description>, <syscall>[ '<path>'[ -> '<dest>']]", with own
  // properties code, errno (negative), syscall, path, dest and (closeSync) fd.
  // TS branches on `error.code` ("ENOENT", "EEXIST"). A few errors Bun creates
  // as `SystemError` (name "SystemError") carry the same fields; see each call.
  // An empty `syscall` is an absent one (Bun.spawn's "Executable not found").
  struct ErrnoError : JsError {
    std::string code;
    std::optional<int> errno_;
    std::string syscall;
    std::optional<std::string> path;
    std::optional<std::string> dest;
    std::optional<int> fd;
    ErrnoError(std::string message_, std::string code_, std::optional<int> errnoValue, std::string syscall_, std::optional<std::string> path_ = std::nullopt,
               std::optional<std::string> dest_ = std::nullopt, std::string name_ = "Error");
  };

  // The error Bun throws when `syscall` fails with the (positive) C errno value
  // `error`: libuv's code and description, path and dest quoted when present.
  ErrnoError errnoError(int error, std::string_view syscall, std::optional<std::string_view> path = std::nullopt,
                        std::optional<std::string_view> dest = std::nullopt);
  // libuv's name ("ENOENT") and description ("no such file or directory") of a
  // positive errno; "UNKNOWN" / "unknown error" outside Bun's table.
  std::string_view errnoCode(int error) noexcept;
  std::string_view errnoDescription(int error) noexcept;

  // `fs.constants` (the host's values; Linux arm64 here). O_* names carry a
  // trailing underscore because <fcntl.h> defines them as macros.
  namespace constants {
    inline constexpr int O_RDONLY_ = O_RDONLY;
    inline constexpr int O_WRONLY_ = O_WRONLY;
    inline constexpr int O_RDWR_ = O_RDWR;
    inline constexpr int O_CREAT_ = O_CREAT;
    inline constexpr int O_EXCL_ = O_EXCL;
    inline constexpr int O_TRUNC_ = O_TRUNC;
    inline constexpr int O_APPEND_ = O_APPEND;
    inline constexpr int O_SYNC_ = O_SYNC;
    inline constexpr int O_NOFOLLOW_ = O_NOFOLLOW;
    inline constexpr int COPYFILE_EXCL = 1;
  }

  // fs.Stats and fs.BigIntStats in one: the integer members are the exact
  // (BigIntStats) values; `atimeMs`/`mtimeMs`/`ctimeMs` are the Stats numbers
  // (seconds * 1e3 + nanoseconds / 1e6); `...Ns` are BigIntStats' nanoseconds
  // (BigIntStats' `mtimeMs` is mtimeNs / 1000000). JS `stat.size` etc. are these
  // integers converted to double; `String(bigintStat.dev)` is std::to_string.
  struct Stats {
    std::uint64_t dev = 0;
    std::uint64_t ino = 0;
    std::uint32_t mode = 0;
    std::uint64_t nlink = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
    std::uint64_t rdev = 0;
    std::int64_t size = 0;
    std::int64_t blksize = 0;
    std::int64_t blocks = 0;
    double atimeMs = 0;
    double mtimeMs = 0;
    double ctimeMs = 0;
    std::int64_t atimeNs = 0;
    std::int64_t mtimeNs = 0;
    std::int64_t ctimeNs = 0;
    bool isFile() const noexcept;
    bool isDirectory() const noexcept;
    bool isSymbolicLink() const noexcept;
    bool isFIFO() const noexcept;
    bool isSocket() const noexcept;
    bool isBlockDevice() const noexcept;
    bool isCharacterDevice() const noexcept;
  };

  // fs.Dirent from readdir(path, { withFileTypes: true }): `parentPath` (and
  // the deprecated `path`) is the directory argument as given.
  struct Dirent {
    std::string name;
    std::string parentPath;
    // d_type of the entry (DT_REG, DT_DIR, DT_LNK, ...).
    unsigned char type = 0;
    const std::string& path() const noexcept { return parentPath; }
    bool isFile() const noexcept;
    bool isDirectory() const noexcept;
    bool isSymbolicLink() const noexcept;
  };
  // The `{ withFileTypes: true }` option: readdirSync(path, rt::fs::withFileTypes).
  struct WithFileTypes {};
  inline constexpr WithFileTypes withFileTypes{};

  struct WriteFileOptions {
    // "w", "wx", "a", ... (the openSync flags strings).
    std::string flag = "w";
    // Applied only when the file is created, under the process umask.
    std::uint32_t mode = 0666;
  };
  struct MkdirOptions {
    bool recursive = false;
    std::uint32_t mode = 0777;
  };
  struct RmOptions {
    bool recursive = false;
    bool force = false;
  };

  // readFileSync(path) (a Buffer) and readFileSync(path, "utf8" | "utf-8"):
  // UTF-8 decoding keeps a BOM and replaces each ill-formed subsequence with
  // U+FFFD (WHATWG). Other encodings throw std::invalid_argument (unused by the TS).
  Bytes readFileSync(std::string_view path);
  std::string readFileSync(std::string_view path, std::string_view encoding);
  Bytes readFileSync(int fd);
  std::string readFileSync(int fd, std::string_view encoding);
  // Text writes combine surrogate pairs and replace lone surrogates with U+FFFD.
  void writeFileSync(std::string_view path, std::string_view data, const WriteFileOptions& options = {});
  void writeFileSync(std::string_view path, std::span<const std::uint8_t> data, const WriteFileOptions& options = {});
  // openSync(path, flags = "r", mode = 0o666). String flags follow Node
  // ("r", "r+", "w", "wx", "a", ...); others throw TypeError
  // "The argument 'flags' is invalid. Received '<flags>'" (code ERR_INVALID_ARG_VALUE).
  int openSync(std::string_view path, std::string_view flags = "r", std::uint32_t mode = 0666);
  int openSync(std::string_view path, int flags, std::uint32_t mode = 0666);
  // closeSync(fd): EBADF carries `fd`.
  void closeSync(int fd);
  Stats fstatSync(int fd);
  void fsyncSync(int fd);
  Stats statSync(std::string_view path);
  Stats lstatSync(std::string_view path);
  // realpathSync(path): failures report syscall "lstat", as Bun does.
  std::string realpathSync(std::string_view path);
  // existsSync(path): follows symlinks (a dangling link is false); "" is false.
  bool existsSync(std::string_view path);
  // mkdirSync(path, { recursive, mode }): undefined (std::nullopt), or with
  // `recursive` the first directory created, spelled as a prefix of `path`
  // (the whole of `path` when only its last component was missing).
  std::optional<std::string> mkdirSync(std::string_view path, const MkdirOptions& options = {});
  // mkdtempSync(prefix): prefix + 6 random [A-Za-z0-9] characters, mode 0o700.
  std::string mkdtempSync(std::string_view prefix);
  // rmSync(path, { recursive, force }): a missing path is ENOENT "lstat" unless
  // `force`; a directory without `recursive` is SystemError ERR_FS_EISDIR
  // "Path is a directory: rm returned EISDIR (is a directory) <path>" (errno 21,
  // syscall "rm"); a path under a non-directory is EFAULT "rm" even with `force`.
  // Recursion never follows symlinks.
  void rmSync(std::string_view path, const RmOptions& options = {});
  void chmodSync(std::string_view path, std::uint32_t mode);
  // copyFileSync(src, dest, mode): COPYFILE_EXCL fails with EEXIST when dest
  // exists; the copy gets the source's permission bits. A directory source is
  // ENOTSUP. Errors name both paths: "copyfile '<src>' -> '<dest>'".
  void copyFileSync(std::string_view src, std::string_view dest, int mode = 0);
  void linkSync(std::string_view existingPath, std::string_view newPath);
  // symlinkSync(target, path): `target` is stored as given (a relative target resolves
  // against the link's directory; it may dangle). Errors read "symlink '<target>' ->
  // '<path>'" with path = target and dest = path (an empty path is left out). The `type`
  // argument only matters on Windows and is not provided.
  void symlinkSync(std::string_view target, std::string_view path);
  // renameSync(oldPath, newPath): rename(2), replacing a file or an empty directory at
  // newPath. Errors read "rename '<oldPath>' -> '<newPath>'" (an empty newPath is left out).
  void renameSync(std::string_view oldPath, std::string_view newPath);
  // A utimesSync time: a JS number (seconds since the epoch) or a Date.
  class TimeLike {
  public:
    template <class T>
      requires(std::is_arithmetic_v<T> && !std::is_same_v<T, bool>)
    TimeLike(T seconds) : value_(static_cast<double>(seconds)) {}
    TimeLike(const Date& date) : value_(date.getTime()), date_(true) {}
    // Seconds for a number, the time value (milliseconds) for a Date.
    double value() const noexcept { return value_; }
    bool isDate() const noexcept { return date_; }

  private:
    double value_;
    bool date_ = false;
  };
  // utimesSync(path, atime, mtime), following symlinks, as Bun sets the times: a number keeps
  // its fraction to the nanosecond (truncated) and a negative number means now; a Date may be
  // negative. NaN, infinities and an Invalid Date throw TypeError "atime must be a number or a
  // Date" (code ERR_INVALID_ARG_TYPE; atime is checked first, before the path); a failed
  // call throws ErrnoError "utime '<path>'".
  void utimesSync(std::string_view path, TimeLike atime, TimeLike mtime);
  // readdirSync(path) in directory order (no sorting), without "." and "..".
  std::vector<std::string> readdirSync(std::string_view path);
  std::vector<Dirent> readdirSync(std::string_view path, WithFileTypes);

  // A node:fs/promises FileHandle (from promises::open). Moving transfers the
  // descriptor; the destructor closes a handle still open.
  class FileHandle {
  public:
    explicit FileHandle(int fd_) noexcept : fd(fd_) {}
    FileHandle(FileHandle&& other) noexcept;
    FileHandle& operator=(FileHandle&& other) noexcept;
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
    ~FileHandle();
    // `handle.fd`; -1 once closed.
    int fd;
    // `handle.stat()`
    Stats stat() const;
    // `handle.read(buffer, offset, length, position)` -> `{ bytesRead }`: one
    // read at `position` (std::nullopt: the current offset). After close():
    // SystemError "Bad file descriptor" (code EBADF, syscall "read"/"fstat").
    struct ReadResult {
      std::size_t bytesRead = 0;
    };
    ReadResult read(std::span<std::uint8_t> buffer, std::size_t offset, std::size_t length, std::optional<std::int64_t> position) const;
    // `handle.close()`; closing twice is a no-op.
    void close();
  };

  // node:fs/promises, as synchronous cores with the same results and errors as
  // the *Sync functions. `readFile(url)` is readFile(rt::fileURLToPath(url)).
  namespace promises {
    Bytes readFile(std::string_view path);
    std::string readFile(std::string_view path, std::string_view encoding);
    void writeFile(std::string_view path, std::string_view data, const WriteFileOptions& options = {});
    void writeFile(std::string_view path, std::span<const std::uint8_t> data, const WriteFileOptions& options = {});
    Stats stat(std::string_view path);
    Stats lstat(std::string_view path);
    std::string realpath(std::string_view path);
    std::vector<std::string> readdir(std::string_view path);
    std::vector<Dirent> readdir(std::string_view path, WithFileTypes);
    std::optional<std::string> mkdir(std::string_view path, const MkdirOptions& options = {});
    std::string mkdtemp(std::string_view prefix);
    void rm(std::string_view path, const RmOptions& options = {});
    FileHandle open(std::string_view path, std::string_view flags = "r", std::uint32_t mode = 0666);
  }
}

// node:path (path.posix).
namespace uos::rt::path {
  inline constexpr std::string_view sep = "/";
  inline constexpr std::string_view delimiter = ":";

  bool isAbsolute(std::string_view path);
  std::string dirname(std::string_view path);
  // path.relative(from, to): both resolved against the current directory first.
  std::string relative(std::string_view from, std::string_view to);

  // Join preserves the first segment's root. Resolve restarts at an absolute segment.
  std::string join(std::span<const std::string_view> parts);
  std::string resolve(std::span<const std::string_view> parts);
  template <class... Parts>
    requires(std::is_convertible_v<const Parts&, std::string_view> && ...)
  std::string join(const Parts&... parts) {
    return join(std::array<std::string_view, sizeof...(Parts)>{std::string_view(parts)...});
  }
  template <class... Parts>
    requires(std::is_convertible_v<const Parts&, std::string_view> && ...)
  std::string resolve(const Parts&... parts) {
    return resolve(std::array<std::string_view, sizeof...(Parts)>{std::string_view(parts)...});
  }
}
