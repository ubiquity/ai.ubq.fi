#include <functional>
#include <memory>
#include <vector>
#include <fcntl.h>
#include <unistd.h>

namespace {
struct Descriptor {
  int value;
  explicit Descriptor(int fd):value(fd){}
  ~Descriptor(){if(value>=0)::close(value);}
  Descriptor(Descriptor const&)=delete;
  Descriptor& operator=(Descriptor const&)=delete;
};
}

int clean_unique_ptr(bool refuse) {
  auto pointer = std::make_unique<int>(42);
  if (refuse) return -1;
  return *pointer;
}

bool clean_descriptor(bool refuse) {
  Descriptor fd(::open("/dev/null",O_RDONLY|O_CLOEXEC));
  if (refuse) return false;
  return fd.value>=0;
}

int clean_shared_callback() {
  auto owner=std::make_shared<int>(42);
  std::weak_ptr<int> observer=owner;
  std::vector<std::function<int()>> callbacks;
  callbacks.emplace_back([owner]{return *owner;});
  owner.reset();
  int result=callbacks.front()();
  callbacks.clear();
  return observer.expired()?result:0;
}
