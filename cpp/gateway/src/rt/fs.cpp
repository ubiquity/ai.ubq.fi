#include "uos/rt/fs.hpp"

#include <dirent.h>
#if defined(__linux__)
#include <sys/random.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <stdexcept>
#include <utility>

namespace uos::rt::fs {
  namespace {
    // Bun's libuv names/descriptions paired with the target's native errno values.
    struct ErrnoEntry {
      int value;
      const char* code;
      const char* description;
    };
    constexpr ErrnoEntry ERRNO_TABLE[] = {
      {EPERM, "EPERM", "operation not permitted"},
      {ENOENT, "ENOENT", "no such file or directory"},
      {ESRCH, "ESRCH", "no such process"},
      {EINTR, "EINTR", "interrupted system call"},
      {EIO, "EIO", "i/o error"},
      {ENXIO, "ENXIO", "no such device or address"},
      {E2BIG, "E2BIG", "argument list too long"},
      {ENOEXEC, "ENOEXEC", "exec format error"},
      {EBADF, "EBADF", "bad file descriptor"},
      {EAGAIN, "EAGAIN", "resource temporarily unavailable"},
      {ENOMEM, "ENOMEM", "not enough memory"},
      {EACCES, "EACCES", "permission denied"},
      {EFAULT, "EFAULT", "bad address in system call argument"},
      {EBUSY, "EBUSY", "resource busy or locked"},
      {EEXIST, "EEXIST", "file already exists"},
      {EXDEV, "EXDEV", "cross-device link not permitted"},
      {ENODEV, "ENODEV", "no such device"},
      {ENOTDIR, "ENOTDIR", "not a directory"},
      {EISDIR, "EISDIR", "illegal operation on a directory"},
      {EINVAL, "EINVAL", "invalid argument"},
      {ENFILE, "ENFILE", "file table overflow"},
      {EMFILE, "EMFILE", "too many open files"},
      {ENOTTY, "ENOTTY", "inappropriate ioctl for device"},
      {ETXTBSY, "ETXTBSY", "text file is busy"},
      {EFBIG, "EFBIG", "file too large"},
      {ENOSPC, "ENOSPC", "no space left on device"},
      {ESPIPE, "ESPIPE", "invalid seek"},
      {EROFS, "EROFS", "read-only file system"},
      {EMLINK, "EMLINK", "too many links"},
      {EPIPE, "EPIPE", "broken pipe"},
      {ERANGE, "ERANGE", "result too large"},
      {ENAMETOOLONG, "ENAMETOOLONG", "name too long"},
      {ENOSYS, "ENOSYS", "function not implemented"},
      {ENOTEMPTY, "ENOTEMPTY", "directory not empty"},
      {ELOOP, "ELOOP", "too many symbolic links encountered"},
#if defined(EUNATCH)
      {EUNATCH, "EUNATCH", "protocol driver not attached"},
#endif
      {ENODATA, "ENODATA", "no data available"},
#if defined(ENONET)
      {ENONET, "ENONET", "machine is not on the network"},
#endif
      {EPROTO, "EPROTO", "protocol error"},
      {EOVERFLOW, "EOVERFLOW", "value too large for defined data type"},
      {EILSEQ, "EILSEQ", "illegal byte sequence"},
      {ENOTSOCK, "ENOTSOCK", "socket operation on non-socket"},
      {EDESTADDRREQ, "EDESTADDRREQ", "destination address required"},
      {EMSGSIZE, "EMSGSIZE", "message too long"},
      {EPROTOTYPE, "EPROTOTYPE", "protocol wrong type for socket"},
      {ENOPROTOOPT, "ENOPROTOOPT", "protocol not available"},
      {EPROTONOSUPPORT, "EPROTONOSUPPORT", "protocol not supported"},
      {ESOCKTNOSUPPORT, "ESOCKTNOSUPPORT", "socket type not supported"},
      {ENOTSUP, "ENOTSUP", "operation not supported on socket"},
      {EAFNOSUPPORT, "EAFNOSUPPORT", "address family not supported"},
      {EADDRINUSE, "EADDRINUSE", "address already in use"},
      {EADDRNOTAVAIL, "EADDRNOTAVAIL", "address not available"},
      {ENETDOWN, "ENETDOWN", "network is down"},
      {ENETUNREACH, "ENETUNREACH", "network is unreachable"},
      {ECONNABORTED, "ECONNABORTED", "software caused connection abort"},
      {ECONNRESET, "ECONNRESET", "connection reset by peer"},
      {ENOBUFS, "ENOBUFS", "no buffer space available"},
      {EISCONN, "EISCONN", "socket is already connected"},
      {ENOTCONN, "ENOTCONN", "socket is not connected"},
      {ESHUTDOWN, "ESHUTDOWN", "cannot send after transport endpoint shutdown"},
      {ETIMEDOUT, "ETIMEDOUT", "connection timed out"},
      {ECONNREFUSED, "ECONNREFUSED", "connection refused"},
      {EHOSTDOWN, "EHOSTDOWN", "host is down"},
      {EHOSTUNREACH, "EHOSTUNREACH", "host is unreachable"},
      {EALREADY, "EALREADY", "connection already in progress"},
#if defined(EREMOTEIO)
      {EREMOTEIO, "EREMOTEIO", "remote I/O error"},
#endif
      {ECANCELED, "ECANCELED", "operation canceled"},
    };

    const ErrnoEntry* findErrno(int error) noexcept {
      for (const auto& entry : ERRNO_TABLE)
        if (entry.value == error) return &entry;
      return nullptr;
    }

    [[noreturn]] void fail(int error, std::string_view syscall, std::optional<std::string_view> path = std::nullopt,
                           std::optional<std::string_view> dest = std::nullopt) {
      throw errnoError(error, syscall, path, dest);
    }

    // A SystemError Bun raises from FileHandle methods after close().
    [[noreturn]] void closedHandle(std::string_view syscall) {
      throw ErrnoError("Bad file descriptor", "EBADF", std::nullopt, std::string(syscall), std::nullopt, std::nullopt, "SystemError");
    }

    std::string cString(std::string_view text) { return std::string(text); }

    constexpr const char* REPLACEMENT = "\xEF\xBF\xBD";

    // WHATWG UTF-8 decode: each maximal ill-formed subsequence becomes U+FFFD.
    std::string decodeUtf8(std::span<const std::uint8_t> bytes) {
      std::string out;
      out.reserve(bytes.size());
      std::size_t index = 0;
      while (index < bytes.size()) {
        const std::uint8_t lead = bytes[index];
        if (lead < 0x80) {
          out += static_cast<char>(lead);
          ++index;
          continue;
        }
        int need = 0;
        std::uint8_t lower = 0x80, upper = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) need = 1;
        else if (lead >= 0xE0 && lead <= 0xEF) {
          need = 2;
          if (lead == 0xE0) lower = 0xA0;
          if (lead == 0xED) upper = 0x9F;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
          need = 3;
          if (lead == 0xF0) lower = 0x90;
          if (lead == 0xF4) upper = 0x8F;
        } else {
          out += REPLACEMENT;
          ++index;
          continue;
        }
        std::size_t next = index + 1;
        int seen = 0;
        while (seen < need && next < bytes.size() && bytes[next] >= lower && bytes[next] <= upper) {
          lower = 0x80;
          upper = 0xBF;
          ++next;
          ++seen;
        }
        if (seen == need) out.append(reinterpret_cast<const char*>(bytes.data() + index), next - index);
        else out += REPLACEMENT;
        index = next;
      }
      return out;
    }

    // The UTF-8 bytes of a JS string held as WTF-8: a surrogate pair split over
    // two 3-byte sequences joins, a lone surrogate becomes U+FFFD.
    std::string encodeUtf8(std::string_view text) {
      std::string out;
      out.reserve(text.size());
      auto unit = [&](std::size_t at) -> unsigned {
        return ((static_cast<unsigned char>(text[at]) & 0x0Fu) << 12) | ((static_cast<unsigned char>(text[at + 1]) & 0x3Fu) << 6) |
               (static_cast<unsigned char>(text[at + 2]) & 0x3Fu);
      };
      auto surrogateAt = [&](std::size_t at) {
        return at + 2 < text.size() && static_cast<unsigned char>(text[at]) == 0xED && static_cast<unsigned char>(text[at + 1]) >= 0xA0;
      };
      for (std::size_t index = 0; index < text.size();) {
        if (!surrogateAt(index)) {
          out += text[index++];
          continue;
        }
        const unsigned high = unit(index);
        if (high <= 0xDBFF && surrogateAt(index + 3) && unit(index + 3) >= 0xDC00) {
          const unsigned codePoint = 0x10000 + ((high - 0xD800) << 10) + (unit(index + 3) - 0xDC00);
          out += static_cast<char>(0xF0 | (codePoint >> 18));
          out += static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F));
          out += static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F));
          out += static_cast<char>(0x80 | (codePoint & 0x3F));
          index += 6;
        } else {
          out += REPLACEMENT;
          index += 3;
        }
      }
      return out;
    }

    void requireUtf8(std::string_view encoding) {
      if (encoding != "utf8" && encoding != "utf-8" && encoding != "UTF8" && encoding != "UTF-8")
        throw std::invalid_argument("uos::rt::fs: unsupported encoding " + std::string(encoding));
    }

    int stringToFlags(std::string_view flags) {
      if (flags == "r") return O_RDONLY;
      if (flags == "rs" || flags == "sr") return O_RDONLY | O_SYNC;
      if (flags == "r+") return O_RDWR;
      if (flags == "rs+" || flags == "sr+") return O_RDWR | O_SYNC;
      if (flags == "w") return O_TRUNC | O_CREAT | O_WRONLY;
      if (flags == "wx" || flags == "xw") return O_TRUNC | O_CREAT | O_WRONLY | O_EXCL;
      if (flags == "w+") return O_TRUNC | O_CREAT | O_RDWR;
      if (flags == "wx+" || flags == "xw+") return O_TRUNC | O_CREAT | O_RDWR | O_EXCL;
      if (flags == "a") return O_APPEND | O_CREAT | O_WRONLY;
      if (flags == "ax" || flags == "xa") return O_APPEND | O_CREAT | O_WRONLY | O_EXCL;
      if (flags == "as" || flags == "sa") return O_APPEND | O_CREAT | O_WRONLY | O_SYNC;
      if (flags == "a+") return O_APPEND | O_CREAT | O_RDWR;
      if (flags == "ax+" || flags == "xa+") return O_APPEND | O_CREAT | O_RDWR | O_EXCL;
      if (flags == "as+" || flags == "sa+") return O_APPEND | O_CREAT | O_RDWR | O_SYNC;
      throw TypeError("The argument 'flags' is invalid. Received '" + std::string(flags) + "'");
    }

    int openPath(std::string_view path, int flags, std::uint32_t mode) {
      const std::string name = cString(path);
      int fd;
      do fd = ::open(name.c_str(), flags | O_CLOEXEC, static_cast<mode_t>(mode));
      while (fd < 0 && errno == EINTR);
      if (fd < 0) fail(errno, "open", path);
      return fd;
    }

    void closeQuietly(int fd) noexcept { ::close(fd); }

    struct Descriptor {
      int fd;
      ~Descriptor() {
        if (fd >= 0) closeQuietly(fd);
      }
    };

    Bytes readAll(int fd) {
      Bytes out;
      std::uint8_t buffer[65536];
      for (;;) {
        const ssize_t count = ::read(fd, buffer, sizeof buffer);
        if (count < 0) {
          if (errno == EINTR) continue;
          fail(errno, "read");
        }
        if (count == 0) break;
        out.insert(out.end(), buffer, buffer + count);
      }
      return out;
    }

    void writeAll(int fd, std::span<const std::uint8_t> data) {
      std::size_t written = 0;
      while (written < data.size()) {
        const ssize_t count = ::write(fd, data.data() + written, data.size() - written);
        if (count < 0) {
          if (errno == EINTR) continue;
          fail(errno, "write");
        }
        written += static_cast<std::size_t>(count);
      }
    }

    Stats toStats(const struct stat& info) {
      Stats out;
      out.dev = static_cast<std::uint64_t>(info.st_dev);
      out.ino = static_cast<std::uint64_t>(info.st_ino);
      out.mode = static_cast<std::uint32_t>(info.st_mode);
      out.nlink = static_cast<std::uint64_t>(info.st_nlink);
      out.uid = static_cast<std::uint32_t>(info.st_uid);
      out.gid = static_cast<std::uint32_t>(info.st_gid);
      out.rdev = static_cast<std::uint64_t>(info.st_rdev);
      out.size = static_cast<std::int64_t>(info.st_size);
      out.blksize = static_cast<std::int64_t>(info.st_blksize);
      out.blocks = static_cast<std::int64_t>(info.st_blocks);
      auto milliseconds = [](const struct timespec& time) { return static_cast<double>(time.tv_sec) * 1e3 + static_cast<double>(time.tv_nsec) / 1e6; };
      auto nanoseconds = [](const struct timespec& time) { return static_cast<std::int64_t>(time.tv_sec) * 1000000000 + static_cast<std::int64_t>(time.tv_nsec); };
#if defined(__APPLE__)
      const auto& atime = info.st_atimespec;
      const auto& mtime = info.st_mtimespec;
      const auto& ctime = info.st_ctimespec;
#else
      const auto& atime = info.st_atim;
      const auto& mtime = info.st_mtim;
      const auto& ctime = info.st_ctim;
#endif
      out.atimeMs = milliseconds(atime);
      out.mtimeMs = milliseconds(mtime);
      out.ctimeMs = milliseconds(ctime);
      out.atimeNs = nanoseconds(atime);
      out.mtimeNs = nanoseconds(mtime);
      out.ctimeNs = nanoseconds(ctime);
      return out;
    }

    bool isDirectoryPath(const std::string& path) {
      struct stat info;
      return ::stat(path.c_str(), &info) == 0 && S_ISDIR(info.st_mode);
    }

    // Bun's mkdirRecursive: try the whole path, then walk back to the first
    // parent that can be created and forward again.
    std::optional<std::string> mkdirRecursive(std::string_view path, mode_t mode) {
      const std::string full = cString(path);
      if (::mkdir(full.c_str(), mode) == 0) return full;
      const int first = errno;
      if (first == EEXIST) {
        if (isDirectoryPath(full)) return std::nullopt;
        fail(first, "mkdir", path);
      }
      if (first != ENOENT || full.empty()) fail(first, "mkdir", path);
      std::size_t index = full.size() - 1;
      for (; index > 0; --index) {
        if (full[index] != '/') continue;
        const std::string parent = full.substr(0, index);
        if (::mkdir(parent.c_str(), mode) == 0) break;
        const int error = errno;
        if (error == EEXIST) break;
        if (error == ENOENT) continue;
        fail(error, "mkdir", path);
      }
      const std::size_t firstMatch = index;
      for (std::size_t forward = index + 1; forward < full.size(); ++forward) {
        if (full[forward] != '/') continue;
        const std::string parent = full.substr(0, forward);
        if (::mkdir(parent.c_str(), mode) != 0 && errno != EEXIST) fail(errno, "mkdir", path);
      }
      if (::mkdir(full.c_str(), mode) != 0 && errno != EEXIST) fail(errno, "mkdir", path);
      return full.substr(0, firstMatch);
    }

    void removeTree(const std::string& path) {
      struct stat info;
      if (::lstat(path.c_str(), &info) != 0) fail(errno, "rm", path);
      if (!S_ISDIR(info.st_mode)) {
        if (::unlink(path.c_str()) != 0) fail(errno, "rm", path);
        return;
      }
      std::vector<std::string> children;
      {
        std::unique_ptr<DIR, int (*)(DIR*)> directory(::opendir(path.c_str()), &::closedir);
        if (!directory) fail(errno, "rm", path);
        while (const dirent* entry = ::readdir(directory.get())) {
          const std::string_view name = entry->d_name;
          if (name == "." || name == "..") continue;
          children.emplace_back(name);
        }
      }
      for (const auto& child : children) removeTree(path + "/" + child);
      if (::rmdir(path.c_str()) != 0) fail(errno, "rm", path);
    }

    std::vector<Dirent> listDirectory(std::string_view path) {
      const std::string name = cString(path);
      std::unique_ptr<DIR, int (*)(DIR*)> directory(::opendir(name.c_str()), &::closedir);
      if (!directory) fail(errno, "scandir", path);
      std::vector<Dirent> out;
      for (;;) {
        errno = 0;
        const dirent* entry = ::readdir(directory.get());
        if (!entry) {
          if (errno != 0) fail(errno, "scandir", path);
          break;
        }
        const std::string_view entryName = entry->d_name;
        if (entryName == "." || entryName == "..") continue;
        Dirent item;
        item.name = std::string(entryName);
        item.parentPath = name;
        item.type = entry->d_type;
        if (item.type == DT_UNKNOWN) {
          struct stat info;
          if (::fstatat(::dirfd(directory.get()), entry->d_name, &info, AT_SYMLINK_NOFOLLOW) == 0) {
            if (S_ISREG(info.st_mode)) item.type = DT_REG;
            else if (S_ISDIR(info.st_mode)) item.type = DT_DIR;
            else if (S_ISLNK(info.st_mode)) item.type = DT_LNK;
            else if (S_ISFIFO(info.st_mode)) item.type = DT_FIFO;
            else if (S_ISSOCK(info.st_mode)) item.type = DT_SOCK;
            else if (S_ISBLK(info.st_mode)) item.type = DT_BLK;
            else if (S_ISCHR(info.st_mode)) item.type = DT_CHR;
          }
        }
        out.push_back(std::move(item));
      }
      return out;
    }
  }

  ErrnoError::ErrnoError(std::string message_, std::string code_, std::optional<int> errnoValue, std::string syscall_, std::optional<std::string> path_,
                         std::optional<std::string> dest_, std::string name_)
    : JsError(std::move(message_), std::move(name_)),
      code(std::move(code_)),
      errno_(errnoValue),
      syscall(std::move(syscall_)),
      path(std::move(path_)),
      dest(std::move(dest_)) {}

  std::string_view errnoCode(int error) noexcept {
    const auto* entry = findErrno(error);
    return entry ? entry->code : "UNKNOWN";
  }

  std::string_view errnoDescription(int error) noexcept {
    const auto* entry = findErrno(error);
    return entry ? entry->description : "unknown error";
  }

  ErrnoError errnoError(int error, std::string_view syscall, std::optional<std::string_view> path, std::optional<std::string_view> dest) {
    if (path && path->empty()) path.reset();
    std::string message = std::string(errnoCode(error)) + ": " + std::string(errnoDescription(error)) + ", " + std::string(syscall);
    if (path) message += " '" + std::string(*path) + "'";
    if (path && dest) message += " -> '" + std::string(*dest) + "'";
    std::optional<std::string> pathValue, destValue;
    if (path) pathValue = std::string(*path);
    if (dest) destValue = std::string(*dest);
    return ErrnoError(std::move(message), std::string(errnoCode(error)), -error, std::string(syscall), std::move(pathValue), std::move(destValue));
  }

  bool Stats::isFile() const noexcept { return S_ISREG(mode); }
  bool Stats::isDirectory() const noexcept { return S_ISDIR(mode); }
  bool Stats::isSymbolicLink() const noexcept { return S_ISLNK(mode); }
  bool Stats::isFIFO() const noexcept { return S_ISFIFO(mode); }
  bool Stats::isSocket() const noexcept { return S_ISSOCK(mode); }
  bool Stats::isBlockDevice() const noexcept { return S_ISBLK(mode); }
  bool Stats::isCharacterDevice() const noexcept { return S_ISCHR(mode); }

  bool Dirent::isFile() const noexcept { return type == DT_REG; }
  bool Dirent::isDirectory() const noexcept { return type == DT_DIR; }
  bool Dirent::isSymbolicLink() const noexcept { return type == DT_LNK; }

  Bytes readFileSync(std::string_view path) {
    Descriptor file{openPath(path, O_RDONLY, 0)};
    return readAll(file.fd);
  }

  std::string readFileSync(std::string_view path, std::string_view encoding) {
    requireUtf8(encoding);
    return decodeUtf8(readFileSync(path));
  }

  Bytes readFileSync(int fd) { return readAll(fd); }

  std::string readFileSync(int fd, std::string_view encoding) {
    requireUtf8(encoding);
    return decodeUtf8(readAll(fd));
  }

  void writeFileSync(std::string_view path, std::span<const std::uint8_t> data, const WriteFileOptions& options) {
    const int flags = stringToFlags(options.flag);
    Descriptor file{openPath(path, flags, options.mode)};
    writeAll(file.fd, data);
  }

  void writeFileSync(std::string_view path, std::string_view data, const WriteFileOptions& options) {
    const std::string bytes = encodeUtf8(data);
    writeFileSync(path, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()), options);
  }

  int openSync(std::string_view path, std::string_view flags, std::uint32_t mode) { return openPath(path, stringToFlags(flags), mode); }

  int openSync(std::string_view path, int flags, std::uint32_t mode) { return openPath(path, flags, mode); }

  void closeSync(int fd) {
    if (::close(fd) != 0) {
      ErrnoError error = errnoError(errno, "close");
      error.fd = fd;
      throw error;
    }
  }

  Stats fstatSync(int fd) {
    struct stat info;
    if (::fstat(fd, &info) != 0) fail(errno, "fstat");
    return toStats(info);
  }

  void fsyncSync(int fd) {
    if (::fsync(fd) != 0) fail(errno, "fsync");
  }

  Stats statSync(std::string_view path) {
    struct stat info;
    if (::stat(cString(path).c_str(), &info) != 0) fail(errno, "stat", path);
    return toStats(info);
  }

  Stats lstatSync(std::string_view path) {
    struct stat info;
    if (::lstat(cString(path).c_str(), &info) != 0) fail(errno, "lstat", path);
    return toStats(info);
  }

  std::string realpathSync(std::string_view path) {
    std::unique_ptr<char, decltype(&std::free)> resolved(::realpath(cString(path).c_str(), nullptr), &std::free);
    if (!resolved) fail(errno, "lstat", path);
    return resolved.get();
  }

  bool existsSync(std::string_view path) {
    if (path.empty()) return false;
    struct stat info;
    return ::stat(cString(path).c_str(), &info) == 0;
  }

  std::optional<std::string> mkdirSync(std::string_view path, const MkdirOptions& options) {
    const auto mode = static_cast<mode_t>(options.mode);
    if (options.recursive) return mkdirRecursive(path, mode);
    if (::mkdir(cString(path).c_str(), mode) != 0) fail(errno, "mkdir", path);
    return std::nullopt;
  }

  std::string mkdtempSync(std::string_view prefix) {
    static constexpr char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    for (int attempt = 0;; ++attempt) {
      std::uint8_t random[6];
#if defined(__APPLE__)
      ::arc4random_buf(random, sizeof random);
#else
      std::size_t filled = 0;
      while (filled < sizeof random) {
        const ssize_t count = ::getrandom(random + filled, sizeof random - filled, 0);
        if (count < 0) {
          if (errno == EINTR) continue;
          fail(errno, "mkdtemp", prefix);
        }
        filled += static_cast<std::size_t>(count);
      }
#endif
      std::string candidate(prefix);
      for (std::uint8_t byte : random) candidate += ALPHABET[byte % 62];
      if (::mkdir(candidate.c_str(), 0700) == 0) return candidate;
      if (errno != EEXIST || attempt >= 100) fail(errno, "mkdtemp", candidate);
    }
  }

  void rmSync(std::string_view path, const RmOptions& options) {
    const std::string name = cString(path);
    struct stat info;
    if (::lstat(name.c_str(), &info) != 0) {
      const int error = errno;
      if (error == ENOTDIR) fail(EFAULT, "rm", path);
      if (error == ENOENT && options.force) return;
      fail(error, "lstat", path);
    }
    if (S_ISDIR(info.st_mode)) {
      if (!options.recursive)
        throw ErrnoError("Path is a directory: rm returned EISDIR (is a directory) " + name, "ERR_FS_EISDIR", EISDIR, "rm", name, std::nullopt, "SystemError");
      removeTree(name);
      return;
    }
    if (::unlink(name.c_str()) != 0) {
      const int error = errno;
      if (error == ENOENT && options.force) return;
      fail(error, "rm", path);
    }
  }

  void chmodSync(std::string_view path, std::uint32_t mode) {
    if (::chmod(cString(path).c_str(), static_cast<mode_t>(mode)) != 0) fail(errno, "chmod", path);
  }

  void copyFileSync(std::string_view src, std::string_view dest, int mode) {
    const std::string source = cString(src), target = cString(dest);
    int input;
    do input = ::open(source.c_str(), O_RDONLY | O_CLOEXEC);
    while (input < 0 && errno == EINTR);
    if (input < 0) fail(errno, "copyfile", src, dest);
    Descriptor in{input};
    struct stat info;
    if (::fstat(in.fd, &info) != 0) fail(errno, "copyfile", src, dest);
    if (S_ISDIR(info.st_mode)) fail(ENOTSUP, "copyfile", src, dest);
    const int flags = O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | ((mode & constants::COPYFILE_EXCL) ? O_EXCL : 0);
    int output;
    do output = ::open(target.c_str(), flags, info.st_mode & 07777);
    while (output < 0 && errno == EINTR);
    if (output < 0) fail(errno, "copyfile", src, dest);
    Descriptor out{output};
    if (::fchmod(out.fd, info.st_mode & 07777) != 0) fail(errno, "copyfile", src, dest);
    std::uint8_t buffer[65536];
    for (;;) {
      const ssize_t count = ::read(in.fd, buffer, sizeof buffer);
      if (count < 0) {
        if (errno == EINTR) continue;
        fail(errno, "copyfile", src, dest);
      }
      if (count == 0) break;
      std::size_t written = 0;
      while (written < static_cast<std::size_t>(count)) {
        const ssize_t step = ::write(out.fd, buffer + written, static_cast<std::size_t>(count) - written);
        if (step < 0) {
          if (errno == EINTR) continue;
          fail(errno, "copyfile", src, dest);
        }
        written += static_cast<std::size_t>(step);
      }
    }
  }

  void linkSync(std::string_view existingPath, std::string_view newPath) {
    if (::link(cString(existingPath).c_str(), cString(newPath).c_str()) != 0) fail(errno, "link", existingPath, newPath);
  }

  namespace {
    // Bun leaves an empty destination out of the error (message and `dest`).
    std::optional<std::string_view> destination(std::string_view dest) {
      if (dest.empty()) return std::nullopt;
      return dest;
    }

    // Bun's TimeLike for utimes on Linux: a number of seconds is {trunc(s), trunc(fract(s) *
    // 1e9)} with the seconds saturated to int64 (a negative number is now: UTIME_NOW); a Date's
    // milliseconds are {floor(ms / 1000), (ms mod 1000) * 1e6} with a floored modulo.
    timespec timeSpec(const TimeLike& time, const char* name) {
      const double value = time.value();
      if (!std::isfinite(value)) {
        TypeError error(std::string(name) + " must be a number or a Date");
        error.code = "ERR_INVALID_ARG_TYPE";
        throw error;
      }
      timespec out{};
      if (time.isDate()) {
        double remainder = std::fmod(value, 1000.0);
        if (remainder < 0) remainder += 1000.0;
        out.tv_sec = static_cast<time_t>(std::floor(value / 1000.0));
        out.tv_nsec = static_cast<long>(remainder * 1e6);
      } else if (value < 0) {
        out.tv_nsec = UTIME_NOW;
      } else {
        out.tv_sec = value >= 9223372036854775808.0 ? std::numeric_limits<time_t>::max() : static_cast<time_t>(value);
        out.tv_nsec = static_cast<long>(std::fmod(value, 1.0) * 1e9);
      }
      return out;
    }
  }

  void symlinkSync(std::string_view target, std::string_view path) {
    if (::symlink(cString(target).c_str(), cString(path).c_str()) != 0) fail(errno, "symlink", target, destination(path));
  }

  void renameSync(std::string_view oldPath, std::string_view newPath) {
    if (::rename(cString(oldPath).c_str(), cString(newPath).c_str()) != 0) fail(errno, "rename", oldPath, destination(newPath));
  }

  void utimesSync(std::string_view path, TimeLike atime, TimeLike mtime) {
    const timespec times[2] = {timeSpec(atime, "atime"), timeSpec(mtime, "mtime")};
    if (::utimensat(AT_FDCWD, cString(path).c_str(), times, 0) != 0) fail(errno, "utime", path);
  }

  std::vector<std::string> readdirSync(std::string_view path) {
    std::vector<std::string> out;
    for (auto& entry : listDirectory(path)) out.push_back(std::move(entry.name));
    return out;
  }

  std::vector<Dirent> readdirSync(std::string_view path, WithFileTypes) { return listDirectory(path); }

  FileHandle::FileHandle(FileHandle&& other) noexcept : fd(std::exchange(other.fd, -1)) {}

  FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
    if (this != &other) {
      if (fd >= 0) closeQuietly(fd);
      fd = std::exchange(other.fd, -1);
    }
    return *this;
  }

  FileHandle::~FileHandle() {
    if (fd >= 0) closeQuietly(fd);
  }

  Stats FileHandle::stat() const {
    if (fd < 0) closedHandle("fstat");
    return fstatSync(fd);
  }

  FileHandle::ReadResult FileHandle::read(std::span<std::uint8_t> buffer, std::size_t offset, std::size_t length, std::optional<std::int64_t> position) const {
    if (fd < 0) closedHandle("read");
    if (offset > buffer.size() || length > buffer.size() - offset) throw std::invalid_argument("uos::rt::fs::FileHandle::read: offset and length exceed the buffer");
    for (;;) {
      const ssize_t count = position ? ::pread(fd, buffer.data() + offset, length, static_cast<off_t>(*position)) : ::read(fd, buffer.data() + offset, length);
      if (count < 0) {
        if (errno == EINTR) continue;
        fail(errno, "read");
      }
      return ReadResult{static_cast<std::size_t>(count)};
    }
  }

  void FileHandle::close() {
    if (fd < 0) return;
    const int closing = std::exchange(fd, -1);
    if (::close(closing) != 0) fail(errno, "close");
  }

  namespace promises {
    Bytes readFile(std::string_view path) { return readFileSync(path); }
    std::string readFile(std::string_view path, std::string_view encoding) { return readFileSync(path, encoding); }
    void writeFile(std::string_view path, std::string_view data, const WriteFileOptions& options) { writeFileSync(path, data, options); }
    void writeFile(std::string_view path, std::span<const std::uint8_t> data, const WriteFileOptions& options) { writeFileSync(path, data, options); }
    Stats stat(std::string_view path) { return statSync(path); }
    Stats lstat(std::string_view path) { return lstatSync(path); }
    std::string realpath(std::string_view path) { return realpathSync(path); }
    std::vector<std::string> readdir(std::string_view path) { return readdirSync(path); }
    std::vector<Dirent> readdir(std::string_view path, WithFileTypes tag) { return readdirSync(path, tag); }
    std::optional<std::string> mkdir(std::string_view path, const MkdirOptions& options) { return mkdirSync(path, options); }
    std::string mkdtemp(std::string_view prefix) { return mkdtempSync(prefix); }
    void rm(std::string_view path, const RmOptions& options) { rmSync(path, options); }
    FileHandle open(std::string_view path, std::string_view flags, std::uint32_t mode) { return FileHandle(openSync(path, flags, mode)); }
  }
}

namespace uos::rt::path {
  namespace {
    std::string currentDirectory() {
      std::string buffer(PATH_MAX, '\0');
      while (::getcwd(buffer.data(), buffer.size()) == nullptr) {
        if (errno != ERANGE) return std::string();
        buffer.resize(buffer.size() * 2);
      }
      buffer.resize(std::strlen(buffer.c_str()));
      return buffer;
    }

    // Node's normalizeString for POSIX separators.
    std::string normalizeString(std::string_view path, bool allowAboveRoot) {
      std::string res;
      std::ptrdiff_t lastSegmentLength = 0;
      std::ptrdiff_t lastSlash = -1;
      int dots = 0;
      char code = 0;
      const auto length = static_cast<std::ptrdiff_t>(path.size());
      for (std::ptrdiff_t i = 0; i <= length; ++i) {
        if (i < length) code = path[static_cast<std::size_t>(i)];
        else if (code == '/') break;
        else code = '/';
        if (code == '/') {
          if (lastSlash == i - 1 || dots == 1) {
          } else if (dots == 2) {
            if (res.size() < 2 || lastSegmentLength != 2 || res[res.size() - 1] != '.' || res[res.size() - 2] != '.') {
              if (res.size() > 2) {
                const auto lastSlashIndex = res.rfind('/');
                if (lastSlashIndex == std::string::npos) {
                  res.clear();
                  lastSegmentLength = 0;
                } else {
                  res.resize(lastSlashIndex);
                  const auto previous = res.rfind('/');
                  lastSegmentLength = static_cast<std::ptrdiff_t>(res.size()) - 1 - (previous == std::string::npos ? -1 : static_cast<std::ptrdiff_t>(previous));
                }
                lastSlash = i;
                dots = 0;
                continue;
              } else if (!res.empty()) {
                res.clear();
                lastSegmentLength = 0;
                lastSlash = i;
                dots = 0;
                continue;
              }
            }
            if (allowAboveRoot) {
              res += res.empty() ? ".." : "/..";
              lastSegmentLength = 2;
            }
          } else {
            const auto segment = path.substr(static_cast<std::size_t>(lastSlash + 1), static_cast<std::size_t>(i - lastSlash - 1));
            if (!res.empty()) {
              res += '/';
              res += segment;
            } else {
              res = std::string(segment);
            }
            lastSegmentLength = i - lastSlash - 1;
          }
          lastSlash = i;
          dots = 0;
        } else if (code == '.' && dots != -1) {
          ++dots;
        } else {
          dots = -1;
        }
      }
      return res;
    }
  }

  std::string resolveAll(std::span<const std::string_view> parts) {
    std::string resolvedPath;
    bool resolvedAbsolute = false;
    std::string cwd;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(parts.size()) - 1; i >= -1 && !resolvedAbsolute; --i) {
      std::string_view part;
      if (i >= 0) part = parts[static_cast<std::size_t>(i)];
      else {
        cwd = currentDirectory();
        part = cwd;
      }
      if (part.empty()) continue;
      resolvedPath = std::string(part) + "/" + resolvedPath;
      resolvedAbsolute = part.front() == '/';
    }
    resolvedPath = normalizeString(resolvedPath, !resolvedAbsolute);
    if (resolvedAbsolute) return "/" + resolvedPath;
    return resolvedPath.empty() ? "." : resolvedPath;
  }

  std::string normalize(std::string_view path) {
    if (path.empty()) return ".";
    const bool absolute = path.front() == '/';
    const bool trailingSeparator = path.back() == '/';
    std::string out = normalizeString(path, !absolute);
    if (out.empty()) {
      if (absolute) return "/";
      return trailingSeparator ? "./" : ".";
    }
    if (trailingSeparator) out += '/';
    return absolute ? "/" + out : out;
  }

  bool isAbsolute(std::string_view path) noexcept { return !path.empty() && path.front() == '/'; }

  std::string joinAll(std::span<const std::string_view> parts) {
    std::optional<std::string> joined;
    for (const auto part : parts) {
      if (part.empty()) continue;
      if (!joined) joined = std::string(part);
      else {
        *joined += '/';
        *joined += part;
      }
    }
    if (!joined) return ".";
    return normalize(*joined);
  }

  std::string relative(std::string_view fromInput, std::string_view toInput) {
    if (fromInput == toInput) return "";
    const std::string from = resolve(fromInput);
    const std::string to = resolve(toInput);
    if (from == to) return "";
    const std::size_t fromStart = 1, fromEnd = from.size(), fromLen = fromEnd - fromStart;
    const std::size_t toStart = 1, toLen = to.size() - toStart;
    const std::size_t length = fromLen < toLen ? fromLen : toLen;
    std::ptrdiff_t lastCommonSep = -1;
    std::size_t i = 0;
    for (; i < length; ++i) {
      const char fromCode = from[fromStart + i];
      if (fromCode != to[toStart + i]) break;
      if (fromCode == '/') lastCommonSep = static_cast<std::ptrdiff_t>(i);
    }
    if (i == length) {
      if (toLen > length) {
        if (to[toStart + i] == '/') return to.substr(toStart + i + 1);
        if (i == 0) return to.substr(toStart + i);
      } else if (fromLen > length) {
        if (from[fromStart + i] == '/') lastCommonSep = static_cast<std::ptrdiff_t>(i);
        else if (i == 0) lastCommonSep = 0;
      }
    }
    std::string out;
    for (std::size_t j = fromStart + static_cast<std::size_t>(lastCommonSep + 1); j <= fromEnd; ++j) {
      if (j == fromEnd || from[j] == '/') out += out.empty() ? ".." : "/..";
    }
    return out + to.substr(static_cast<std::size_t>(static_cast<std::ptrdiff_t>(toStart) + lastCommonSep));
  }

  std::string dirname(std::string_view path) {
    if (path.empty()) return ".";
    const bool hasRoot = path.front() == '/';
    std::ptrdiff_t end = -1;
    bool matchedSlash = true;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1; i >= 1; --i) {
      if (path[static_cast<std::size_t>(i)] == '/') {
        if (!matchedSlash) {
          end = i;
          break;
        }
      } else {
        matchedSlash = false;
      }
    }
    if (end == -1) return hasRoot ? "/" : ".";
    if (hasRoot && end == 1) return "//";
    return std::string(path.substr(0, static_cast<std::size_t>(end)));
  }

  std::string basename(std::string_view path) {
    std::size_t start = 0;
    std::ptrdiff_t end = -1;
    bool matchedSlash = true;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1; i >= 0; --i) {
      if (path[static_cast<std::size_t>(i)] == '/') {
        if (!matchedSlash) {
          start = static_cast<std::size_t>(i + 1);
          break;
        }
      } else if (end == -1) {
        matchedSlash = false;
        end = i + 1;
      }
    }
    if (end == -1) return "";
    return std::string(path.substr(start, static_cast<std::size_t>(end) - start));
  }

  std::string basename(std::string_view path, std::string_view suffix) {
    if (suffix.empty() || suffix.size() > path.size()) return basename(path);
    if (suffix == path) return "";
    std::size_t start = 0;
    std::ptrdiff_t end = -1;
    bool matchedSlash = true;
    std::ptrdiff_t extIdx = static_cast<std::ptrdiff_t>(suffix.size()) - 1;
    std::ptrdiff_t firstNonSlashEnd = -1;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1; i >= 0; --i) {
      const char code = path[static_cast<std::size_t>(i)];
      if (code == '/') {
        if (!matchedSlash) {
          start = static_cast<std::size_t>(i + 1);
          break;
        }
      } else {
        if (firstNonSlashEnd == -1) {
          matchedSlash = false;
          firstNonSlashEnd = i + 1;
        }
        if (extIdx >= 0) {
          if (code == suffix[static_cast<std::size_t>(extIdx)]) {
            if (--extIdx == -1) end = i;
          } else {
            extIdx = -1;
            end = firstNonSlashEnd;
          }
        }
      }
    }
    if (static_cast<std::ptrdiff_t>(start) == end) end = firstNonSlashEnd;
    else if (end == -1) end = static_cast<std::ptrdiff_t>(path.size());
    return std::string(path.substr(start, static_cast<std::size_t>(end) - start));
  }

  std::string extname(std::string_view path) {
    std::ptrdiff_t startDot = -1;
    std::ptrdiff_t startPart = 0;
    std::ptrdiff_t end = -1;
    bool matchedSlash = true;
    int preDotState = 0;
    for (std::ptrdiff_t i = static_cast<std::ptrdiff_t>(path.size()) - 1; i >= 0; --i) {
      const char code = path[static_cast<std::size_t>(i)];
      if (code == '/') {
        if (!matchedSlash) {
          startPart = i + 1;
          break;
        }
        continue;
      }
      if (end == -1) {
        matchedSlash = false;
        end = i + 1;
      }
      if (code == '.') {
        if (startDot == -1) startDot = i;
        else if (preDotState != 1) preDotState = 1;
      } else if (startDot != -1) {
        preDotState = -1;
      }
    }
    if (startDot == -1 || end == -1 || preDotState == 0 || (preDotState == 1 && startDot == end - 1 && startDot == startPart + 1)) return "";
    return std::string(path.substr(static_cast<std::size_t>(startDot), static_cast<std::size_t>(end - startDot)));
  }
}
