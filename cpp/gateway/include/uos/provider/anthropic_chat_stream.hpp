#pragma once
// Port of src/provider/anthropic-chat-stream.ts.
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <string>
#include <string_view>
#include <variant>

namespace uos::provider::anthropic_chat_stream {
  // ChatFrame: Readonly<{ kind: "chunk"; value }> | Readonly<{ kind: "done" }> | Readonly<{ kind: "comment"; text }>.
  struct ChatFrameChunk {
    static constexpr std::string_view kind = "chunk";
    json::object value;
  };
  struct ChatFrameDone {
    static constexpr std::string_view kind = "done";
  };
  struct ChatFrameComment {
    static constexpr std::string_view kind = "comment";
    std::string text;
  };
  using ChatFrame = std::variant<ChatFrameChunk, ChatFrameDone, ChatFrameComment>;

  rt::AsyncGenerator<ChatFrame> readAnthropicChatStream(rt::Response response, rt::AbortSignal downstream);
}
