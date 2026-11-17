#pragma once
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::provider::anthropic_replay {
  struct AnthropicReplayContext {
    std::string scope;
    std::string provider;
    std::string model;
  };

  struct NativeReplayEntryOriginal {
    static constexpr std::string_view type = "original";
    json::object item;
    friend bool operator==(const NativeReplayEntryOriginal&, const NativeReplayEntryOriginal&) = default;
  };
  struct NativeReplayEntryNativeAssistant {
    static constexpr std::string_view type = "native_assistant";
    json::array blocks;
    friend bool operator==(const NativeReplayEntryNativeAssistant&, const NativeReplayEntryNativeAssistant&) = default;
  };
  using NativeReplayEntry = std::variant<NativeReplayEntryOriginal, NativeReplayEntryNativeAssistant>;

  // The same object type as NativeReplayEntry's `original` alternative.
  using AnthropicChatReplayEntryOriginal = NativeReplayEntryOriginal;
  struct AnthropicChatReplayEntryNativeChatAssistant {
    static constexpr std::string_view type = "native_chat_assistant";
    json::object message;
    friend bool operator==(const AnthropicChatReplayEntryNativeChatAssistant&, const AnthropicChatReplayEntryNativeChatAssistant&) = default;
  };
  using AnthropicChatReplayEntry = std::variant<AnthropicChatReplayEntryOriginal, AnthropicChatReplayEntryNativeChatAssistant>;

  // `output` and `nativeBlocks` hold the items as received: elements that are not
  // records are rejected at run time, as in the TypeScript.
  struct ReplayPublication : AnthropicReplayContext {
    json::array output;
    json::array nativeBlocks;
  };
  struct ChatReplayPublication : AnthropicReplayContext {
    // { name: string; namespace: string | null }
    struct ToolName {
      std::string name;
      std::optional<std::string> namespace_;
    };
    json::array output;
    json::object assistant;
    std::optional<rt::OrderedMap<std::string, ToolName>> toolNames{};
    std::optional<rt::OrderedSet<std::string>> customToolNames{};
  };
  struct ReplayLimits {
    std::function<double()> now{};  // empty: Date.now
    std::optional<double> ttlMs{};
    std::optional<double> maxEntries{};
    std::optional<double> maxBytes{};
    std::optional<double> maxEntryBytes{};
  };

  /**
   * Process-local, authenticated receipts preserve native signed assistant blocks
   * behind ordinary public item IDs. Restarts/expiry fail closed to Responses.
   * No signature, raw scope, or private encoding enters the public wire or disk.
   * The object `createAnthropicReplayStore` returns; copies share one store.
   */
  class AnthropicReplayStore {
  public:
    struct State;
    explicit AnthropicReplayStore(std::shared_ptr<State> state);

    bool remember(const ReplayPublication& publication) const;
    std::optional<std::vector<NativeReplayEntry>> restore(const json::array& input, const AnthropicReplayContext& context) const;
    bool rememberChat(const ChatReplayPublication& publication) const;
    std::optional<std::vector<AnthropicChatReplayEntry>> restoreChat(const json::array& input, const AnthropicReplayContext& context) const;

  private:
    std::shared_ptr<State> state_;
  };

  AnthropicReplayStore createAnthropicReplayStore(ReplayLimits limits = {});

  bool rememberAnthropicReplay(const ReplayPublication& publication);
  std::optional<std::vector<NativeReplayEntry>> restoreAnthropicReplay(const json::array& input, const AnthropicReplayContext& context);
  bool rememberAnthropicChatReplay(const ChatReplayPublication& publication);
  std::optional<std::vector<AnthropicChatReplayEntry>> restoreAnthropicChatReplay(const json::array& input, const AnthropicReplayContext& context);
}
