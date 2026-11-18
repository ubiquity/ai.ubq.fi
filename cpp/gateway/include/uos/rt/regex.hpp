#pragma once
// rt::Regex: ECMAScript regular expressions using QuickJS-ng libregexp.
// Subjects and results are std::string in WTF-8 (docs/rt/text.md). Indices are reported both
// as UTF-16 code units (what JavaScript reports) and as byte offsets into the subject.
// See docs/rt/regex.md for the TypeScript-to-C++ mapping.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt {
  namespace regex_detail {
    struct Program;
  }

  // Where one capture lies: [start, end) in UTF-16 code units (JS `indices`) and in bytes.
  // A boundary inside a surrogate pair (possible without u or v) has its byte offset
  // rounded down to the start of that four-byte sequence; use the UTF-16 indices there.
  struct RegexSpan {
    std::size_t start = 0;
    std::size_t end = 0;
    std::size_t byteStart = 0;
    std::size_t byteEnd = 0;
    bool operator==(const RegexSpan&) const = default;
  };

  // One property of match.groups. With duplicate names it holds the group that participated.
  struct RegexGroup {
    std::string name;
    std::optional<std::string> value;
    std::optional<RegexSpan> span;
  };

  // The array RegExp.prototype.exec returns.
  struct RegexMatch {
    std::size_t index = 0;                             // match.index (UTF-16)
    std::size_t byteIndex = 0;                         // byte offset of match[0]
    std::vector<std::optional<std::string>> captures;  // match[0..n]; nullopt = undefined
    std::vector<std::optional<RegexSpan>> spans;       // match.indices[0..n], always filled
    std::vector<RegexGroup> groups;                    // match.groups in property order; empty = undefined
    std::string_view input;                            // match.input: a view of the subject

    std::size_t size() const { return captures.size(); }  // match.length
    // match[n]; undefined (nullopt) past the last capture, as in JS.
    const std::optional<std::string>& operator[](std::size_t n) const;
    const std::string& str() const { return *captures[0]; }       // match[0]
    std::size_t endIndex() const { return spans[0]->end; }         // match.index + match[0].length
    std::size_t byteEndIndex() const { return spans[0]->byteEnd; }
    // match.groups?.[name]; undefined when the group did not participate or does not exist.
    const std::optional<std::string>& group(std::string_view name) const;
    std::optional<RegexSpan> groupSpan(std::string_view name) const;  // match.indices.groups[name]
  };

  // A non-owning reference to a replace callback: `std::string(const RegexMatch&)`. The match
  // carries what JS passes positionally: m[0] = matched, m[1..] = p1.., m.index = offset (UTF-16),
  // m.input = string, m.groups = groups.
  class RegexReplacer {
  public:
    template <class F>
      requires(std::is_invocable_r_v<std::string, F&, const RegexMatch&> && !std::is_same_v<std::remove_cvref_t<F>, RegexReplacer>)
    RegexReplacer(F&& callable)
        : object_(const_cast<void*>(static_cast<const void*>(std::addressof(callable)))), call_(&invoke<std::remove_reference_t<F>>) {}
    std::string operator()(const RegexMatch& match) const { return call_(object_, match); }

  private:
    template <class F> static std::string invoke(void* object, const RegexMatch& match) { return std::invoke(*static_cast<F*>(object), match); }
    void* object_;
    std::string (*call_)(void*, const RegexMatch&);
  };

  class Regex {
  public:
    // new RegExp(pattern, flags). Throws rt::SyntaxError for invalid patterns or flags. The
    // immutable bytecode is shared by copies. Matching may throw rt::RangeError at a resource limit.
    explicit Regex(std::string_view pattern, std::string_view flags = "");

    // RegExp.prototype.lastIndex, in UTF-16 code units. Read and written only by the non-const
    // members and overloads, exactly where JS reads and writes it for global/sticky regexes.
    std::size_t lastIndex = 0;

    const std::string& source() const;  // RegExp.prototype.source (escaped for a regex literal)
    const std::string& flags() const;   // RegExp.prototype.flags, canonical order "dgimsuvy"
    std::string toString() const;       // String(re): "/" + source + "/" + flags
    bool hasIndices() const;
    bool global() const;
    bool ignoreCase() const;
    bool multiline() const;
    bool dotAll() const;
    bool unicode() const;
    bool unicodeSets() const;
    bool sticky() const;
    std::size_t captureCount() const;  // number of capture groups, not counting match[0]

    // re.test(s) / re.exec(s). The non-const overloads read and update lastIndex for g/y
    // regexes (a module-level `const RE = /x/g` that JS uses statefully). The const overloads
    // behave like a fresh regex literal: they start from the current lastIndex (0 unless it was
    // changed) and leave it untouched.
    bool test(std::string_view subject);
    bool test(std::string_view subject) const;
    std::optional<RegexMatch> exec(std::string_view subject);
    std::optional<RegexMatch> exec(std::string_view subject) const;

    const regex_detail::Program& program() const { return *program_; }

  private:
    std::shared_ptr<const regex_detail::Program> program_;
  };

  // String.prototype methods with a RegExp argument. Overloads taking `Regex&` update lastIndex
  // as JS does; `const Regex&` overloads leave it untouched.

  // str.match(re) for a regex without g (same as re.exec(str)). Throws std::logic_error for g.
  std::optional<RegexMatch> match(std::string_view subject, Regex& re);
  std::optional<RegexMatch> match(std::string_view subject, const Regex& re);
  // str.match(re) for a regex with g: every matched string, or nullopt (JS null) when none.
  // Throws std::logic_error when re lacks g.
  std::optional<std::vector<std::string>> matchGlobal(std::string_view subject, Regex& re);
  std::optional<std::vector<std::string>> matchGlobal(std::string_view subject, const Regex& re);
  // [...str.matchAll(re)]. Throws rt::TypeError (JSC's message) when re lacks g.
  std::vector<RegexMatch> matchAll(std::string_view subject, const Regex& re);

  // str.replace(re, replacement) with $$ $& $` $' $n $nn $<name> substitution, or with a callback.
  std::string replace(std::string_view subject, Regex& re, std::string_view replacement);
  std::string replace(std::string_view subject, const Regex& re, std::string_view replacement);
  std::string replace(std::string_view subject, Regex& re, RegexReplacer replacer);
  std::string replace(std::string_view subject, const Regex& re, RegexReplacer replacer);
  // str.replaceAll(re, ...). Throws rt::TypeError (JSC's message) when re lacks g.
  std::string replaceAll(std::string_view subject, Regex& re, std::string_view replacement);
  std::string replaceAll(std::string_view subject, const Regex& re, std::string_view replacement);
  std::string replaceAll(std::string_view subject, Regex& re, RegexReplacer replacer);
  std::string replaceAll(std::string_view subject, const Regex& re, RegexReplacer replacer);

  // str.search(re): UTF-16 index of the first match, or -1.
  std::int64_t search(std::string_view subject, const Regex& re);

  // str.split(re, limit) for a regex without capture groups (throws std::logic_error otherwise).
  std::vector<std::string> split(std::string_view subject, const Regex& re, std::uint32_t limit = 0xFFFFFFFFu);
  // str.split(re, limit) for a regex with capture groups: captures are spliced in, undefined
  // ones as nullopt.
  std::vector<std::optional<std::string>> splitWithCaptures(std::string_view subject, const Regex& re, std::uint32_t limit = 0xFFFFFFFFu);
}
