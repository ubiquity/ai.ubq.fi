#pragma once
// Internal interface shared by the rt::Regex translation units (regex*.cpp). Not public API.
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::rt::regex_detail {
  inline constexpr std::size_t kNoPos = static_cast<std::size_t>(-1);
  // A quantifier bound JavaScriptCore reads as unbounded (2^32 - 1 or any overflowing literal).
  inline constexpr std::uint32_t kInfinite = 0xFFFFFFFFu;
  inline constexpr std::uint32_t kMaxCodePoint = 0x10FFFF;

  inline bool isHighSurrogate(std::uint32_t unit) { return unit >= 0xD800 && unit <= 0xDBFF; }
  inline bool isLowSurrogate(std::uint32_t unit) { return unit >= 0xDC00 && unit <= 0xDFFF; }
  inline std::uint32_t combineSurrogates(std::uint32_t high, std::uint32_t low) { return 0x10000 + ((high - 0xD800) << 10) + (low - 0xDC00); }
  inline bool isLineTerminator(std::uint32_t c) { return c == 0x0A || c == 0x0D || c == 0x2028 || c == 0x2029; }

  // Decodes the WTF-8 sequence at bytes[i] and advances i. A lone surrogate (ED A0..BF xx) decodes
  // to itself; malformed bytes yield U+FFFD per maximal ill-formed subpart (the WHATWG rule).
  inline std::uint32_t decodeWtf8(std::string_view bytes, std::size_t& i) {
    unsigned b0 = static_cast<unsigned char>(bytes[i]);
    if (b0 < 0x80) {
      ++i;
      return b0;
    }
    unsigned need = 0, lo = 0x80, hi = 0xBF;
    std::uint32_t cp = 0;
    if (b0 >= 0xC2 && b0 <= 0xDF) {
      need = 1, cp = b0 & 0x1Fu;
    } else if (b0 >= 0xE0 && b0 <= 0xEF) {
      need = 2, cp = b0 & 0x0Fu;
      if (b0 == 0xE0) lo = 0xA0;
    } else if (b0 >= 0xF0 && b0 <= 0xF4) {
      need = 3, cp = b0 & 0x07u;
      if (b0 == 0xF0) lo = 0x90;
      if (b0 == 0xF4) hi = 0x8F;
    } else {
      ++i;
      return 0xFFFD;
    }
    std::size_t j = i + 1;
    for (unsigned k = 0; k < need; ++k, ++j) {
      unsigned b = j < bytes.size() ? static_cast<unsigned char>(bytes[j]) : 0u;
      if (j >= bytes.size() || b < lo || b > hi) {
        i = j;
        return 0xFFFD;
      }
      lo = 0x80, hi = 0xBF;
      cp = (cp << 6) | (b & 0x3Fu);
    }
    i = j;
    return cp;
  }

  // Appends a code point (or a lone surrogate) as WTF-8.
  inline void appendWtf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
      out += static_cast<char>(c);
    } else if (c < 0x800) {
      out += static_cast<char>(0xC0 | (c >> 6));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else if (c < 0x10000) {
      out += static_cast<char>(0xE0 | (c >> 12));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (c >> 18));
      out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (c & 0x3F));
    }
  }

  struct Range {
    std::uint32_t lo;
    std::uint32_t hi;  // inclusive
  };

  // Sorted, disjoint, non-adjacent inclusive code point ranges.
  class RangeSet {
  public:
    void add(std::uint32_t lo, std::uint32_t hi);
    void add(std::uint32_t c) { add(c, c); }
    void addAll(const RangeSet& other);
    bool contains(std::uint32_t c) const;
    RangeSet complement(std::uint32_t maxValue) const;
    const std::vector<Range>& ranges() const { return ranges_; }
    bool empty() const { return ranges_.empty(); }

  private:
    std::vector<Range> ranges_;
  };

  // A compiled character class: a bitmap for ASCII and sorted ranges for everything above.
  struct CharClass {
    std::uint64_t ascii[2] = {0, 0};
    std::vector<Range> ranges;
    bool contains(std::uint32_t c) const {
      if (c < 128) return ((ascii[c >> 6] >> (c & 63)) & 1) != 0;
      std::size_t lo = 0, hi = ranges.size();
      while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        if (ranges[mid].hi < c) lo = mid + 1;
        else hi = mid;
      }
      return lo < ranges.size() && ranges[lo].lo <= c;
    }
  };

  enum class Op : std::uint8_t {
    Char,           // a: code point (code unit without u)
    Class,          // a: class index
    Any,            // any character
    Dot,            // any character but a line terminator
    String,         // a: string index (case-sensitive code units)
    RepeatChar,     // sub: Char/Class/Any/Dot matcher with operand a; b: min; c: max
    AssertStart,
    AssertEnd,
    AssertLineStart,
    AssertLineEnd,
    AssertWord,
    AssertNotWord,
    Jump,           // a: target
    Fork,           // push a choice at a, continue with the next instruction
    ForkPrefer,     // push a choice at the next instruction, continue at a
    SaveOpen,       // a: group, b: register holding the open position
    SaveClose,      // a: group, b: register
    ResetCaps,      // a: first group, b: end group (exclusive)
    LoopInit,       // a: counter register
    LoopCheck,      // a: counter register, b: min, c: max, d: exit
    MarkPos,        // a: position register
    LoopTail,       // a: counter register, b: position register or -1, c: min, d: loop head
    CheckProgress,  // a: position register; fail when the iteration matched empty
    LookBegin,      // a: register, b: continuation after the matching LookEnd
    LookEnd,        // a: register
    BackRef,        // a: offset into groupLists, b: number of groups
    Fail,
    Match,
  };

  inline constexpr std::uint8_t kBackward = 1;
  inline constexpr std::uint8_t kLazy = 2;
  inline constexpr std::uint8_t kIgnoreCase = 4;
  inline constexpr std::uint8_t kNegative = 8;
  inline constexpr std::uint8_t kUnicodeWord = 16;

  struct Inst {
    Op op = Op::Fail;
    std::uint8_t flags = 0;
    Op sub = Op::Fail;
    std::int32_t a = 0;
    std::int32_t b = 0;
    std::int32_t c = 0;
    std::int32_t d = 0;
  };

  struct NamedGroup {
    std::string name;
    std::vector<std::uint32_t> groups;  // capture numbers carrying this name, ascending
  };

  struct Program {
    std::string source;  // RegExp.prototype.source
    std::string flags;   // RegExp.prototype.flags
    bool hasIndices = false;
    bool global = false;
    bool ignoreCase = false;
    bool multiline = false;
    bool dotAll = false;
    bool unicode = false;
    bool unicodeSets = false;
    bool sticky = false;
    std::uint32_t captureCount = 0;  // capture groups, not counting the whole match
    std::vector<NamedGroup> names;   // distinct group names in order of first appearance
    std::vector<Inst> code;
    std::vector<CharClass> classes;
    std::vector<std::u16string> strings;
    std::vector<std::uint32_t> groupLists;
    std::uint32_t registerCount = 0;
    // Search acceleration.
    bool anchored = false;             // every match starts at index 0 (leading ^ without m)
    bool firstFilter = false;          // firstUnits/firstHigh restrict where a match can start
    std::uint64_t firstUnits[4] = {};  // code units below 256 that can start a match
    bool firstHigh = false;            // whether a code unit >= 256 can start a match
    std::u16string prefix;             // case-sensitive literal every match starts with
    std::string asciiPrefix;           // the same when it is ASCII
  };

  // Throws rt::SyntaxError with JavaScriptCore's message for invalid patterns and flags.
  std::shared_ptr<const Program> compile(std::string_view pattern, std::string_view flags);

  // Case-insensitive canonical form (the smallest member of the equivalence class).
  std::uint32_t canonicalize(std::uint32_t c, bool unicode);

  struct Frame {
    std::uint32_t kind;
    std::uint32_t a;
    std::size_t b;
    std::size_t c;
  };

  // A vector of trivially copyable values that lives inline until it outgrows N.
  template <class T, std::size_t N> class SmallVec {
    static_assert(std::is_trivially_copyable_v<T>);

  public:
    SmallVec() = default;
    SmallVec(const SmallVec&) = delete;
    SmallVec& operator=(const SmallVec&) = delete;
    T* data() { return data_; }
    const T* data() const { return data_; }
    std::size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    T& operator[](std::size_t i) { return data_[i]; }
    const T& operator[](std::size_t i) const { return data_[i]; }
    T& back() { return data_[size_ - 1]; }
    void clear() { size_ = 0; }
    void pop_back() { --size_; }
    void push_back(const T& value) {
      if (size_ == capacity_) grow(size_ + 1);
      data_[size_++] = value;
    }
    void resize(std::size_t n, const T& value) {
      if (n > capacity_) grow(n);
      for (std::size_t i = size_; i < n; ++i) data_[i] = value;
      size_ = n;
    }
    void truncate(std::size_t n) { size_ = n; }

  private:
    void grow(std::size_t needed) {
      std::size_t capacity = capacity_ * 2;
      if (capacity < needed) capacity = needed;
      std::unique_ptr<T[]> heap(new T[capacity]);
      for (std::size_t i = 0; i < size_; ++i) heap[i] = data_[i];
      heap_ = std::move(heap);
      data_ = heap_.get();
      capacity_ = capacity;
    }
    T inline_[N];
    T* data_ = inline_;
    std::size_t size_ = 0;
    std::size_t capacity_ = N;
    std::unique_ptr<T[]> heap_;
  };

  // The backtracking machine. One Matcher serves one API call; it is not shared.
  class Matcher {
  public:
    explicit Matcher(const Program& program);
    // Finds the leftmost match starting at or after `start` (exactly at `start` when sticky).
    // Under u a start inside a surrogate pair moves back to the pair, as JavaScriptCore does.
    // CharT is char for a pure-ASCII subject and char16_t otherwise.
    template <class CharT> bool search(const CharT* input, std::size_t length, std::size_t start, bool sticky);
    std::size_t captureStart(std::uint32_t group) const { return caps_[2 * group]; }
    std::size_t captureEnd(std::uint32_t group) const { return caps_[2 * group + 1]; }
    const std::size_t* captures() const { return caps_.data(); }

  private:
    template <class CharT> bool run(const CharT* input, std::size_t length, std::size_t start);
    bool push(const Frame& frame);

    const Program& program_;
    SmallVec<std::size_t, 24> caps_;
    SmallVec<std::size_t, 24> regs_;
    SmallVec<Frame, 96> stack_;
    bool exhausted_ = false;
  };

  // Generated Unicode data (regex_tables.cpp).
  struct CaseEntry {
    std::uint32_t codePoint;
    std::uint16_t classIndex;
  };
  struct CaseTable {
    const std::uint32_t* members;     // equivalence classes, each ascending, concatenated
    const std::uint16_t* classStart;  // offsets into members, one past the last class at the end
    const CaseEntry* entries;         // every member, sorted by code point
    std::size_t entryCount;
  };
  extern const CaseTable kNonUnicodeCaseTable;  // Canonicalize without u (toUpperCase based)
  extern const CaseTable kUnicodeCaseTable;     // Canonicalize with u (simple case folding)
  struct GcRun {
    std::uint32_t start;
    std::uint8_t category;  // index into the General_Category leaf list (Lu, Ll, ... Cn)
  };
  extern const GcRun kGcRuns[];
  extern const std::size_t kGcRunCount;
  extern const std::uint32_t kGroupNameStart[];
  extern const std::size_t kGroupNameStartCount;
  extern const std::uint32_t kGroupNameContinue[];
  extern const std::size_t kGroupNameContinueCount;
  // u-mode property sets as delta-encoded toggle lists (decoded by addToggles in regex_compile.cpp).
  struct BinaryPropertyData {
    const char* name;
    const char* alias;  // nullptr when the property has no short name
    const char* toggles;
  };
  extern const BinaryPropertyData kBinaryProperties[];
  extern const std::size_t kBinaryPropertyCount;
  struct ScriptData {
    const char* name;        // long name, e.g. "Latin"
    const char* code;        // ISO 15924 code, e.g. "Latn"
    const char* alias;       // extra alias ("Qaac", "Qaai") or nullptr
    const char* scToggles;   // Script
    const char* scxToggles;  // Script_Extensions; nullptr when it equals Script
  };
  extern const ScriptData kScripts[];
  extern const std::size_t kScriptCount;
}
