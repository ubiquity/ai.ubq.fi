#include "uos/rt/admission_resources.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <limits>
#include <span>
#include <string_view>
#include <utility>

#if defined(__linux__) || defined(__APPLE__)
#include <sys/resource.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/magic.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#endif

#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/sysctl.h>
#include <vector>
#endif

namespace uos::rt::admission_resources {
  namespace {
#if defined(__linux__) || defined(__APPLE__)
    constexpr std::uint64_t descriptorReserve = 32;

    std::uint64_t subtract(std::uint64_t value, std::uint64_t amount) noexcept {
      return value > amount ? value - amount : 0;
    }

    std::uint64_t reserveMemory(std::uint64_t available, std::uint64_t total) noexcept {
      return subtract(available, total / 5 + (total % 5 != 0));
    }

    std::optional<std::uint64_t> descriptorLimit() noexcept {
      struct rlimit limit {};
      if (::getrlimit(RLIMIT_NOFILE, &limit) != 0) return std::nullopt;
#if defined(__APPLE__)
      int maximum = 0;
      std::size_t size = sizeof(maximum);
      if (::sysctlbyname("kern.maxfilesperproc", &maximum, &size, nullptr, 0) == 0 &&
          size == sizeof(maximum) && maximum > 0)
        return std::min(static_cast<std::uint64_t>(limit.rlim_cur), static_cast<std::uint64_t>(maximum));
#endif
      if (limit.rlim_cur == RLIM_INFINITY) return std::nullopt;
      return static_cast<std::uint64_t>(limit.rlim_cur);
    }
#endif

#if defined(__linux__)
    constexpr std::string_view meminfoPath = "/proc/meminfo";
    constexpr std::size_t maxMountinfoBytes = 256 * 1024;
    constexpr std::size_t maxCgroupDepth = 64;

    std::string_view take(std::string_view& text, char delimiter) noexcept {
      const auto end = text.find(delimiter);
      const auto first = text.substr(0, end);
      text.remove_prefix(end == text.npos ? text.size() : end + 1);
      return first;
    }

    std::string_view trim(std::string_view text) noexcept {
      const auto start = text.find_first_not_of(" \t\n");
      if (start == text.npos) return {};
      return text.substr(start, text.find_last_not_of(" \t\n") - start + 1);
    }

    std::optional<std::uint64_t> number(std::string_view text) noexcept {
      if (text.empty()) return std::nullopt;
      std::uint64_t result{};
      const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), result);
      if (error != std::errc{} || end != text.data() + text.size()) return std::nullopt;
      return result;
    }

    std::optional<std::uint64_t> kibibytes(std::string_view text) noexcept {
      if (!text.ends_with(" kB")) return std::nullopt;
      text.remove_suffix(3);
      const auto value = number(trim(text));
      if (!value || *value > std::numeric_limits<std::uint64_t>::max() / 1024) return std::nullopt;
      return *value * 1024;
    }

    struct File {
      int fd;
      ~File() { if (fd >= 0) ::close(fd); }
    };

    std::optional<std::string_view> readFile(int fd, std::span<char> buffer) noexcept {
      if (fd < 0) return std::nullopt;
      std::size_t offset = 0;
      while (offset < buffer.size()) {
        const auto size = ::read(fd, buffer.data() + offset, buffer.size() - offset);
        if (size < 0 && errno == EINTR) continue;
        if (size < 0) return std::nullopt;
        if (size == 0) return std::string_view(buffer.data(), offset);
        offset += static_cast<std::size_t>(size);
      }
      return std::nullopt;
    }

    bool onFilesystem(int fd, long type) noexcept {
      struct statfs info {};
      return fd >= 0 && ::fstatfs(fd, &info) == 0 && info.f_type == type;
    }

    std::optional<std::string_view> readProc(const char* path, std::span<char> buffer) noexcept {
      const File file{::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
      if (!onFilesystem(file.fd, PROC_SUPER_MAGIC)) return std::nullopt;
      return readFile(file.fd, buffer);
    }

    // All components must be real filesystem names. The directory entry for
    // a cgroup2 mount must itself be a cgroup directory.
    int openKernelPath(const char* path, int flags) noexcept {
      const struct open_how how{
        .flags = static_cast<std::uint64_t>(flags | O_CLOEXEC),
        .mode = 0,
        .resolve = RESOLVE_NO_SYMLINKS,
      };
      return static_cast<int>(::syscall(SYS_openat2, AT_FDCWD, path, &how, sizeof(how)));
    }

    struct Counter {
      std::string path;
      dev_t device;
      ino_t inode;

      bool matches(int fd) const noexcept {
        struct stat info {};
        return fd >= 0 && ::fstat(fd, &info) == 0 && info.st_dev == device && info.st_ino == inode;
      }

      std::optional<std::string_view> read(std::span<char> buffer) const noexcept {
        const File file{::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW)};
        return matches(file.fd) ? readFile(file.fd, buffer) : std::nullopt;
      }
    };

    std::optional<Counter> counter(const std::string& path, long filesystem, bool& missing) {
      const File file{openKernelPath(path.c_str(), O_PATH)};
      missing = file.fd < 0 && errno == ENOENT;
      struct stat info {};
      if (!onFilesystem(file.fd, filesystem) || ::fstat(file.fd, &info) != 0 || !S_ISREG(info.st_mode)) return std::nullopt;
      return Counter{path, info.st_dev, info.st_ino};
    }

    bool absolutePath(std::string_view path) noexcept {
      if (!path.starts_with('/') || path.size() >= 4096 || path.find('\0') != path.npos) return false;
      if (path == "/") return true;
      if (path.ends_with('/')) return false;
      path.remove_prefix(1);
      while (!path.empty()) {
        const auto part = take(path, '/');
        if (part.empty() || part == "." || part == "..") return false;
      }
      return true;
    }

    std::optional<std::string> mountPath(std::string_view text) {
      std::string path;
      path.reserve(text.size());
      while (!text.empty()) {
        const char character = text.front();
        text.remove_prefix(1);
        if (character != '\\') {
          path.push_back(character);
          continue;
        }
        const auto escape = text.substr(0, 3);
        if (escape == "040") path.push_back(' ');
        else if (escape == "011") path.push_back('\t');
        else if (escape == "012") path.push_back('\n');
        else if (escape == "134") path.push_back('\\');
        else return std::nullopt;
        text.remove_prefix(3);
      }
      return absolutePath(path) ? std::optional(std::move(path)) : std::nullopt;
    }

    bool beneath(std::string_view root, std::string_view path) noexcept {
      return root == "/" || path == root || (path.starts_with(root) && path[root.size()] == '/');
    }

    struct CgroupMount {
      std::string root;
      std::string point;
      std::string group;
    };

    std::optional<CgroupMount> findMount(std::string_view mountinfo, std::string_view member) {
      std::optional<CgroupMount> selected;
      while (!mountinfo.empty()) {
        auto line = take(mountinfo, '\n');
        const auto separator = line.find(" - ");
        if (separator == line.npos || !line.substr(separator + 3).starts_with("cgroup2 ")) continue;
        line = line.substr(0, separator);
        for (int index = 0; index < 3; ++index) (void)take(line, ' ');
        auto root = mountPath(take(line, ' '));
        auto point = mountPath(take(line, ' '));
        if (!root || !point || !beneath(*root, member) || (selected && selected->root.size() <= root->size())) continue;
        const auto relative = member.substr(*root == "/" ? 0 : root->size());
        std::string group;
        if (relative.empty() || relative == "/") group = *point;
        else group = (*point == "/" ? std::string{} : *point) + std::string(relative);
        if (!absolutePath(group)) continue;
        const File directory{openKernelPath(group.c_str(), O_PATH | O_DIRECTORY)};
        if (!onFilesystem(directory.fd, CGROUP2_SUPER_MAGIC)) continue;
        selected = CgroupMount{std::move(*root), std::move(*point), std::move(group)};
      }
      return selected;
    }

    struct CgroupCounters {
      Counter maximum;
      std::optional<Counter> current;
    };

    struct LinuxCounters {
      std::optional<Counter> meminfo;
      std::vector<CgroupCounters> groups;
      bool groupsReadable = true;
    };

    bool discoverCgroups(LinuxCounters& counters) {
      std::array<char, 16 * 1024> cgroup{};
      auto membership = readProc("/proc/self/cgroup", cgroup);
      if (!membership) return false;
      while (!membership->empty()) {
        const auto line = take(*membership, '\n');
        if (!line.starts_with("0::")) continue;
        const auto member = line.substr(3);
        if (!absolutePath(member)) return false;
        std::string mountinfo(maxMountinfoBytes, '\0');
        const auto mounts = readProc("/proc/self/mountinfo", mountinfo);
        if (!mounts) return false;
        auto mount = findMount(*mounts, member);
        if (!mount) {
          // A cgroup2 view may be absent or deliberately masked in a
          // container. A nested membership with no accessible view is unknown.
          return member == "/" && mounts->find(" - cgroup2 ") == mounts->npos;
        }
        for (std::size_t depth = 0; depth < maxCgroupDepth; ++depth) {
          const std::string prefix = mount->group == "/" ? std::string{} : mount->group;
          bool missingMaximum = false;
          bool missingCurrent = false;
          auto maximum = counter(prefix + "/memory.max", CGROUP2_SUPER_MAGIC, missingMaximum);
          auto current = counter(prefix + "/memory.current", CGROUP2_SUPER_MAGIC, missingCurrent);
          if (maximum) counters.groups.push_back({std::move(*maximum), std::move(current)});
          else if (!missingMaximum || !missingCurrent) return false;
          // The unified root and groups without a memory controller have
          // neither file. Every visible ancestor can limit its descendants.
          if (mount->group == mount->point) return true;
          const auto slash = mount->group.rfind('/');
          mount->group.resize(std::max(std::size_t{1}, slash));
        }
        return false;
      }
      return true;  // No unified hierarchy is configured.
    }

    const LinuxCounters& linuxCounters() {
      static const auto counters = [] {
        LinuxCounters result;
        bool missing = false;
        result.meminfo = counter(std::string(meminfoPath), PROC_SUPER_MAGIC, missing);
        result.groupsReadable = discoverCgroups(result);
        return result;
      }();
      return counters;
    }

    const Counter* findCounter(std::string_view path) {
      const auto& counters = linuxCounters();
      if (counters.meminfo && counters.meminfo->path == path) return &*counters.meminfo;
      for (const auto& group : counters.groups) {
        if (group.maximum.path == path) return &group.maximum;
        if (group.current && group.current->path == path) return &*group.current;
      }
      return nullptr;
    }

    std::optional<std::uint64_t> memoryHeadroom() {
      const auto& counters = linuxCounters();
      if (!counters.meminfo || !counters.groupsReadable) return std::nullopt;
      std::array<char, 8192> host{};
      const auto meminfo = counters.meminfo->read(host);
      auto available = meminfo ? detail::linuxHostHeadroom(*meminfo) : std::nullopt;
      if (!available) return std::nullopt;
      for (const auto& group : counters.groups) {
        std::array<char, 64> maximum{};
        const auto limit = group.maximum.read(maximum);
        if (!limit) return std::nullopt;
        if (trim(*limit) == "max") continue;
        std::array<char, 64> current{};
        const auto usage = group.current ? group.current->read(current) : std::nullopt;
        const auto headroom = usage ? detail::linuxLimitedHeadroom(*limit, *usage) : std::nullopt;
        if (!headroom) return std::nullopt;
        available = std::min(*available, *headroom);
      }
      return available;
    }

    std::optional<std::uint64_t> descriptorHeadroom() noexcept {
      const auto limit = descriptorLimit();
      if (!limit) return std::nullopt;
      const File directory{::open("/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
      if (directory.fd < 0) return errno == EMFILE ? std::optional<std::uint64_t>(0) : std::nullopt;
      std::array<char, 8192> buffer;
      std::uint64_t used = 0;
      for (;;) {
        const auto count = ::syscall(SYS_getdents64, directory.fd, buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) return std::nullopt;
        if (count == 0) break;
        constexpr auto nameOffset = offsetof(struct dirent64, d_name);
        const auto end = static_cast<std::size_t>(count);
        for (std::size_t offset = 0; offset < end;) {
          if (end - offset <= nameOffset) return std::nullopt;
          unsigned short length = 0;
          std::memcpy(&length, buffer.data() + offset + offsetof(struct dirent64, d_reclen), sizeof(length));
          if (length <= nameOffset || length > end - offset) return std::nullopt;
          const char* name = buffer.data() + offset + nameOffset;
          const auto* terminator = static_cast<const char*>(std::memchr(name, 0, length - nameOffset));
          if (!terminator) return std::nullopt;
          const auto descriptor = number(std::string_view(name, static_cast<std::size_t>(terminator - name)));
          if (descriptor && *descriptor != static_cast<std::uint64_t>(directory.fd) && *descriptor < *limit) ++used;
          offset += length;
        }
      }
      return subtract(subtract(*limit, used), descriptorReserve);
    }
#elif defined(__APPLE__)
    std::optional<std::uint64_t> memoryHeadroom() noexcept {
      static const std::uint64_t total = [] {
        std::uint64_t bytes = 0;
        std::size_t size = sizeof(bytes);
        if (::sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0 || size != sizeof(bytes)) return std::uint64_t{0};
        return bytes;
      }();
      if (total == 0) return std::nullopt;
      const auto host = ::mach_host_self();
      if (host == MACH_PORT_NULL) return std::nullopt;
      vm_statistics64_data_t stats{};
      mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
      vm_size_t pageSize = 0;
      const auto status = ::host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&stats), &count);
      const auto pageStatus = ::host_page_size(host, &pageSize);
      (void)::mach_port_deallocate(::mach_task_self(), host);
      if (status != KERN_SUCCESS || pageStatus != KERN_SUCCESS || pageSize == 0 || count < HOST_VM_INFO64_REV1_COUNT)
        return std::nullopt;
      // Count free and file-backed pages, excluding anonymous and compressed
      // memory. The kernel includes speculative pages in both counters.
      const auto cached = subtract(stats.external_page_count, stats.speculative_count);
      const auto pages = static_cast<std::uint64_t>(stats.free_count) + cached;
      return reserveMemory(std::min(pages, total / pageSize) * pageSize, total);
    }

    std::optional<std::uint64_t> descriptorHeadroom() {
      const auto limit = descriptorLimit();
      if (!limit) return std::nullopt;
      std::array<proc_fdinfo, 2048> entries;
      std::vector<proc_fdinfo> overflow;
      std::span<proc_fdinfo> descriptors(entries);
      const int pid = ::getpid();
      errno = 0;
      int bytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, entries.data(), static_cast<int>(sizeof(entries)));
      if (bytes == static_cast<int>(sizeof(entries))) {
        const int needed = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, nullptr, 0);
        if (needed < bytes || needed >= 1024 * 1024) return std::nullopt;
        overflow.resize(static_cast<std::size_t>(needed) / sizeof(proc_fdinfo) + 1);
        descriptors = overflow;
        errno = 0;
        bytes = ::proc_pidinfo(pid, PROC_PIDLISTFDS, 0, overflow.data(), static_cast<int>(descriptors.size_bytes()));
      }
      if (bytes < 0 || (bytes == 0 && errno != 0)) return std::nullopt;
      const auto length = static_cast<std::size_t>(bytes);
      if (length >= descriptors.size_bytes() || length % sizeof(proc_fdinfo) != 0) return std::nullopt;
      const auto snapshot = descriptors.first(length / sizeof(proc_fdinfo));
      const auto used = std::ranges::count_if(snapshot, [limit](const auto& entry) {
        return entry.proc_fd >= 0 && static_cast<std::uint64_t>(entry.proc_fd) < *limit;
      });
      return subtract(subtract(*limit, static_cast<std::uint64_t>(used)), descriptorReserve);
    }
#endif
  }

#if defined(__linux__)
  std::optional<std::uint64_t> detail::linuxHostHeadroom(std::string_view meminfo) {
    std::optional<std::uint64_t> total;
    std::optional<std::uint64_t> available;
    while (!meminfo.empty()) {
      const auto line = take(meminfo, '\n');
      if (line.starts_with("MemTotal:")) {
        if (total) return std::nullopt;
        total = kibibytes(line.substr(9));
        if (!total) return std::nullopt;
      } else if (line.starts_with("MemAvailable:")) {
        if (available) return std::nullopt;
        available = kibibytes(line.substr(13));
        if (!available) return std::nullopt;
      }
    }
    if (!total || !available || *total == 0 || *available > *total) return std::nullopt;
    return reserveMemory(*available, *total);
  }

  std::optional<std::uint64_t> detail::linuxLimitedHeadroom(std::string_view maximum, std::string_view current) {
    const auto limit = number(trim(maximum));
    const auto used = number(trim(current));
    if (!limit || !used) return std::nullopt;
    return reserveMemory(subtract(*limit, *used), *limit);
  }

  void detail::appendLinuxReadPaths(std::vector<std::string>& paths) {
    const auto& counters = linuxCounters();
    if (counters.meminfo) paths.push_back(counters.meminfo->path);
    for (const auto& group : counters.groups) {
      paths.push_back(group.maximum.path);
      if (group.current) paths.push_back(group.current->path);
    }
  }

  bool detail::isLinuxReadPath(std::string_view path) { return findCounter(path) != nullptr; }

  int detail::openLinuxReadGrant(std::string_view path) {
    const auto* entry = findCounter(path);
    if (!entry) return -1;
    const int fd = openKernelPath(entry->path.c_str(), O_PATH);
    if (entry->matches(fd)) return fd;
    if (fd >= 0) ::close(fd);
    return -1;
  }
#endif

  Headroom readHeadroom() {
#if defined(__linux__) || defined(__APPLE__)
    return {.memoryBytes = memoryHeadroom(), .descriptors = descriptorHeadroom()};
#else
    return {};
#endif
  }
}
