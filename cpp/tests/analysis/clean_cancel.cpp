// Named coroutine/real-pipe ownership adapted from the qualified isolated probe.
// Static input only; existing sanitizer socket tests exercise actual cancellation.
#include <utility>
#include <array>
#include <memory>
#include <stdexcept>
#include <boost/asio/awaitable.hpp>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/redirect_error.hpp>
#include <boost/asio/use_awaitable.hpp>
#include <unistd.h>

namespace {
namespace asio=boost::asio;
struct Fd {
  int value;
  explicit Fd(int fd):value(fd){}
  ~Fd(){if(value>=0)::close(value);}
  Fd(Fd const&)=delete;
  Fd& operator=(Fd const&)=delete;
};
struct Control {
  asio::posix::stream_descriptor input;
  bool entered=false,completed=false;
  unsigned completions=0;
  explicit Control(asio::io_context& io):input(io){}
};
asio::awaitable<void> read_frame(std::shared_ptr<Control> owner) {
  std::array<unsigned char,4> prefix{};
  owner->entered=true;
  boost::system::error_code error;
  co_await asio::async_read(owner->input,asio::buffer(prefix),asio::redirect_error(asio::use_awaitable,error));
  if(error!=asio::error::operation_aborted)throw std::runtime_error("Expected cancellation");
}
asio::awaitable<void> named_session(std::shared_ptr<Control> owner){co_await read_frame(owner);}
}

void clean_pending_read_cancellation() {
  asio::io_context io;
  int pipes[2]={-1,-1};
  if(::pipe(pipes)!=0)throw std::runtime_error("pipe failed");
  Fd read_fd(pipes[0]),write_fd(pipes[1]);
  auto owner=std::make_shared<Control>(io);
  owner->input.assign(read_fd.value);
  read_fd.value=-1;
  asio::co_spawn(io,named_session(owner),[owner](std::exception_ptr error){
    if(error)std::rethrow_exception(error);
    owner->completed=true;++owner->completions;
  });
  asio::post(io,[owner]{
    if(!owner->entered)throw std::runtime_error("Cancellation before pending read");
    owner->input.cancel();
  });
  io.run();
  if(!owner->completed||owner->completions!=1)throw std::runtime_error("Completion must drain once");
}
