#include "uos/harmony/parse.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

namespace uos::harmony::parse {
  namespace {
    using types::HarmonyChannel;
    using types::HarmonyRawMessage;
    using types::HarmonyTurn;

    constexpr std::string_view START = "<|start|>";
    constexpr std::string_view CHANNEL = "<|channel|>";
    constexpr std::string_view MESSAGE = "<|message|>";
    constexpr std::string_view END = "<|end|>";
    constexpr std::string_view CALL = "<|call|>";
    constexpr std::string_view RETURN = "<|return|>";
    constexpr std::string_view CONSTRAIN = "<|constrain|>";

    constexpr std::array<std::string_view, 7> MARKERS{START, CHANNEL, CONSTRAIN, MESSAGE, END, CALL, RETURN};

    constexpr std::array<std::string_view, 3> ASSISTANT_CHANNELS{"analysis", "commentary", "final"};

    bool isAssistantChannel(std::string_view value) { return std::find(ASSISTANT_CHANNELS.begin(), ASSISTANT_CHANNELS.end(), value) != ASSISTANT_CHANNELS.end(); }

    std::optional<std::string> firstToken(std::string_view value) {
      static const rt::Regex FIRST_TOKEN("^\\s*([^\\s<]+)", "");
      const std::optional<rt::RegexMatch> match = FIRST_TOKEN.exec(value);
      return match ? (*match)[1] : std::nullopt;
    }

    std::optional<std::string> findRecipient(std::string_view value) {
      static const rt::Regex RECIPIENT("(?:^|\\s)to=([^\\s<]+)", "");
      const std::optional<rt::RegexMatch> match = RECIPIENT.exec(value);
      return match ? (*match)[1] : std::nullopt;
    }

    /** One header section, keyed by the marker that introduced it. */
    struct HeaderSection {
      // "start" | "channel" | "constrain"
      std::string_view kind;
      std::string text;
    };

    struct Header {
      std::string role;
      std::optional<HarmonyChannel> channel;
      std::optional<std::string> recipient;
      std::optional<std::string> constrain;
    };

    Header freshHeader() { return Header{.role = "assistant", .channel = std::nullopt, .recipient = std::nullopt, .constrain = std::nullopt}; }

    std::string sectionText(const std::vector<HeaderSection>& sections, std::string_view kind) {
      for (const HeaderSection& section : sections) {
        if (section.kind == kind) return section.text;
      }
      return "";
    }

    /** Extracts role/channel/recipient/constrain from the ordered header sections. */
    Header parseHeader(const std::vector<HeaderSection>& sections) {
      const std::string startText = sectionText(sections, "start");
      const std::string channelText = sectionText(sections, "channel");
      const std::string constrainText = sectionText(sections, "constrain");

      const std::optional<std::string> roleToken = firstToken(startText);
      std::string role = roleToken && *roleToken != "assistant" ? *roleToken : std::string("assistant");
      const std::optional<std::string> channelToken = firstToken(channelText);
      std::optional<std::string> recipient = findRecipient(startText);
      if (!recipient) recipient = findRecipient(channelText);
      return Header{
        .role = std::move(role),
        .channel = channelToken && isAssistantChannel(*channelToken) ? channelToken : std::nullopt,
        .recipient = std::move(recipient),
        .constrain = firstToken(constrainText),
      };
    }

    /**
     * Maps one assistant message to its adapter turn, or null when the message
     * carries no turn at all (no content, or a channel that never becomes one).
     */
    std::optional<HarmonyTurn> turnFromAssistantMessage(const HarmonyRawMessage& message) {
      if (rt::truthy(message.recipient)) {
        // Function calls travel on the commentary channel; built-in tools
        // (browser/python) travel on the analysis channel.  Either way the
        // recipient identifies the call.
        if (message.content.empty()) return std::nullopt;
        return types::HarmonyTurnToolCall{
          .recipient = *message.recipient,
          .name = functionNameFromRecipient(*message.recipient),
          .arguments = normalizeToolArguments(message.content),
        };
      }
      if (message.content.empty()) return std::nullopt;
      if (message.channel == "analysis") return types::HarmonyTurnReasoning{.text = message.content};
      if (message.channel == "final" || (!message.channel && message.stoppedBy == "<|return|>")) {
        return types::HarmonyTurnFinal{.text = message.content};
      }
      if (message.channel == "commentary") return types::HarmonyTurnCommentary{.text = message.content};
      return std::nullopt;
    }

    /** Scanner state for one completion: the parser's former local variables. */
    struct ScannerState {
      Header header = freshHeader();
      std::vector<HeaderSection> sections{};
      HeaderSection currentSection{.kind = "start", .text = ""};
      bool contentStarted = false;
      std::string content{};
      std::vector<HarmonyRawMessage> messages{};
      bool truncated = false;
    };

    /** Drops the pending header sections and whatever they collected. */
    void resetScannerHeader(ScannerState& state) {
      state.header = freshHeader();
      state.sections = {};
      state.currentSection = HeaderSection{.kind = "start", .text = ""};
      state.contentStarted = false;
      state.content = "";
    }

    /** Appends text to the open content or, before `<|message|>`, to the header section. */
    void appendScannerText(ScannerState& state, std::string_view text) {
      if (text.empty()) return;
      if (state.contentStarted) rt::append(state.content, text);
      else rt::append(state.currentSection.text, text);
    }

    /** Emits the pending message when it carries anything, then starts a fresh header. */
    void emitScannerMessage(ScannerState& state, std::string_view stoppedBy) {
      bool headerHasText = !rt::trim(state.currentSection.text).empty();
      for (const HeaderSection& section : state.sections) {
        if (!rt::trim(section.text).empty()) headerHasText = true;
      }
      if (!state.content.empty() || headerHasText) {
        state.messages.push_back(HarmonyRawMessage{
          .role = state.header.role,
          .channel = state.header.channel,
          .recipient = state.header.recipient,
          .constrain = state.header.constrain,
          .content = state.content,
          .stoppedBy = std::string(stoppedBy),
        });
      }
      if (stoppedBy == "truncated") state.truncated = true;
      resetScannerHeader(state);
    }

    // state.sections.concat(state.currentSection)
    std::vector<HeaderSection> sectionsWithCurrent(const ScannerState& state) {
      std::vector<HeaderSection> sections = state.sections;
      sections.push_back(state.currentSection);
      return sections;
    }

    /** Applies one special token to the scanner state. */
    void applyScannerMarker(ScannerState& state, std::string_view marker) {
      if (marker == START) {
        if (state.contentStarted && !state.content.empty()) emitScannerMessage(state, "truncated");
        resetScannerHeader(state);
        return;
      }
      if (marker == CHANNEL) {
        state.sections.push_back(state.currentSection);
        state.currentSection = HeaderSection{.kind = "channel", .text = ""};
        return;
      }
      if (marker == CONSTRAIN) {
        state.sections.push_back(state.currentSection);
        state.currentSection = HeaderSection{.kind = "constrain", .text = ""};
        return;
      }
      if (marker == END || marker == CALL || marker == RETURN) {
        state.header = parseHeader(sectionsWithCurrent(state));
        emitScannerMessage(state, marker);
        return;
      }
      if (marker == MESSAGE) {
        state.header = parseHeader(sectionsWithCurrent(state));
        state.contentStarted = true;
      }
    }

    struct FoundMarker {
      std::size_t index;
      std::string_view marker;
    };

    /** The earliest special token at or after `position`, or null when none remains. */
    // Byte offsets: every marker is ASCII, so a byte search finds the same tokens.
    std::optional<FoundMarker> nextMarkerAt(std::string_view text, std::size_t position) {
      std::optional<FoundMarker> found;
      for (std::string_view candidate : MARKERS) {
        const std::size_t index = text.find(candidate, position);
        if (index == std::string_view::npos) continue;
        if (found && index >= found->index) continue;
        found = FoundMarker{.index = index, .marker = candidate};
      }
      return found;
    }
  }

  std::string functionNameFromRecipient(std::string_view recipient) {
    constexpr std::string_view prefix = "functions.";
    return recipient.starts_with(prefix) ? std::string(recipient.substr(prefix.size())) : std::string(recipient);
  }

  std::string normalizeToolArguments(std::string_view argumentsText) {
    if (rt::trim(argumentsText).empty()) return std::string(argumentsText);
    try {
      return rt::jsonStringify(rt::jsonParse(argumentsText));
    } catch (const rt::JsError&) {
      return std::string(argumentsText);
    }
  }

  std::vector<HarmonyTurn> harmonyTurnsFromMessages(const std::vector<HarmonyRawMessage>& messages) {
    std::vector<HarmonyTurn> turns;
    for (const HarmonyRawMessage& message : messages) {
      if (message.role != "assistant") continue;
      std::optional<HarmonyTurn> turn = turnFromAssistantMessage(message);
      if (turn) turns.push_back(std::move(*turn));
    }
    return turns;
  }

  types::HarmonyParseResult parseHarmonyOutput(std::string_view text) {
    ScannerState state;

    std::size_t position = 0;
    while (position < text.size()) {
      const std::optional<FoundMarker> found = nextMarkerAt(text, position);
      if (!found) {
        appendScannerText(state, text.substr(position));
        if ((state.contentStarted && !state.content.empty()) || !rt::trim(state.currentSection.text).empty()) emitScannerMessage(state, "truncated");
        break;
      }

      appendScannerText(state, text.substr(position, found->index - position));
      applyScannerMarker(state, found->marker);
      position = found->index + found->marker.size();
    }

    if (state.messages.empty() && !rt::trim(text).empty()) {
      state.messages.push_back(HarmonyRawMessage{
        .role = "assistant",
        .channel = std::nullopt,
        .recipient = std::nullopt,
        .constrain = std::nullopt,
        .content = std::string(text),
        .stoppedBy = "truncated",
      });
      state.truncated = true;
    }

    std::vector<HarmonyTurn> turns = harmonyTurnsFromMessages(state.messages);
    return types::HarmonyParseResult{.messages = std::move(state.messages), .turns = std::move(turns), .truncated = state.truncated};
  }
}
