#include "uos/harmony/conversation.hpp"

#include "uos/rt/json.hpp"

#include <unordered_set>
#include <utility>

namespace uos::harmony::conversation {
  namespace {
    using types::AssistantTurn;
    using types::ConversationTurn;
    using types::ToolCall;
    using types::ToolResultTurn;

    std::vector<ToolCall> foldToolCalls(const std::vector<ConversationTurn>& turns) {
      std::vector<ToolCall> calls;
      for (const ConversationTurn& turn : turns) {
        if (const AssistantTurn* assistant = std::get_if<AssistantTurn>(&turn)) calls.insert(calls.end(), assistant->toolCalls.begin(), assistant->toolCalls.end());
      }
      return calls;
    }

    // `{ role: turn.role, content: turn.content }`, or null for empty content.
    std::optional<json::object> roleContentMessage(std::string_view role, const std::string& content) {
      if (content.empty()) return std::nullopt;
      json::object message;
      message["role"] = role;
      message["content"] = content;
      return message;
    }

    /** Renders one conversation turn as its wire message, or null when it has none. */
    std::optional<json::object> wireMessageFromTurn(const ConversationTurn& turn) {
      if (const auto* system = std::get_if<types::ConversationTurnSystem>(&turn)) return roleContentMessage(system->role, system->content);
      if (const auto* developer = std::get_if<types::ConversationTurnDeveloper>(&turn)) return roleContentMessage(developer->role, developer->content);
      if (const auto* user = std::get_if<types::ConversationTurnUser>(&turn)) return roleContentMessage(user->role, user->content);
      if (const auto* assistant = std::get_if<AssistantTurn>(&turn)) {
        const bool hasCalls = !assistant->toolCalls.empty();
        if (!assistant->content && !hasCalls) return std::nullopt;
        json::object message;
        message["role"] = "assistant";
        message["content"] = assistant->content ? json::value(*assistant->content) : (hasCalls ? json::value(nullptr) : json::value(""));
        if (hasCalls) {
          json::array toolCalls;
          for (const ToolCall& call : assistant->toolCalls) {
            json::object function;
            function["name"] = call.name;
            function["arguments"] = call.arguments;
            json::object entry;
            entry["id"] = call.id;
            entry["type"] = "function";
            entry["function"] = std::move(function);
            toolCalls.push_back(std::move(entry));
          }
          message["tool_calls"] = std::move(toolCalls);
        }
        return message;
      }
      const ToolResultTurn& tool = std::get<ToolResultTurn>(turn);
      json::object message;
      message["role"] = "tool";
      message["tool_call_id"] = tool.toolCallId;
      message["content"] = tool.content;
      return message;
    }
  }

  Conversation createConversation() { return Conversation{.turns = {}}; }

  Conversation createConversation(std::vector<ConversationTurn> turns) { return Conversation{.turns = std::move(turns)}; }

  Conversation appendTurn(const Conversation& conversation, ConversationTurn turn) {
    Conversation next{.turns = conversation.turns};
    next.turns.push_back(std::move(turn));
    return next;
  }

  Conversation appendUser(const Conversation& conversation, std::string content) {
    return appendTurn(conversation, types::ConversationTurnUser{.content = std::move(content)});
  }

  Conversation appendToolResult(const Conversation& conversation, std::string toolCallId, std::string name, std::string content) {
    ToolResultTurn result{.toolCallId = std::move(toolCallId), .name = std::move(name), .content = std::move(content)};
    return appendTurn(conversation, std::move(result));
  }

  Conversation advanceConversation(const Conversation& conversation, const types::NormalizedAssistantResponse& response) {
    AssistantTurn turn{
      .content = response.content,
      .analysis = response.analysis,
      .toolCalls = response.toolCalls,
      .finishReason = response.finishReason,
    };
    return appendTurn(conversation, std::move(turn));
  }

  Conversation dropAnalysisBeforeCompletedFinal(const Conversation& conversation) {
    std::int64_t lastCompletedIndex = -1;
    for (std::size_t i = 0; i < conversation.turns.size(); i += 1) {
      const AssistantTurn* turn = std::get_if<AssistantTurn>(&conversation.turns[i]);
      if (turn && turn->toolCalls.empty() && turn->content) {
        lastCompletedIndex = static_cast<std::int64_t>(i);
      }
    }
    if (lastCompletedIndex == -1) return conversation;
    Conversation next;
    for (std::size_t index = 0; index < conversation.turns.size(); index += 1) {
      const ConversationTurn& turn = conversation.turns[index];
      const AssistantTurn* assistant = std::get_if<AssistantTurn>(&turn);
      if (assistant && !assistant->analysis.empty() && static_cast<std::int64_t>(index) <= lastCompletedIndex) {
        AssistantTurn dropped = *assistant;
        dropped.analysis = {};
        next.turns.push_back(std::move(dropped));
        continue;
      }
      next.turns.push_back(turn);
    }
    return next;
  }

  std::vector<json::object> wireMessagesFromConversation(const Conversation& conversation) {
    std::vector<json::object> messages;
    for (const ConversationTurn& turn : conversation.turns) {
      std::optional<json::object> message = wireMessageFromTurn(turn);
      if (message) messages.push_back(std::move(*message));
    }
    return messages;
  }

  std::int64_t analysisLineCount(const Conversation& conversation) {
    std::int64_t count = 0;
    for (const ConversationTurn& turn : conversation.turns) {
      if (const AssistantTurn* assistant = std::get_if<AssistantTurn>(&turn)) count += static_cast<std::int64_t>(assistant->analysis.size());
    }
    return count;
  }

  std::int64_t pendingToolCallCount(const Conversation& conversation) {
    const std::vector<ToolCall> calls = foldToolCalls(conversation.turns);
    std::unordered_set<std::string> results;
    for (const ConversationTurn& turn : conversation.turns) {
      if (const ToolResultTurn* tool = std::get_if<ToolResultTurn>(&turn)) results.insert(tool->toolCallId);
    }
    std::int64_t pending = 0;
    for (const ToolCall& call : calls) {
      if (!results.contains(call.id)) pending += 1;
    }
    return pending;
  }

  bool hasCompletedFinal(const Conversation& conversation) {
    for (const ConversationTurn& turn : conversation.turns) {
      const AssistantTurn* assistant = std::get_if<AssistantTurn>(&turn);
      if (assistant && assistant->toolCalls.empty() && assistant->content) return true;
    }
    return false;
  }
}
