// Fixture-only probe. A private inherited socket requests arena info or a trim from an
// ordinary thread, with no production route, signal handler or allocator hook.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <pthread.h>
#include <unistd.h>

namespace {
  char* arenaInfo(std::size_t& size) {
    char* xml = nullptr;
    FILE* stream = open_memstream(&xml, &size);
    if (!stream) return nullptr;
    const int result = malloc_info(0, stream);
    std::fclose(stream);
    if (result == 0) return xml;
    std::free(xml);
    return nullptr;
  }

  std::size_t countArenas() {
    std::size_t size = 0;
    char* xml = arenaInfo(size);
    std::size_t count = 0;
    if (xml) {
      for (const char* next = xml; (next = std::strstr(next, "<heap nr=")); ++next) ++count;
    }
    std::free(xml);
    return count;
  }

  void* serve(void* context) {
    const int fd = *static_cast<int*>(context);
    char request;
    while (::read(fd, &request, 1) == 1) {
      if (request == 'i') {
        std::size_t size = 0;
        char* xml = arenaInfo(size);
        const auto written = xml ? ::write(fd, xml, size) : -1;
        std::free(xml);
        if (written <= 0 || static_cast<std::size_t>(written) != size) break;
        continue;
      }
      const auto arenas = countArenas();
      const auto before = mallinfo2();
      const int trimmed = malloc_trim(0);
      const auto after = mallinfo2();
      char reply[256];
      const int size = std::snprintf(reply, sizeof reply, "%d %zu %zu %zu %zu %zu %zu %zu\n",
                                     trimmed, arenas, before.uordblks, before.fordblks, before.hblkhd,
                                     after.uordblks, after.fordblks, after.hblkhd);
      if (size <= 0 || static_cast<std::size_t>(size) >= sizeof reply ||
          ::write(fd, reply, static_cast<std::size_t>(size)) != size) break;
    }
    ::close(fd);
    return nullptr;
  }

  __attribute__((constructor)) void install() {
    const char* descriptor = std::getenv("UOS_MEMORY_TRIM_FD");
    if (!descriptor) return;
    static int fd;
    fd = std::atoi(descriptor);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, serve, &fd) == 0) pthread_detach(thread);
    else ::close(fd);
  }
}
