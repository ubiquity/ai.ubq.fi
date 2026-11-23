// RegExp.prototype and String.prototype regex methods over rt::Regex programs, following the
// ECMAScript algorithms (RegExpBuiltinExec, @@replace, @@split, @@matchAll, @@search).
#include "uos/rt/regex.hpp"

#include "regex_internal.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/string.hpp"

#include <boost/container/small_vector.hpp>

#include <algorithm>
#include <stdexcept>

namespace uos::rt {
  namespace {
    using regex_detail::kNoPos;
    using regex_detail::Matcher;
    using regex_detail::Program;

    // Appends caller-provided WTF-8 text, keeping the result canonical.
    void appendText(std::string& out, std::string_view text) {
      if (text.find('\xED') == std::string_view::npos) {
        out.append(text);
        return;
      }
      for (std::uint32_t ch : codePoints(text)) appendCodePoint(out, ch);
    }

    // A subject prepared for matching: the bytes themselves when they are ASCII, otherwise its
    // UTF-16 code units plus a checkpoint every 64 units for mapping indices back to bytes.
    class Subject {
    public:
      Subject(std::string_view bytes, const Program& program) : bytes_(bytes), ascii_(checkAscii(bytes, program)) {
        if (ascii_) {
          length_ = bytes.size();
          return;
        }
        const auto points = codePoints(bytes);
        for (auto it = points.begin(); it != points.end(); ++it) {
          const std::uint32_t ch = *it;
          const std::size_t index = units_.size();
          const std::size_t width = ch >= 0x10000 ? 2 : 1;
          if (checkpoints_.size() * 64 < index + width) {
            checkpoints_.push_back({index, static_cast<std::size_t>(it.text().data() - bytes.data())});
          }
          units_.push_back(leadCodeUnit(ch));
          if (width == 2) units_.push_back(static_cast<std::uint16_t>(0xDC00 + ((ch - 0x10000) & 0x3FF)));
        }
        length_ = units_.size();
      }

      std::size_t length() const { return length_; }

      bool search(Matcher& matcher, std::size_t start, bool splitting = false) const {
        if (ascii_) return matcher.search(bytes_.empty() ? "" : bytes_.data(), length_, start, false, splitting);
        return matcher.search(units_.data(), length_, start, true, splitting);
      }

      // Byte offset of a UTF-16 index (rounded down inside a pair). Callers mostly ask in
      // increasing order, so the walk resumes from the previous answer when that is closer.
      std::size_t byteOffset(std::size_t index) const {
        if (ascii_) return index;
        if (index >= length_) return bytes_.size();
        const Checkpoint& checkpoint = checkpoints_[index / 64];
        std::size_t unit = checkpoint.index, byte = checkpoint.byte;
        if (cursorUnit_ <= index && cursorUnit_ > unit) unit = cursorUnit_, byte = cursorByte_;
        for (auto it = codePoints(bytes_.substr(byte)).begin(); unit < index; ++it) {
          const std::size_t width = *it >= 0x10000 ? 2 : 1;
          if (unit + width > index) break;
          unit += width;
          byte += it.text().size();
        }
        cursorUnit_ = unit, cursorByte_ = byte;
        return byte;
      }

      void append(std::string& out, std::size_t start, std::size_t end) const {
        if (start >= end) return;
        if (ascii_) {
          out.append(bytes_.data() + start, end - start);
          return;
        }
        for (std::size_t i = start; i < end; ++i) appendCodePoint(out, units_[i]);
      }

      std::string slice(std::size_t start, std::size_t end) const {
        std::string out;
        append(out, start, end);
        return out;
      }

      // AdvanceStringIndex.
      std::size_t advance(std::size_t index, bool unicode) const {
        if (!unicode || ascii_ || index + 1 >= length_) return index + 1;
        return regex_detail::isHighSurrogate(units_[index]) && regex_detail::isLowSurrogate(units_[index + 1]) ? index + 2 : index + 1;
      }

    private:
      static bool checkAscii(std::string_view bytes, const Program& program) {
        if (bytes.size() > program.subjectBytes) throw RangeError("regular expression subject is too large");
        if (program.sseFraming && bytes.size() > limits::kSseLineBytes) {
          for (auto remaining = bytes; !remaining.empty();) {
            const auto end = remaining.find_first_of("\r\n");
            if (std::min(end, remaining.size()) > limits::kSseLineBytes) throw RangeError("SSE line exceeds limit");
            if (end == std::string_view::npos) break;
            remaining.remove_prefix(end + 1);
          }
        }
        return isAscii(bytes);
      }

      struct Checkpoint {
        std::size_t index;
        std::size_t byte;
      };
      std::string_view bytes_;
      bool ascii_;
      std::size_t length_ = 0;
      boost::container::small_vector<std::uint16_t, 256> units_;
      boost::container::small_vector<Checkpoint, 8> checkpoints_;
      mutable std::size_t cursorUnit_ = 0;
      mutable std::size_t cursorByte_ = 0;
    };

    // RegExpBuiltinExec with lastIndex passed explicitly; updates it as JS does for g and y.
    bool builtinExec(const Program& program, Matcher& matcher, const Subject& subject, std::size_t& lastIndex) {
      bool stateful = program.global || program.sticky;
      std::size_t start = stateful ? lastIndex : 0;
      if (start > subject.length() || !subject.search(matcher, start)) {
        if (stateful) lastIndex = 0;
        return false;
      }
      if (stateful) lastIndex = matcher.captureEnd(0);
      return true;
    }

    std::optional<RegexSpan> spanOf(const Subject& subject, const std::size_t* caps, std::uint32_t group) {
      std::size_t start = caps[2 * group];
      if (start == kNoPos) return std::nullopt;
      std::size_t end = caps[2 * group + 1];
      return RegexSpan{start, end, subject.byteOffset(start), subject.byteOffset(end)};
    }

    RegexMatch buildMatch(const Program& program, const std::size_t* caps, const Subject& subject, std::string_view input) {
      RegexMatch match;
      std::uint32_t count = program.captureCount + 1;
      match.captures.resize(count);
      match.spans.resize(count);
      for (std::uint32_t g = 0; g < count; ++g) {
        if (caps[2 * g] == kNoPos) continue;
        match.captures[g] = subject.slice(caps[2 * g], caps[2 * g + 1]);
        match.spans[g] = spanOf(subject, caps, g);
      }
      match.index = match.spans[0]->start;
      match.byteIndex = match.spans[0]->byteStart;
      match.groups.reserve(program.names.size());
      for (const regex_detail::NamedGroup& named : program.names) {
        RegexGroup group{named.name, std::nullopt, std::nullopt};
        for (std::uint32_t g : named.groups) {
          if (caps[2 * g] == kNoPos) continue;
          group.value = match.captures[g];
          group.span = match.spans[g];
          break;
        }
        match.groups.push_back(std::move(group));
      }
      match.input = input;
      return match;
    }

    std::optional<RegexMatch> execWith(const Program& program, std::string_view input, std::size_t& lastIndex) {
      Subject subject(input, program);
      Matcher matcher(program, input.size());
      if (!builtinExec(program, matcher, subject, lastIndex)) return std::nullopt;
      return buildMatch(program, matcher.captures(), subject, input);
    }

    bool testWith(const Program& program, std::string_view input, std::size_t& lastIndex) {
      Subject subject(input, program);
      Matcher matcher(program, input.size());
      return builtinExec(program, matcher, subject, lastIndex);
    }

    // GetSubstitution for a replacement template.
    void appendSubstitution(std::string& out, std::string_view replacement, const Program& program, const Subject& subject, const std::size_t* caps) {
      std::size_t position = caps[0], end = caps[1];
      std::size_t i = 0;
      while (i < replacement.size()) {
        std::size_t dollar = replacement.find('$', i);
        if (dollar == std::string_view::npos) {
          appendText(out, replacement.substr(i));
          return;
        }
        appendText(out, replacement.substr(i, dollar - i));
        i = dollar;
        char next = i + 1 < replacement.size() ? replacement[i + 1] : '\0';
        if (next == '$') {
          out += '$';
          i += 2;
        } else if (next == '&') {
          subject.append(out, position, end);
          i += 2;
        } else if (next == '`') {
          subject.append(out, 0, position);
          i += 2;
        } else if (next == '\'') {
          subject.append(out, std::min(end, subject.length()), subject.length());
          i += 2;
        } else if (next >= '0' && next <= '9') {
          std::size_t digits = i + 2 < replacement.size() && replacement[i + 2] >= '0' && replacement[i + 2] <= '9' ? 2 : 1;
          std::size_t index = static_cast<std::size_t>(next - '0');
          if (digits == 2) index = index * 10 + static_cast<std::size_t>(replacement[i + 2] - '0');
          if (index > program.captureCount && digits == 2) {
            digits = 1;
            index = static_cast<std::size_t>(next - '0');
          }
          if (index >= 1 && index <= program.captureCount) {
            if (caps[2 * index] != kNoPos) subject.append(out, caps[2 * index], caps[2 * index + 1]);
          } else {
            out.append(replacement.substr(i, 1 + digits));
          }
          i += 1 + digits;
        } else if (next == '<') {
          std::size_t close = replacement.find('>', i + 2);
          if (close == std::string_view::npos || program.names.empty()) {
            out += "$<";
            i += 2;
            continue;
          }
          std::string_view name = replacement.substr(i + 2, close - i - 2);
          for (const regex_detail::NamedGroup& named : program.names) {
            if (named.name != name) continue;
            for (std::uint32_t g : named.groups) {
              if (caps[2 * g] == kNoPos) continue;
              subject.append(out, caps[2 * g], caps[2 * g + 1]);
              break;
            }
            break;
          }
          i = close + 1;
        } else {
          out += '$';
          i += 1;
        }
      }
    }

    // These literal substitutions translate the base64 alphabet without asking
    // the VM to match or retain captures across an encoded storage envelope.
    std::optional<std::string> replaceBase64Alphabet(const Program& program, std::string_view input, std::string_view replacement) {
      if (program.flags != "g" || input.size() <= limits::kRegexSubjectBytes) return std::nullopt;
      char from;
      if (program.source == "-" && replacement == "+") from = '-';
      else if (program.source == "_" && replacement == "/") from = '_';
      else if (program.source == R"(\+)" && replacement == "-") from = '+';
      else if (program.source == R"(\/)" && replacement == "_") from = '/';
      else return std::nullopt;

      if (input.size() > limits::kBase64InputBytes || !isAscii(input)) throw RangeError("regular expression subject is too large");
      std::string out(input);
      std::ranges::replace(out, from, replacement.front());
      return out;
    }

    // RegExp.prototype[@@replace]: all matches are found first, then callbacks run in order.
    std::string replaceWith(const Program& program, std::string_view input, std::size_t& lastIndex, std::string_view replacement, const RegexReplacer* replacer) {
      if (!replacer) {
        if (auto translated = replaceBase64Alphabet(program, input, replacement)) {
          lastIndex = 0;
          return std::move(*translated);
        }
      }
      Subject subject(input, program);
      Matcher matcher(program, input.size());
      std::size_t width = 2 * (static_cast<std::size_t>(program.captureCount) + 1);
      std::vector<std::size_t> found;
      if (program.global) {
        lastIndex = 0;
        while (builtinExec(program, matcher, subject, lastIndex)) {
          const std::size_t* caps = matcher.captures();
          found.insert(found.end(), caps, caps + width);
          if (caps[0] == caps[1]) lastIndex = subject.advance(lastIndex, program.fullUnicode());
        }
      } else if (builtinExec(program, matcher, subject, lastIndex)) {
        found.assign(matcher.captures(), matcher.captures() + width);
      }
      if (found.empty()) return std::string(input);
      std::string out;
      out.reserve(input.size());
      std::size_t nextSource = 0;
      for (std::size_t offset = 0; offset < found.size(); offset += width) {
        const std::size_t* caps = found.data() + offset;
        std::string computed;
        if (replacer) computed = (*replacer)(buildMatch(program, caps, subject, input));
        if (caps[0] < nextSource) continue;
        subject.append(out, nextSource, caps[0]);
        if (replacer) appendText(out, computed);
        else appendSubstitution(out, replacement, program, subject, caps);
        nextSource = caps[1];
      }
      subject.append(out, nextSource, subject.length());
      return out;
    }

    std::optional<std::vector<std::string>> matchGlobalWith(const Program& program, std::string_view input, std::size_t& lastIndex) {
      if (!program.global) throw std::logic_error("rt::matchGlobal: the regex has no g flag; use rt::match");
      Subject subject(input, program);
      Matcher matcher(program, input.size());
      std::vector<std::string> out;
      lastIndex = 0;
      while (builtinExec(program, matcher, subject, lastIndex)) {
        out.push_back(subject.slice(matcher.captureStart(0), matcher.captureEnd(0)));
        if (matcher.captureStart(0) == matcher.captureEnd(0)) lastIndex = subject.advance(lastIndex, program.fullUnicode());
      }
      if (out.empty()) return std::nullopt;
      return out;
    }

    // RegExp.prototype[@@split]. Repeated sticky attempts at q, q+1, ... are one forward search.
    template <class Piece, class OnCaptures> std::vector<Piece> splitWith(const Program& program, std::string_view input, std::uint32_t limit, OnCaptures&& onCaptures) {
      std::vector<Piece> out;
      if (limit == 0) return out;
      Subject subject(input, program);
      Matcher matcher(program, input.size());
      std::size_t size = subject.length();
      if (size == 0) {
        if (!subject.search(matcher, 0, true)) out.push_back(Piece(std::string(input)));
        return out;
      }
      std::size_t p = 0, q = 0;
      while (q < size) {
        if (!subject.search(matcher, q, true)) break;
        std::size_t start = matcher.captureStart(0), end = matcher.captureEnd(0);
        if (start >= size) break;
        if (end == p) {
          q = subject.advance(start, program.fullUnicode());
          continue;
        }
        out.push_back(Piece(subject.slice(p, start)));
        if (out.size() == limit) return out;
        p = end;
        if (onCaptures(out, matcher, subject, limit)) return out;
        q = p;
      }
      out.push_back(Piece(subject.slice(p, size)));
      return out;
    }
  }

  const std::optional<std::string>& RegexMatch::operator[](std::size_t n) const {
    static const std::optional<std::string> undefined;
    return n < captures.size() ? captures[n] : undefined;
  }

  const std::optional<std::string>& RegexMatch::group(std::string_view name) const {
    static const std::optional<std::string> undefined;
    for (const RegexGroup& entry : groups)
      if (entry.name == name) return entry.value;
    return undefined;
  }

  std::optional<RegexSpan> RegexMatch::groupSpan(std::string_view name) const {
    for (const RegexGroup& entry : groups)
      if (entry.name == name) return entry.span;
    return std::nullopt;
  }

  Regex::Regex(std::string_view pattern, std::string_view flags) : program_(regex_detail::compile(pattern, flags)) {}

  const std::string& Regex::source() const { return program_->source; }
  const std::string& Regex::flags() const { return program_->flags; }
  std::string Regex::toString() const { return "/" + program_->source + "/" + program_->flags; }
  bool Regex::hasIndices() const { return program_->hasIndices; }
  bool Regex::global() const { return program_->global; }
  bool Regex::ignoreCase() const { return program_->ignoreCase; }
  bool Regex::multiline() const { return program_->multiline; }
  bool Regex::dotAll() const { return program_->dotAll; }
  bool Regex::unicode() const { return program_->unicode; }
  bool Regex::unicodeSets() const { return program_->unicodeSets; }
  bool Regex::sticky() const { return program_->sticky; }
  std::size_t Regex::captureCount() const { return program_->captureCount; }

  bool Regex::test(std::string_view subject) { return testWith(*program_, subject, lastIndex); }
  bool Regex::test(std::string_view subject) const {
    std::size_t index = lastIndex;
    return testWith(*program_, subject, index);
  }
  std::optional<RegexMatch> Regex::exec(std::string_view subject) { return execWith(*program_, subject, lastIndex); }
  std::optional<RegexMatch> Regex::exec(std::string_view subject) const {
    std::size_t index = lastIndex;
    return execWith(*program_, subject, index);
  }

  std::optional<RegexMatch> match(std::string_view subject, Regex& re) {
    if (re.global()) throw std::logic_error("rt::match: the regex has the g flag; use rt::matchGlobal");
    return re.exec(subject);
  }
  std::optional<RegexMatch> match(std::string_view subject, const Regex& re) {
    if (re.global()) throw std::logic_error("rt::match: the regex has the g flag; use rt::matchGlobal");
    return re.exec(subject);
  }

  std::optional<std::vector<std::string>> matchGlobal(std::string_view subject, Regex& re) { return matchGlobalWith(re.program(), subject, re.lastIndex); }
  std::optional<std::vector<std::string>> matchGlobal(std::string_view subject, const Regex& re) {
    std::size_t index = re.lastIndex;
    return matchGlobalWith(re.program(), subject, index);
  }

  std::vector<RegexMatch> matchAll(std::string_view input, const Regex& re) {
    const Program& program = re.program();
    if (!program.global) throw TypeError("String.prototype.matchAll argument must not be a non-global regular expression");
    Subject subject(input, program);
    Matcher matcher(program, input.size());
    std::vector<RegexMatch> out;
    std::size_t lastIndex = re.lastIndex;
    while (builtinExec(program, matcher, subject, lastIndex)) {
      out.push_back(buildMatch(program, matcher.captures(), subject, input));
      if (matcher.captureStart(0) == matcher.captureEnd(0)) lastIndex = subject.advance(lastIndex, program.fullUnicode());
    }
    return out;
  }

  std::string replace(std::string_view subject, Regex& re, std::string_view replacement) { return replaceWith(re.program(), subject, re.lastIndex, replacement, nullptr); }
  std::string replace(std::string_view subject, const Regex& re, std::string_view replacement) {
    std::size_t index = re.lastIndex;
    return replaceWith(re.program(), subject, index, replacement, nullptr);
  }
  std::string replace(std::string_view subject, Regex& re, RegexReplacer replacer) { return replaceWith(re.program(), subject, re.lastIndex, {}, &replacer); }
  std::string replace(std::string_view subject, const Regex& re, RegexReplacer replacer) {
    std::size_t index = re.lastIndex;
    return replaceWith(re.program(), subject, index, {}, &replacer);
  }

  namespace {
    void requireGlobalForReplaceAll(const Regex& re) {
      if (!re.global()) throw TypeError("String.prototype.replaceAll argument must not be a non-global regular expression");
    }
  }

  std::string replaceAll(std::string_view subject, Regex& re, std::string_view replacement) {
    requireGlobalForReplaceAll(re);
    return replace(subject, re, replacement);
  }
  std::string replaceAll(std::string_view subject, const Regex& re, std::string_view replacement) {
    requireGlobalForReplaceAll(re);
    return replace(subject, re, replacement);
  }
  std::string replaceAll(std::string_view subject, Regex& re, RegexReplacer replacer) {
    requireGlobalForReplaceAll(re);
    return replace(subject, re, replacer);
  }
  std::string replaceAll(std::string_view subject, const Regex& re, RegexReplacer replacer) {
    requireGlobalForReplaceAll(re);
    return replace(subject, re, replacer);
  }

  std::int64_t search(std::string_view input, const Regex& re) {
    const Program& program = re.program();
    Subject subject(input, program);
    Matcher matcher(program, input.size());
    std::size_t lastIndex = 0;
    if (!builtinExec(program, matcher, subject, lastIndex)) return -1;
    return static_cast<std::int64_t>(matcher.captureStart(0));
  }

  std::vector<std::string> split(std::string_view subject, const Regex& re, std::uint32_t limit) {
    if (re.captureCount() != 0) throw std::logic_error("rt::split: the regex has capture groups; use rt::splitWithCaptures");
    return splitWith<std::string>(re.program(), subject, limit, [](std::vector<std::string>&, const Matcher&, const Subject&, std::uint32_t) { return false; });
  }

  std::vector<std::optional<std::string>> splitWithCaptures(std::string_view subject, const Regex& re, std::uint32_t limit) {
    const Program& program = re.program();
    return splitWith<std::optional<std::string>>(program, subject, limit, [&program](std::vector<std::optional<std::string>>& out, const Matcher& matcher, const Subject& text, std::uint32_t max) {
      for (std::uint32_t g = 1; g <= program.captureCount; ++g) {
        if (matcher.captureStart(g) == kNoPos) out.emplace_back(std::nullopt);
        else out.emplace_back(text.slice(matcher.captureStart(g), matcher.captureEnd(g)));
        if (out.size() == max) return true;
      }
      return false;
    });
  }
}
