#pragma once
// The WHATWG ReadableStream subset the gateway uses: default controller with
// start/pull/cancel and highWaterMark, default reader (read/cancel/releaseLock/
// closed), stream.cancel, `locked`, tee() and async iteration. Generic over the
// chunk type; byte streams are ReadableStream<std::vector<std::uint8_t>>.
// Methods that return a promise in JS do their synchronous part when called
// and return a Task for the rest, so `auto read = reader.read();` registers the
// read request at once, like JS.
#include "uos/rt/async.hpp"
#include <cstdint>
#include <deque>
#include <exception>
#include <functional>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt {
  template <class T> class ReadableStream;
  template <class T> class ReadableStreamDefaultController;
  template <class T> class ReadableStreamDefaultReader;

  // `{ done, value }` from reader.read().
  template <class T> using ReadableStreamReadResult = IteratorResult<T>;

  // The reason a tee()'d source is cancelled with once both branches are: the
  // JS array [reason1, reason2] (a non-Error value; String() joins the two).
  class TeeCancelReason : public ThrownValue {
  public:
    Reason reason1;
    Reason reason2;
    TeeCancelReason(Reason first, Reason second);
  };

  namespace detail {
    // A start/pull/cancel algorithm: any callable taking `A` and returning void
    // or Task<void>. The callable stays alive while a call it started is running.
    template <class A> class SourceAlgorithm {
    public:
      SourceAlgorithm() = default;
      template <class F>
        requires(!std::is_same_v<std::decay_t<F>, SourceAlgorithm> && std::is_invocable_v<std::decay_t<F>&, A> &&
                 (std::is_void_v<std::invoke_result_t<std::decay_t<F>&, A>> || std::is_same_v<std::invoke_result_t<std::decay_t<F>&, A>, Task<void>>))
      SourceAlgorithm(F&& callable) {
        using R = std::invoke_result_t<std::decay_t<F>&, A>;
        if constexpr (std::is_void_v<R>) sync_ = std::make_shared<const std::function<void(A)>>(std::forward<F>(callable));
        else async_ = std::make_shared<const std::function<Task<void>(A&&)>>(std::forward<F>(callable));
      }
      explicit operator bool() const noexcept { return sync_ || async_; }
      // `Promise.resolve(algorithm(argument))`; a synchronous throw rejects
      // unless `propagateSyncThrow` (the start algorithm, whose throw escapes the
      // constructor). Call it on a copy: the algorithm may clear the stream's.
      Promise<void> call(A argument, bool propagateSyncThrow = false) const {
        if (async_) return Promise<void>(runAsync(async_, std::move(argument)));
        Promise<void> result;
        if (sync_) {
          try {
            (*sync_)(std::move(argument));
          } catch (...) {
            if (propagateSyncThrow) throw;
            result.state()->reject(std::current_exception());
            return result;
          }
        }
        result.state()->fulfill();
        return result;
      }

    private:
      static Task<void> runAsync(std::shared_ptr<const std::function<Task<void>(A&&)>> function, A argument) {
        auto handling = (*function)(std::move(argument));
        co_await std::move(handling);
      }
      std::shared_ptr<const std::function<void(A)>> sync_;
      std::shared_ptr<const std::function<Task<void>(A&&)>> async_;
    };

    enum class StreamError : std::uint8_t {
      locked,           // getReader() / tee() on a locked stream
      cancelLocked,     // stream.cancel() on a locked stream
      enqueueClosed,    // controller.enqueue() after close() or on a closed/errored stream
      closeClosed,      // controller.close() after close() or on a closed/errored stream
      releasedRead,     // reader.read() after releaseLock()
      releasedCancel,   // reader.cancel() after releaseLock()
      releasingReader,  // reads pending when the lock is released
      readerReleased,   // reader.closed once the lock is released
      highWaterMark,    // invalid highWaterMark (RangeError)
    };
    // The exact error Bun raises for each misuse.
    Reason streamError(StreamError which);
    std::uint64_t nextReaderId() noexcept;

    template <class T> struct StreamState : std::enable_shared_from_this<StreamState<T>> {
      enum class Status : std::uint8_t { readable, closed, errored };
      Status status = Status::readable;
      Reason storedError;
      bool disturbed = false;
      std::deque<T> queue;
      double highWaterMark = 1;
      bool started = false;
      bool closeRequested = false;
      bool pulling = false;
      bool pullAgain = false;
      SourceAlgorithm<ReadableStreamDefaultController<T>> pullAlgorithm;
      SourceAlgorithm<Reason> cancelAlgorithm;
      // The current reader, identified by number so a dropped reader handle keeps
      // the stream locked (as in JS, where the stream keeps its reader alive).
      std::uint64_t readerId = 0;
      std::shared_ptr<PromiseState<void>> readerClosed;
      std::deque<std::shared_ptr<PromiseState<ReadableStreamReadResult<T>>>> readRequests;

      ReadableStreamDefaultController<T> controller() { return ReadableStreamDefaultController<T>(this->shared_from_this()); }
      bool locked() const noexcept { return readerId != 0; }
      bool canCloseOrEnqueue() const noexcept { return !closeRequested && status == Status::readable; }
      std::optional<double> desiredSize() const {
        if (status == Status::errored) return std::nullopt;
        if (status == Status::closed) return 0.0;
        return highWaterMark - static_cast<double>(queue.size());
      }
      bool shouldCallPull() const {
        if (!canCloseOrEnqueue() || !started) return false;
        if (locked() && !readRequests.empty()) return true;
        return *desiredSize() > 0;
      }
      void clearAlgorithms() {
        pullAlgorithm = {};
        cancelAlgorithm = {};
      }
      void callPullIfNeeded() {
        if (!shouldCallPull()) return;
        if (pulling) {
          pullAgain = true;
          return;
        }
        pulling = true;
        auto self = this->shared_from_this();
        auto algorithm = pullAlgorithm;
        auto pulled = algorithm.call(controller()).state();
        pulled->react([self, pulled] {
          if (pulled->status == PromiseCore::Status::rejected) {
            self->error(pulled->error);
            return;
          }
          self->pulling = false;
          if (self->pullAgain) {
            self->pullAgain = false;
            self->callPullIfNeeded();
          }
        });
      }
      // ReadableStreamDefaultControllerEnqueue (callers check canCloseOrEnqueue).
      void enqueue(T chunk) {
        if (locked() && !readRequests.empty()) {
          auto request = std::move(readRequests.front());
          readRequests.pop_front();
          request->fulfill(ReadableStreamReadResult<T>{false, std::move(chunk)});
        } else {
          queue.push_back(std::move(chunk));
        }
        callPullIfNeeded();
      }
      // ReadableStreamDefaultControllerClose.
      void requestClose() {
        if (!canCloseOrEnqueue()) return;
        closeRequested = true;
        if (queue.empty()) {
          clearAlgorithms();
          close();
        }
      }
      // ReadableStreamDefaultControllerError.
      void error(Reason reason) {
        if (status != Status::readable) return;
        queue.clear();
        clearAlgorithms();
        status = Status::errored;
        storedError = reason;
        if (!locked()) return;
        if (readerClosed) {
          readerClosed->handled = true;
          readerClosed->reject(reason);
        }
        auto requests = std::exchange(readRequests, {});
        for (auto& request : requests) request->reject(reason);
      }
      // ReadableStreamClose.
      void close() {
        status = Status::closed;
        if (!locked()) return;
        if (readerClosed) readerClosed->fulfill();
        auto requests = std::exchange(readRequests, {});
        for (auto& request : requests) request->fulfill(ReadableStreamReadResult<T>{true, std::nullopt});
      }
    };

    template <class T> struct ReaderState {
      std::shared_ptr<StreamState<T>> stream;
      std::uint64_t id = 0;
      Promise<void> closed;
    };

    // ReadableStreamDefaultReaderRead: settles `request` now (a queued chunk, the close, the
    // stored error) or registers it with the stream.
    template <class T> void readInto(const std::shared_ptr<ReaderState<T>>& reader, const std::shared_ptr<PromiseState<ReadableStreamReadResult<T>>>& request) {
      const auto& stream = reader->stream;
      using Status = typename StreamState<T>::Status;
      if (!stream) {
        request->reject(streamError(StreamError::releasedRead));
      } else {
        stream->disturbed = true;
        if (stream->status == Status::closed) {
          request->fulfill(ReadableStreamReadResult<T>{true, std::nullopt});
        } else if (stream->status == Status::errored) {
          request->reject(stream->storedError);
        } else if (!stream->queue.empty()) {
          T chunk = std::move(stream->queue.front());
          stream->queue.pop_front();
          if (stream->closeRequested && stream->queue.empty()) {
            stream->clearAlgorithms();
            stream->close();
          } else {
            stream->callPullIfNeeded();
          }
          request->fulfill(ReadableStreamReadResult<T>{false, std::move(chunk)});
        } else {
          stream->readRequests.push_back(request);
          stream->callPullIfNeeded();
        }
      }
    }

    template <class T> Task<ReadableStreamReadResult<T>> readFrom(const std::shared_ptr<ReaderState<T>>& reader) {
      auto request = std::make_shared<PromiseState<ReadableStreamReadResult<T>>>();
      readInto(reader, request);
      return awaitOwned(std::move(request));
    }

    // ReadableStreamCancel: `out` settles one turn after the cancel algorithm's promise.
    template <class T> void cancelStreamInto(std::shared_ptr<StreamState<T>> stream, Reason reason, const std::shared_ptr<PromiseState<void>>& out) {
      stream->disturbed = true;
      using Status = typename StreamState<T>::Status;
      if (stream->status == Status::closed) {
        out->fulfill();
        return;
      }
      if (stream->status == Status::errored) {
        out->reject(stream->storedError);
        return;
      }
      stream->close();
      stream->queue.clear();
      auto algorithm = stream->cancelAlgorithm;
      auto cancelled = algorithm.call(std::move(reason)).state();
      stream->clearAlgorithms();
      cancelled->react([cancelled, out] {
        if (cancelled->status == PromiseCore::Status::rejected) out->reject(cancelled->error);
        else out->fulfill();
      });
    }

    // ReadableStreamCancel: the returned promise settles one turn after the
    // cancel algorithm's.
    template <class T> std::shared_ptr<PromiseState<void>> cancelStream(const std::shared_ptr<StreamState<T>>& stream, Reason reason) {
      auto out = std::make_shared<PromiseState<void>>();
      cancelStreamInto(stream, std::move(reason), out);
      return out;
    }

    // ReadableStreamDefaultReaderRelease.
    template <class T> void releaseReader(ReaderState<T>& reader) {
      auto stream = reader.stream;
      if (!stream) return;
      auto released = streamError(StreamError::readerReleased);
      if (stream->status == StreamState<T>::Status::readable) {
        reader.closed.state()->handled = true;
        reader.closed.state()->reject(released);
      } else {
        reader.closed = Promise<void>();
        reader.closed.state()->handled = true;
        reader.closed.state()->reject(released);
      }
      stream->readerId = 0;
      stream->readerClosed = nullptr;
      reader.stream = nullptr;
      auto requests = std::exchange(stream->readRequests, {});
      auto releasing = streamError(StreamError::releasingReader);
      for (auto& request : requests) request->reject(releasing);
    }

    template <class T> struct TeeState;
  }

  // The `underlyingSource` argument of `new ReadableStream(...)`. Each callback
  // returns void or Task<void>; JS awaits the result the same way either way.
  template <class T> struct UnderlyingSource {
    detail::SourceAlgorithm<ReadableStreamDefaultController<T>> start;
    detail::SourceAlgorithm<ReadableStreamDefaultController<T>> pull;
    detail::SourceAlgorithm<Reason> cancel;
  };

  // The queuing strategy; chunks count 1 each (the default CountQueuingStrategy).
  struct QueuingStrategy {
    double highWaterMark = 1;
  };

  template <class T> class ReadableStreamDefaultController {
  public:
    explicit ReadableStreamDefaultController(std::shared_ptr<detail::StreamState<T>> stream_) : stream(std::move(stream_)) {}
    // `controller.desiredSize`: std::nullopt when errored, 0 when closed.
    std::optional<double> desiredSize() const { return stream->desiredSize(); }
    // `controller.enqueue(chunk)`: TypeError once close() was called or the stream is not readable.
    void enqueue(T chunk) const {
      if (!stream->canCloseOrEnqueue()) rethrow(detail::streamError(detail::StreamError::enqueueClosed));
      stream->enqueue(std::move(chunk));
    }
    // `controller.close()`: TypeError once close() was called or the stream is not readable.
    void close() const {
      if (!stream->canCloseOrEnqueue()) rethrow(detail::streamError(detail::StreamError::closeClosed));
      stream->requestClose();
    }
    // `controller.error(reason)`: no-op unless the stream is readable.
    void error(Reason reason = nullptr) const { stream->error(std::move(reason)); }

  private:
    std::shared_ptr<detail::StreamState<T>> stream;
  };

  template <class T> class ReadableStreamDefaultReader {
  public:
    // `new ReadableStreamDefaultReader(stream)` / `stream.getReader()`:
    // TypeError when the stream is locked.
    explicit ReadableStreamDefaultReader(const ReadableStream<T>& stream);
    // `reader.read()`: the read request is registered now; the Task resolves
    // one microtask after a chunk (or the close) is available.
    [[nodiscard]] Task<ReadableStreamReadResult<T>> read() const { return detail::readFrom(reader); }
    // `reader.cancel(reason)`
    [[nodiscard]] Task<void> cancel(Reason reason = nullptr) const {
      if (!reader->stream) return detail::rejected<void>(detail::streamError(detail::StreamError::releasedCancel));
      return detail::awaitOwned(detail::cancelStream(reader->stream, std::move(reason)));
    }
    // `reader.read()` held as the JS promise (C++ addition beside read(), same read request): it
    // is settled in the turn JS's read promise is, so a combinator over it costs the turns JS's
    // does. `await Promise.race([reader.read(), aborted])` is
    // `rt::race(std::vector{reader.readPromise(), aborted})` then `.wait()`, and a ready chunk wins
    // over an already-rejected `aborted` as in JS; racing the Task through rt::Promise(read())
    // settles one turn later. `await reader.read()` is `co_await reader.readPromise().wait()` too.
    [[nodiscard]] Promise<ReadableStreamReadResult<T>> readPromise() const {
      Promise<ReadableStreamReadResult<T>> request;
      detail::readInto(reader, request.state());
      return request;
    }
    // `reader.cancel(reason)` held as the JS promise (C++ addition beside cancel()).
    [[nodiscard]] Promise<void> cancelPromise(Reason reason = nullptr) const {
      if (!reader->stream) return Promise<void>::reject(detail::streamError(detail::StreamError::releasedCancel));
      Promise<void> cancelled;
      detail::cancelStreamInto(reader->stream, std::move(reason), cancelled.state());
      return cancelled;
    }
    // `reader.releaseLock()`: pending reads and `closed` reject with a TypeError.
    void releaseLock() const { detail::releaseReader(*reader); }
    // `reader.closed`
    Promise<void> closed() const { return reader->closed; }

  private:
    template <class U> friend class ReadableStream;
    std::shared_ptr<detail::ReaderState<T>> reader;
  };

  template <class T> class ReadableStream {
  public:
    // `new ReadableStream(underlyingSource, { highWaterMark })`: start runs now
    // (a synchronous throw escapes the constructor); pull runs once start
    // settled and whenever desiredSize > 0 or a read waits.
    explicit ReadableStream(UnderlyingSource<T> source = {}, QueuingStrategy strategy = {}) : state_(std::make_shared<detail::StreamState<T>>()) {
      if (!(strategy.highWaterMark >= 0)) rethrow(detail::streamError(detail::StreamError::highWaterMark));
      state_->highWaterMark = strategy.highWaterMark;
      state_->pullAlgorithm = std::move(source.pull);
      state_->cancelAlgorithm = std::move(source.cancel);
      auto started = source.start.call(state_->controller(), true).state();
      auto self = state_;
      started->react([self, started] {
        if (started->status == detail::PromiseCore::Status::rejected) {
          self->error(started->error);
          return;
        }
        self->started = true;
        self->callPullIfNeeded();
      });
    }

    // `stream.locked`
    bool locked() const noexcept { return state_->locked(); }
    // The spec's [[disturbed]] slot (no JS property): set by the first read() and by a
    // cancel; `bodyUsed` and `new Response(stream)` read it.
    bool disturbed() const noexcept { return state_->disturbed; }
    // `stream.cancel(reason)`: rejects with a TypeError when locked.
    [[nodiscard]] Task<void> cancel(Reason reason = nullptr) const {
      if (state_->locked()) return detail::rejected<void>(detail::streamError(detail::StreamError::cancelLocked));
      return detail::awaitOwned(detail::cancelStream(state_, std::move(reason)));
    }
    // `stream.cancel(reason)` held as the JS promise (C++ addition beside cancel()).
    [[nodiscard]] Promise<void> cancelPromise(Reason reason = nullptr) const {
      if (state_->locked()) return Promise<void>::reject(detail::streamError(detail::StreamError::cancelLocked));
      Promise<void> cancelled;
      detail::cancelStreamInto(state_, std::move(reason), cancelled.state());
      return cancelled;
    }
    // `stream.getReader()`: TypeError when locked.
    ReadableStreamDefaultReader<T> getReader() const { return ReadableStreamDefaultReader<T>(*this); }
    // `stream.tee()` (`const [a, b] = stream.tee()` is `auto [a, b] = stream.tee()`):
    // locks this stream; both branches see every chunk (copies of it). Once
    // both branches are cancelled the source is cancelled with a TeeCancelReason.
    std::pair<ReadableStream, ReadableStream> tee() const;
    // `for await (const chunk of stream)`: an iterator holding a reader; ending
    // the iteration early (return_(), or rt::forAwait's break) cancels the
    // stream and releases the lock.
    AsyncGenerator<T> values() const;

    friend bool operator==(const ReadableStream& left, const ReadableStream& right) noexcept { return left.state_ == right.state_; }
    const std::shared_ptr<detail::StreamState<T>>& state() const noexcept { return state_; }

  private:
    std::shared_ptr<detail::StreamState<T>> state_;
  };

  template <class T> ReadableStreamDefaultReader<T>::ReadableStreamDefaultReader(const ReadableStream<T>& stream) : reader(std::make_shared<detail::ReaderState<T>>()) {
    const auto& target = stream.state();
    if (target->locked()) rethrow(detail::streamError(detail::StreamError::locked));
    reader->stream = target;
    reader->id = detail::nextReaderId();
    target->readerId = reader->id;
    using Status = typename detail::StreamState<T>::Status;
    if (target->status == Status::readable) {
      target->readerClosed = reader->closed.state();
    } else if (target->status == Status::closed) {
      reader->closed.state()->fulfill();
    } else {
      reader->closed.state()->handled = true;
      reader->closed.state()->reject(target->storedError);
    }
  }

  namespace detail {
    // ReadableStreamDefaultTee (no chunk cloning: each branch gets a copy).
    template <class T> struct TeeState {
      std::shared_ptr<ReaderState<T>> reader;
      std::shared_ptr<StreamState<T>> source;
      bool reading = false;
      bool readAgain = false;
      bool canceled1 = false;
      bool canceled2 = false;
      Reason reason1;
      Reason reason2;
      std::weak_ptr<StreamState<T>> branch1;
      std::weak_ptr<StreamState<T>> branch2;
      std::shared_ptr<PromiseState<void>> cancelPromise = std::make_shared<PromiseState<void>>();
    };

    template <class T> void teePull(const std::shared_ptr<TeeState<T>>& tee);

    template <class T> Task<void> teeRead(std::weak_ptr<TeeState<T>> weak) {
      ReadableStreamReadResult<T> result;
      bool failed = false;
      try {
        Task<ReadableStreamReadResult<T>> reading;
        if (auto tee = weak.lock()) reading = readFrom(tee->reader);
        else co_return;
        result = co_await std::move(reading);
      } catch (...) {
        failed = true;
      }
      auto tee = weak.lock();
      if (!tee) co_return;
      if (failed) {
        tee->reading = false;
        co_return;
      }
      auto* step = &result;
      if (step->done) {
        tee->reading = false;
        if (!tee->canceled1) {
          if (auto branch = tee->branch1.lock()) branch->requestClose();
        }
        if (!tee->canceled2) {
          if (auto branch = tee->branch2.lock()) branch->requestClose();
        }
        if (!tee->canceled1 || !tee->canceled2) tee->cancelPromise->fulfill();
        co_return;
      }
      tee->readAgain = false;
      auto first = tee->canceled1 ? nullptr : tee->branch1.lock();
      auto second = tee->canceled2 ? nullptr : tee->branch2.lock();
      if (first && first->canCloseOrEnqueue()) {
        if (second) first->enqueue(*step->value);
        else first->enqueue(std::move(*step->value));
      }
      if (second && second->canCloseOrEnqueue()) second->enqueue(std::move(*step->value));
      tee->reading = false;
      if (tee->readAgain) teePull(tee);
    }

    template <class T> void teePull(const std::shared_ptr<TeeState<T>>& tee) {
      if (tee->reading) {
        tee->readAgain = true;
        return;
      }
      tee->reading = true;
      start(currentLoop(), teeRead<T>(tee));
    }

    template <class T> Task<void> teeCancel(std::shared_ptr<TeeState<T>> tee, bool first, Reason reason) {
      if (first) {
        tee->canceled1 = true;
        tee->reason1 = std::move(reason);
      } else {
        tee->canceled2 = true;
        tee->reason2 = std::move(reason);
      }
      if (tee->canceled1 && tee->canceled2) adopt(tee->cancelPromise, cancelStream(tee->source, makeReason(TeeCancelReason(tee->reason1, tee->reason2))));
      auto cancelPromise = tee->cancelPromise;
      tee.reset();
      auto cancelled = awaitSettled(cancelPromise);
      co_await std::move(cancelled);
      if (cancelPromise->status == PromiseCore::Status::rejected) rethrow(cancelPromise->error);
    }

    template <class T> Task<void> iterateStream(ReadableStreamDefaultReader<T> reader, Yield<T> yield) {
      for (;;) {
        ReadableStreamReadResult<T> result;
        Reason failure;
        try {
          result = co_await reader.read();
        } catch (...) {
          failure = std::current_exception();
        }
        auto* step = &result;
        if (failure) {
          reader.releaseLock();
          std::rethrow_exception(failure);
        }
        if (step->done) {
          reader.releaseLock();
          co_return;
        }
        bool returning = false;
        try {
          auto yielding = yield(std::move(*step->value));
          co_await std::move(yielding);
        } catch (const GeneratorReturn&) {
          returning = true;
        }
        if (returning) {
          auto cancelled = reader.cancel();
          reader.releaseLock();
          co_await std::move(cancelled);
          throw GeneratorReturn{};
        }
      }
    }
  }

  template <class T> std::pair<ReadableStream<T>, ReadableStream<T>> ReadableStream<T>::tee() const {
    auto reader = getReader();
    auto tee = std::make_shared<detail::TeeState<T>>();
    tee->reader = reader.reader;
    tee->source = state_;
    auto branch = [&tee](bool first) {
      UnderlyingSource<T> source;
      source.pull = [tee](ReadableStreamDefaultController<T>) { detail::teePull(tee); };
      source.cancel = [tee, first](Reason reason) { return detail::teeCancel(tee, first, std::move(reason)); };
      return ReadableStream<T>(std::move(source));
    };
    ReadableStream<T> branch1 = branch(true);
    ReadableStream<T> branch2 = branch(false);
    tee->branch1 = branch1.state();
    tee->branch2 = branch2.state();
    std::weak_ptr<detail::TeeState<T>> weak = tee;
    auto closed = tee->reader->closed.state();
    closed->react([closed, weak] {
      auto state = weak.lock();
      if (!state || closed->status != detail::PromiseCore::Status::rejected) return;
      if (auto first = state->branch1.lock()) first->error(closed->error);
      if (auto second = state->branch2.lock()) second->error(closed->error);
      if (!state->canceled1 || !state->canceled2) state->cancelPromise->fulfill();
    });
    return {std::move(branch1), std::move(branch2)};
  }

  template <class T> AsyncGenerator<T> ReadableStream<T>::values() const {
    auto reader = getReader();
    return AsyncGenerator<T>([reader](Yield<T> yield) { return detail::iterateStream(reader, std::move(yield)); });
  }

  using ByteChunk = std::vector<std::uint8_t>;
  extern template class ReadableStream<ByteChunk>;
  extern template class ReadableStreamDefaultReader<ByteChunk>;
  extern template class ReadableStreamDefaultController<ByteChunk>;
}
