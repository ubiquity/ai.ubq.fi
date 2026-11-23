// Compile JS strings and flags through the standalone QuickJS-ng regexp API.
#include "regex_internal.hpp"

#include "libregexp.h"
#include "uos/rt/error.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>

namespace uos::rt::regex_detail {
  namespace {
    struct Flag {
      char letter;
      int quickjs;
      bool Program::*accessor;
    };

    constexpr Flag flagsInOrder[] = {
      {'d', LRE_FLAG_INDICES, &Program::hasIndices}, {'g', LRE_FLAG_GLOBAL, &Program::global},
      {'i', LRE_FLAG_IGNORECASE, &Program::ignoreCase}, {'m', LRE_FLAG_MULTILINE, &Program::multiline},
      {'s', LRE_FLAG_DOTALL, &Program::dotAll}, {'u', LRE_FLAG_UNICODE, &Program::unicode},
      {'v', LRE_FLAG_UNICODE_SETS, &Program::unicodeSets}, {'y', LRE_FLAG_STICKY, &Program::sticky},
    };

    int parseFlags(Program& program, std::string_view flags) {
      int mask = 0;
      for (char ch : flags) {
        auto flag = std::ranges::find(flagsInOrder, ch, &Flag::letter);
        if (flag == std::end(flagsInOrder) || (mask & flag->quickjs)) throw SyntaxError("invalid regular expression flags");
        program.*(flag->accessor) = true;
        mask |= flag->quickjs;
      }
      if (program.unicode && program.unicodeSets) throw SyntaxError("invalid regular expression flags");
      for (const Flag& flag : flagsInOrder)
        if (mask & flag.quickjs) program.flags += flag.letter;
      return mask;
    }

    // RegExp.prototype.source is an escaped literal, kept separately from the input to libregexp.
    std::string escapeSource(std::string_view pattern, bool unicodeSets) {
      if (pattern.empty()) return "(?:)";
      std::string out;
      bool escaped = false;
      std::size_t brackets = 0;
      for (std::uint32_t ch : codePoints(pattern)) {
        if (!escaped) {
          if (ch == '[' && (brackets == 0 || unicodeSets)) ++brackets;
          else if (ch == ']' && brackets != 0) --brackets;
          else if (ch == '/' && brackets == 0) out += '\\';
        }
        switch (ch) {
          case '\n': case '\r': case 0x2028: case 0x2029:
            if (!escaped) out += '\\';
            out += ch == '\n' ? "n" : ch == '\r' ? "r" : ch == 0x2028 ? "u2028" : "u2029";
            break;
          default:
            appendCodePoint(out, ch);
        }
        escaped = !escaped && ch == '\\';
      }
      return out;
    }

    std::string libraryPattern(std::string_view pattern, bool unicode) {
      if (isAscii(pattern)) return std::string(pattern);
      if (unicode) return canonicalWtf8(pattern);
      // As in QuickJS's RegExp constructor, non-Unicode patterns use CESU-8, with
      // each UTF-16 surrogate encoded separately so atoms and quantifiers see code units.
      std::string out;
      out.reserve(pattern.size());
      for (std::uint32_t ch : codePoints(pattern)) {
        if (ch < 0x10000) {
          out += fromCharCode(static_cast<std::uint16_t>(ch));
        } else {
          out += fromCharCode(leadCodeUnit(ch));
          out += fromCharCode(static_cast<std::uint16_t>(0xDC00 + ((ch - 0x10000) & 0x3FF)));
        }
      }
      return out;
    }

    Bytecode compileBytecode(const std::string& input, std::size_t length, int flags, Limits& limits) {
      std::array<char, 128> error{};
      int size = 0;
      Bytecode bytecode(lre_compile(&size, error.data(), static_cast<int>(error.size()), input.c_str(), length, flags, &limits));
      // The compiler has stack probes and no periodic interrupt hook.
      if (limits.backstopExceeded || limits.checkBackstop()) throw RangeError("regular expression compilation wall-clock backstop exceeded");
      if (bytecode) return bytecode;
      const std::string_view message(error.data());
      if (message == "stack overflow" || message == "out of memory") throw RangeError(std::string(message));
      throw SyntaxError(std::string(message));
    }

    void readGroups(Program& program) {
      const char* name = lre_get_groupnames(program.bytecode.get());
      if (!name) return;
      for (std::uint32_t group = 1; group <= program.captureCount; ++group) {
        const std::string_view current(name);
        if (!current.empty()) {
          auto existing = std::ranges::find(program.names, current, &NamedGroup::name);
          if (existing == program.names.end()) program.names.push_back({std::string(current), {group}});
          else existing->groups.push_back(group);
        }
        name += current.size() + LRE_GROUP_NAME_TRAILER_LEN;
      }
    }

    constexpr std::string_view imageDataUrl = R"(^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$)";
    constexpr std::string_view standardBase64 = R"(^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$)";
    constexpr std::string_view credentialKey = R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1(?:[.-]\w*){0,16}(["']?)\s*(?=[:=]))re";

    std::size_t subjectBudget(Program& program, std::string_view pattern, int flags) {
      if (flags == (LRE_FLAG_IGNORECASE | LRE_FLAG_UNICODE) && (pattern == "^data:" || pattern == "^https?://" || pattern == imageDataUrl)) {
        program.workBudget = limits::kImageRegexWorkBudget;
        return limits::kImageRegexSubjectBytes;
      }
      if (flags == LRE_FLAG_GLOBAL && (pattern == R"([A-Za-z]+|\d+|[^\sA-Za-z\d])" || pattern == R"(\s+)")) {
        program.workBudget = limits::kCompactionRegexWorkBudget;
        return limits::kCompactionRegexSubjectBytes;
      }
      if (flags == 0 && pattern == standardBase64) {
        program.workBudget = limits::kBase64RegexWorkBudget;
        program.standardBase64 = true;
        return limits::kBase64InputBytes;
      }
      if (flags == 0 && (pattern == R"(\r?\n\r?\n)" || pattern == R"(\r\n\r\n|\n\n|\r\r)" || pattern == R"(\r\n|\r|\n)" || pattern == R"(\r?\n)")) {
        program.workBudget = limits::kSseRegexWorkBudget;
        program.sseFraming = true;
        return limits::kSseRegexSubjectBytes;
      }
      if (flags == (LRE_FLAG_GLOBAL | LRE_FLAG_IGNORECASE) && pattern == credentialKey) {
        program.workBudget = limits::kSupervisorRegexWorkBudget;
      }
      return limits::kRegexSubjectBytes;
    }
  }

  std::shared_ptr<const Program> compile(std::string_view pattern, std::string_view flags) {
    if (pattern.size() > limits::kRegexPatternBytes) throw RangeError("regular expression pattern is too large");
    auto program = std::make_shared<Program>();
    int mask = parseFlags(*program, flags);
    program->subjectBytes = subjectBudget(*program, pattern, mask);
    program->source = escapeSource(pattern, program->unicodeSets);
    // The image data URL validator accepts tens of MiB of encoded data. Fixed-size
    // blocks avoid a backtracking entry per byte, and the lookahead commits to the
    // maximal run before checking the end anchor. Spelling out eight atoms keeps
    // the library's loop polling cost per block low. The leading anchor allows
    // sticky execution without scanning every suffix after a failed match.
    if (mask == (LRE_FLAG_IGNORECASE | LRE_FLAG_UNICODE) && pattern == imageDataUrl) {
      program->imageDataUrl = true;
      pattern = R"(^data:([^;,]+);base64,(?=((?:(?:[a-z0-9+/][a-z0-9+/][a-z0-9+/][a-z0-9+/])"
                R"([a-z0-9+/][a-z0-9+/][a-z0-9+/][a-z0-9+/]){512})*[a-z0-9+/]{0,4095}={0,2}))(?-i:\2)$)";
      mask |= LRE_FLAG_STICKY;
    }
    auto input = libraryPattern(pattern, program->fullUnicode());
    const std::size_t length = input.size();
    // The compiler and name parser can look ahead while decoding a UTF-8 character.
    input.resize(length + 8, '\0');
    Limits budget(limits::kRegexCompileBackstop);
    budget.stackFloor = compileStackFloor(reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0)));
    program->bytecode = compileBytecode(input, length, mask, budget);
    program->captureCount = static_cast<std::uint32_t>(lre_get_capture_count(program->bytecode.get()) - 1);
    program->registerCount = static_cast<std::size_t>(lre_get_alloc_count(program->bytecode.get()));
    readGroups(*program);
    if (program->sticky) {
      program->splitBytecode = compileBytecode(input, length, mask & ~LRE_FLAG_STICKY, budget);
      program->registerCount = std::max(program->registerCount, static_cast<std::size_t>(lre_get_alloc_count(program->splitBytecode.get())));
    }
    return program;
  }
}
