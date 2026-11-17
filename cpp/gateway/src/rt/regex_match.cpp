// The rt::Regex backtracking machine. Backtracking state lives on an explicit stack (an undo
// log of capture and register writes interleaved with choice points), so input length never
// turns into C++ recursion depth.
#include "regex_internal.hpp"

#include <string_view>

namespace uos::rt::regex_detail {
  namespace {
    enum : std::uint32_t { kChoice, kRestoreCap, kRestoreReg, kLook, kRepeatGreedy, kRepeatLazy };

    // About 72 MiB of backtracking state (the stack doubles from 96 frames, so this is one of its
    // sizes). Past it a search reports no match, which is what JavaScriptCore does when its own
    // backtracking memory runs out (for /^(?:a|b)*$/ both give up between 1 and 2 million
    // characters).
    constexpr std::size_t kMaxFrames = std::size_t{96} << 15;

    template <class CharT> inline std::uint32_t unitAt(const CharT* input, std::size_t i) {
      if constexpr (sizeof(CharT) == 1) return static_cast<unsigned char>(input[i]);
      else return static_cast<std::uint32_t>(input[i]);
    }

    template <class CharT> struct Reader {
      const CharT* input;
      std::size_t length;
      bool unicode;

      // The character starting at pos. Under u a surrogate pair is one character.
      bool next(std::size_t pos, std::uint32_t& c, std::size_t& width) const {
        if (pos >= length) return false;
        c = unitAt(input, pos);
        width = 1;
        if constexpr (sizeof(CharT) == 2) {
          if (unicode && isHighSurrogate(c) && pos + 1 < length) {
            std::uint32_t low = unitAt(input, pos + 1);
            if (isLowSurrogate(low)) c = combineSurrogates(c, low), width = 2;
          }
        }
        return true;
      }
      // The character ending at pos.
      bool prev(std::size_t pos, std::uint32_t& c, std::size_t& width) const {
        if (pos == 0) return false;
        c = unitAt(input, pos - 1);
        width = 1;
        if constexpr (sizeof(CharT) == 2) {
          if (unicode && isLowSurrogate(c) && pos >= 2) {
            std::uint32_t high = unitAt(input, pos - 2);
            if (isHighSurrogate(high)) c = combineSurrogates(high, c), width = 2;
          }
        }
        return true;
      }
    };

    inline bool isWordUnit(std::uint32_t c, bool unicodeWord) {
      if (c < 128) return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
      return unicodeWord && (c == 0x017F || c == 0x212A);
    }

    inline std::uint32_t operand(std::int32_t value) { return static_cast<std::uint32_t>(value); }
  }

  Matcher::Matcher(const Program& program) : program_(program) {
    caps_.resize(2 * (static_cast<std::size_t>(program.captureCount) + 1), kNoPos);
    regs_.resize(program.registerCount, 0);
  }

  bool Matcher::push(const Frame& frame) {
    if (stack_.size() >= kMaxFrames) {
      exhausted_ = true;
      return false;
    }
    stack_.push_back(frame);
    return true;
  }

  template <class CharT> bool Matcher::search(const CharT* input, std::size_t length, std::size_t start, bool sticky) {
    const Program& program = program_;
    exhausted_ = false;
    for (std::size_t i = 0; i < caps_.size(); ++i) caps_[i] = kNoPos;
    if constexpr (sizeof(CharT) == 2) {
      if (program.unicode && start > 0 && start < length && isLowSurrogate(unitAt(input, start)) && isHighSurrogate(unitAt(input, start - 1))) --start;
    }
    if (start > length) return false;
    if (sticky) return run(input, length, start);
    if (program.anchored) return start == 0 && run(input, length, 0);
    bool codePoints = sizeof(CharT) == 2 && program.unicode;
    std::size_t pos = start;
    for (;;) {
      if (!program.prefix.empty()) {
        std::size_t found;
        if constexpr (sizeof(CharT) == 1) {
          if (program.asciiPrefix.empty()) return false;
          found = std::string_view(input, length).find(program.asciiPrefix, pos);
        } else {
          found = std::u16string_view(input, length).find(program.prefix, pos);
        }
        if (found == std::string_view::npos) return false;
        pos = found;
      } else if (program.firstFilter) {
        while (pos < length) {
          std::uint32_t unit = unitAt(input, pos);
          if (unit < 256 ? ((program.firstUnits[unit >> 6] >> (unit & 63)) & 1) != 0 : program.firstHigh) break;
          pos += codePoints && isHighSurrogate(unit) && pos + 1 < length && isLowSurrogate(unitAt(input, pos + 1)) ? 2 : 1;
        }
        if (pos >= length) return false;
      }
      if (run(input, length, pos)) return true;
      if (exhausted_ || pos >= length) return false;
      pos += codePoints && isHighSurrogate(unitAt(input, pos)) && pos + 1 < length && isLowSurrogate(unitAt(input, pos + 1)) ? 2 : 1;
    }
  }

  template <class CharT> bool Matcher::run(const CharT* input, std::size_t length, std::size_t start) {
    const Program& program = program_;
    const Inst* code = program.code.data();
    const Reader<CharT> reader{input, length, program.unicode};
    std::size_t* caps = caps_.data();
    std::size_t* regs = regs_.data();
    stack_.clear();
    std::size_t pc = 0, pos = start;

    auto matchOne = [&](const Inst& inst, std::size_t at, std::size_t& out) {
      std::uint32_t c = 0;
      std::size_t width = 0;
      bool backward = (inst.flags & kBackward) != 0;
      if (backward ? !reader.prev(at, c, width) : !reader.next(at, c, width)) return false;
      bool ok = false;
      switch (inst.sub) {
        case Op::Char: ok = c == operand(inst.a); break;
        case Op::Class: ok = program.classes[operand(inst.a)].contains(c); break;
        case Op::Any: ok = true; break;
        case Op::Dot: ok = !isLineTerminator(c); break;
        default: break;
      }
      if (!ok) return false;
      out = backward ? at - width : at + width;
      return true;
    };

    auto backrefMatch = [&](const Inst& inst) {
      std::size_t group = kNoPos;
      for (std::int32_t k = 0; k < inst.b; ++k) {
        std::uint32_t g = program.groupLists[operand(inst.a) + operand(k)];
        if (caps[2 * g] != kNoPos) {
          group = g;
          break;
        }
      }
      if (group == kNoPos) return true;
      std::size_t from = caps[2 * group], to = caps[2 * group + 1];
      std::size_t n = to - from;
      bool ignoreCase = (inst.flags & kIgnoreCase) != 0;
      bool backward = (inst.flags & kBackward) != 0;
      if (sizeof(CharT) == 1 || !program.unicode) {
        std::size_t at = backward ? pos - n : pos;
        if (backward ? pos < n : pos + n > length) return false;
        for (std::size_t k = 0; k < n; ++k) {
          std::uint32_t a = unitAt(input, from + k), b = unitAt(input, at + k);
          if (a != b && (!ignoreCase || canonicalize(a, false) != canonicalize(b, false))) return false;
        }
        pos = backward ? at : at + n;
        return true;
      }
      std::size_t at = pos;
      if (backward) {
        std::size_t characters = 0;
        for (std::size_t i = from; i < to; ++characters) {
          std::uint32_t c = 0;
          std::size_t width = 0;
          reader.next(i, c, width);
          i += width;
        }
        for (std::size_t k = 0; k < characters; ++k) {
          std::uint32_t c = 0;
          std::size_t width = 0;
          if (!reader.prev(at, c, width)) return false;
          at -= width;
        }
      }
      std::size_t i = from, j = at;
      while (i < to) {
        std::uint32_t a = 0, b = 0;
        std::size_t wa = 0, wb = 0;
        reader.next(i, a, wa);
        if (!reader.next(j, b, wb)) return false;
        if (a != b && (!ignoreCase || canonicalize(a, true) != canonicalize(b, true))) return false;
        i += wa, j += wb;
      }
      if (backward) {
        if (j != pos) return false;
        pos = at;
      } else {
        pos = j;
      }
      return true;
    };

    for (;;) {
      const Inst& inst = code[pc];
      bool backward = (inst.flags & kBackward) != 0;
      switch (inst.op) {
        case Op::Char: {
          std::uint32_t c = 0;
          std::size_t width = 0;
          if (backward ? !reader.prev(pos, c, width) : !reader.next(pos, c, width)) goto fail;
          if (c != operand(inst.a)) goto fail;
          pos = backward ? pos - width : pos + width;
          ++pc;
          continue;
        }
        case Op::Class: {
          std::uint32_t c = 0;
          std::size_t width = 0;
          if (backward ? !reader.prev(pos, c, width) : !reader.next(pos, c, width)) goto fail;
          if (!program.classes[operand(inst.a)].contains(c)) goto fail;
          pos = backward ? pos - width : pos + width;
          ++pc;
          continue;
        }
        case Op::Any:
        case Op::Dot: {
          std::uint32_t c = 0;
          std::size_t width = 0;
          if (backward ? !reader.prev(pos, c, width) : !reader.next(pos, c, width)) goto fail;
          if (inst.op == Op::Dot && isLineTerminator(c)) goto fail;
          pos = backward ? pos - width : pos + width;
          ++pc;
          continue;
        }
        case Op::String: {
          const std::u16string& text = program.strings[operand(inst.a)];
          std::size_t n = text.size();
          if (backward ? pos < n : pos + n > length) goto fail;
          std::size_t at = backward ? pos - n : pos;
          for (std::size_t k = 0; k < n; ++k)
            if (unitAt(input, at + k) != static_cast<std::uint32_t>(text[k])) goto fail;
          pos = backward ? at : at + n;
          ++pc;
          continue;
        }
        case Op::RepeatChar: {
          std::uint32_t min = operand(inst.b), max = operand(inst.c);
          std::size_t count = 0, at = pos, next = 0;
          if ((inst.flags & kLazy) == 0) {
            while (count < max && matchOne(inst, at, next)) at = next, ++count;
            if (count < min) goto fail;
            if (count > min && !push({kRepeatGreedy, static_cast<std::uint32_t>(pc), at, count})) return false;
          } else {
            while (count < min) {
              if (!matchOne(inst, at, next)) goto fail;
              at = next, ++count;
            }
            if (count < max && !push({kRepeatLazy, static_cast<std::uint32_t>(pc), at, count})) return false;
          }
          pos = at;
          ++pc;
          continue;
        }
        case Op::AssertStart:
          if (pos != 0) goto fail;
          ++pc;
          continue;
        case Op::AssertEnd:
          if (pos != length) goto fail;
          ++pc;
          continue;
        case Op::AssertLineStart:
          if (pos != 0 && !isLineTerminator(unitAt(input, pos - 1))) goto fail;
          ++pc;
          continue;
        case Op::AssertLineEnd:
          if (pos != length && !isLineTerminator(unitAt(input, pos))) goto fail;
          ++pc;
          continue;
        case Op::AssertWord:
        case Op::AssertNotWord: {
          bool unicodeWord = (inst.flags & kUnicodeWord) != 0;
          bool before = pos > 0 && isWordUnit(unitAt(input, pos - 1), unicodeWord);
          bool after = pos < length && isWordUnit(unitAt(input, pos), unicodeWord);
          if ((before != after) != (inst.op == Op::AssertWord)) goto fail;
          ++pc;
          continue;
        }
        case Op::Jump:
          pc = operand(inst.a);
          continue;
        case Op::Fork:
          if (!push({kChoice, operand(inst.a), pos, 0})) return false;
          ++pc;
          continue;
        case Op::ForkPrefer:
          if (!push({kChoice, static_cast<std::uint32_t>(pc + 1), pos, 0})) return false;
          pc = operand(inst.a);
          continue;
        case Op::SaveOpen:
          if (!push({kRestoreReg, operand(inst.b), regs[inst.b], 0})) return false;
          regs[inst.b] = pos;
          ++pc;
          continue;
        case Op::SaveClose: {
          std::size_t g = operand(inst.a);
          if (!push({kRestoreCap, operand(inst.a), caps[2 * g], caps[2 * g + 1]})) return false;
          std::size_t open = regs[inst.b];
          caps[2 * g] = backward ? pos : open;
          caps[2 * g + 1] = backward ? open : pos;
          ++pc;
          continue;
        }
        case Op::ResetCaps:
          for (std::uint32_t g = operand(inst.a); g < operand(inst.b); ++g) {
            if (caps[2 * g] == kNoPos) continue;
            if (!push({kRestoreCap, g, caps[2 * g], caps[2 * g + 1]})) return false;
            caps[2 * g] = caps[2 * g + 1] = kNoPos;
          }
          ++pc;
          continue;
        case Op::LoopInit:
          if (!push({kRestoreReg, operand(inst.a), regs[inst.a], 0})) return false;
          regs[inst.a] = 0;
          ++pc;
          continue;
        case Op::LoopCheck: {
          std::size_t count = regs[inst.a];
          std::uint32_t max = operand(inst.c);
          if (count < operand(inst.b)) {
            ++pc;
          } else if (max != kInfinite && count >= max) {
            pc = operand(inst.d);
          } else if ((inst.flags & kLazy) == 0) {
            if (!push({kChoice, operand(inst.d), pos, 0})) return false;
            ++pc;
          } else {
            if (!push({kChoice, static_cast<std::uint32_t>(pc + 1), pos, 0})) return false;
            pc = operand(inst.d);
          }
          continue;
        }
        case Op::MarkPos:
          if (!push({kRestoreReg, operand(inst.a), regs[inst.a], 0})) return false;
          regs[inst.a] = pos;
          ++pc;
          continue;
        case Op::LoopTail: {
          std::size_t count = regs[inst.a];
          if (inst.b >= 0 && count >= operand(inst.c) && pos == regs[inst.b]) goto fail;
          if (!push({kRestoreReg, operand(inst.a), count, 0})) return false;
          regs[inst.a] = count + 1;
          pc = operand(inst.d);
          continue;
        }
        case Op::CheckProgress:
          if (pos == regs[inst.a]) goto fail;
          ++pc;
          continue;
        case Op::LookBegin:
          if (!push({kRestoreReg, operand(inst.a), regs[inst.a], 0})) return false;
          regs[inst.a] = stack_.size();
          if (!push({kLook, operand(inst.b), pos, (inst.flags & kNegative) != 0 ? 1u : 0u})) return false;
          ++pc;
          continue;
        case Op::LookEnd: {
          std::size_t base = regs[inst.a];
          Frame look = stack_[base];
          if ((inst.flags & kNegative) == 0) {
            std::size_t kept = base;
            for (std::size_t r = base + 1; r < stack_.size(); ++r)
              if (stack_[r].kind == kRestoreCap || stack_[r].kind == kRestoreReg) stack_[kept++] = stack_[r];
            stack_.truncate(kept);
            pos = look.b;
            ++pc;
            continue;
          }
          while (stack_.size() > base + 1) {
            Frame frame = stack_.back();
            stack_.pop_back();
            if (frame.kind == kRestoreCap) caps[2 * frame.a] = frame.b, caps[2 * frame.a + 1] = frame.c;
            else if (frame.kind == kRestoreReg) regs[frame.a] = frame.b;
          }
          stack_.pop_back();
          goto fail;
        }
        case Op::BackRef:
          if (!backrefMatch(inst)) goto fail;
          ++pc;
          continue;
        case Op::Fail:
          goto fail;
        case Op::Match:
          caps[0] = start;
          caps[1] = pos;
          return true;
      }
    fail:
      for (;;) {
        if (stack_.empty()) return false;
        Frame& frame = stack_.back();
        if (frame.kind == kChoice) {
          pc = frame.a, pos = frame.b;
          stack_.pop_back();
          break;
        }
        if (frame.kind == kRestoreCap) {
          caps[2 * frame.a] = frame.b, caps[2 * frame.a + 1] = frame.c;
          stack_.pop_back();
          continue;
        }
        if (frame.kind == kRestoreReg) {
          regs[frame.a] = frame.b;
          stack_.pop_back();
          continue;
        }
        if (frame.kind == kLook) {
          bool negative = frame.c != 0;
          std::size_t resume = frame.a, saved = frame.b;
          stack_.pop_back();
          if (!negative) continue;
          pc = resume, pos = saved;
          break;
        }
        const Inst& repeat = code[frame.a];
        bool repeatBackward = (repeat.flags & kBackward) != 0;
        if (frame.kind == kRepeatGreedy) {
          std::uint32_t c = 0;
          std::size_t width = 0, at = frame.b;
          if (repeatBackward) reader.next(at, c, width), at += width;
          else reader.prev(at, c, width), at -= width;
          std::size_t count = frame.c - 1;
          pc = frame.a + 1, pos = at;
          if (count > operand(repeat.b)) frame.b = at, frame.c = count;
          else stack_.pop_back();
          break;
        }
        std::size_t next = 0;
        if (!matchOne(repeat, frame.b, next)) {
          stack_.pop_back();
          continue;
        }
        std::size_t count = frame.c + 1;
        pc = frame.a + 1, pos = next;
        if (count < operand(repeat.c)) frame.b = next, frame.c = count;
        else stack_.pop_back();
        break;
      }
    }
  }

  template bool Matcher::search<char>(const char*, std::size_t, std::size_t, bool);
  template bool Matcher::search<char16_t>(const char16_t*, std::size_t, std::size_t, bool);
}
