#pragma once
#include "pilot/core.hpp"
namespace pilot {
  class Translator {
    Prepared const& request_;
    Limits const& limits_;
    json::storage_ptr storage_;
    std::string id_, message_id_, finish_reason_;
    std::int64_t created_;
    json::object strings_, calls_;
    json::array output_;
    json::value usage_;
    bool started_ = false, finished_ = false, message_open_ = false, message_done_ = false, reasoning_open_ = false, reasoning_pending_ = false;
    int next_ = 0, message_index_ = -1, reasoning_index_ = -1, text_index_ = -1, refusal_index_ = -1;
    json::array start();
    json::object envelope(std::string const& status);
    void close_reasoning(json::array& events);
    void open_reasoning(json::array& events);
    void open_message(json::array& events);
    void close_message(json::array& events);
    void announce_tool(json::object& call,json::array& events);
    void retain(std::string_view key,std::string_view delta);
    public: Translator(Prepared const& request,Limits const& limits,std::string id,json::storage_ptr storage);
    json::array open();
    json::array push(json::object const& chunk);
    json::array finish();
    json::value const& usage() const {
      return usage_;
    } std::string terminal_kind() const;
    std::string const& id() const {
      return id_;
    }
  };
  class SseReader {
    std::string pending_;
    std::size_t cap_, frames_ = 0, max_frames_;
    public: SseReader(std::size_t cap,std::size_t frames):cap_(cap),max_frames_(frames) {
    } void feed(std::string_view input);
    std::optional<std::string> next();
  };
}
