// Pattern parser and bytecode compiler for rt::Regex (ECMAScript syntax, including Annex B for
// patterns without u, as JavaScriptCore's YarrParser accepts it, with its error precedence).
#include "regex_internal.hpp"

#include "uos/rt/error.hpp"

#include <algorithm>
#include <limits>
#include <optional>

namespace uos::rt::regex_detail {
  void RangeSet::add(std::uint32_t lo, std::uint32_t hi) {
    if (lo > hi) return;
    if (ranges_.empty() || lo > ranges_.back().hi + 1) {
      ranges_.push_back({lo, hi});
      return;
    }
    if (lo >= ranges_.back().lo) {
      ranges_.back().hi = std::max(ranges_.back().hi, hi);
      return;
    }
    auto first = std::lower_bound(ranges_.begin(), ranges_.end(), lo, [](const Range& range, std::uint32_t value) { return range.hi + 1 < value; });
    auto last = first;
    std::uint32_t newLo = lo, newHi = hi;
    while (last != ranges_.end() && last->lo <= hi + 1) {
      newLo = std::min(newLo, last->lo);
      newHi = std::max(newHi, last->hi);
      ++last;
    }
    if (first == last) {
      ranges_.insert(first, Range{lo, hi});
      return;
    }
    *first = Range{newLo, newHi};
    ranges_.erase(first + 1, last);
  }

  void RangeSet::addAll(const RangeSet& other) {
    if (other.ranges_.empty()) return;
    if (ranges_.empty()) {
      ranges_ = other.ranges_;
      return;
    }
    std::vector<Range> merged;
    merged.reserve(ranges_.size() + other.ranges_.size());
    std::size_t i = 0, j = 0;
    auto append = [&merged](const Range& range) {
      if (!merged.empty() && range.lo <= merged.back().hi + 1) merged.back().hi = std::max(merged.back().hi, range.hi);
      else merged.push_back(range);
    };
    while (i < ranges_.size() || j < other.ranges_.size()) {
      if (j == other.ranges_.size() || (i < ranges_.size() && ranges_[i].lo <= other.ranges_[j].lo)) append(ranges_[i++]);
      else append(other.ranges_[j++]);
    }
    ranges_ = std::move(merged);
  }

  bool RangeSet::contains(std::uint32_t c) const {
    auto it = std::lower_bound(ranges_.begin(), ranges_.end(), c, [](const Range& range, std::uint32_t value) { return range.hi < value; });
    return it != ranges_.end() && it->lo <= c;
  }

  RangeSet RangeSet::complement(std::uint32_t maxValue) const {
    RangeSet out;
    std::uint32_t next = 0;
    for (const Range& range : ranges_) {
      if (range.lo > maxValue) break;
      if (range.lo > next) out.ranges_.push_back({next, range.lo - 1});
      next = range.hi + 1;
    }
    if (next <= maxValue) out.ranges_.push_back({next, maxValue});
    return out;
  }

  namespace {
    const CaseTable& caseTable(bool unicode) { return unicode ? kUnicodeCaseTable : kNonUnicodeCaseTable; }

    const CaseEntry* findCaseEntry(std::uint32_t c, bool unicode) {
      const CaseTable& table = caseTable(unicode);
      std::size_t lo = 0, hi = table.entryCount;
      while (lo < hi) {
        std::size_t mid = (lo + hi) / 2;
        if (table.entries[mid].codePoint < c) lo = mid + 1;
        else hi = mid;
      }
      return lo < table.entryCount && table.entries[lo].codePoint == c ? &table.entries[lo] : nullptr;
    }
  }

  std::uint32_t canonicalize(std::uint32_t c, bool unicode) {
    if (c < 128) return c >= 'a' && c <= 'z' ? c - 32 : c;
    const CaseEntry* entry = findCaseEntry(c, unicode);
    if (!entry) return c;
    return caseTable(unicode).members[caseTable(unicode).classStart[entry->classIndex]];
  }

  namespace {
    [[noreturn]] void fail(const char* message) { throw SyntaxError(std::string("Invalid regular expression: ") + message); }

    // JavaScriptCore reports runaway nesting as an out-of-memory RangeError. Its limit depends on
    // its stack (about 30500 levels in Bun); rt::Regex parses and compiles recursively and stops
    // at a fixed depth whose stack use stays under 1 MiB even in an unoptimized build.
    constexpr std::size_t kMaxNesting = 500;
    // JavaScriptCore's limit on capture groups.
    constexpr std::uint32_t kMaxCaptures = 32768;

    // Every character the engine treats as case-insensitively equal to a member of `set`. One
    // merge walk over the sorted case entries finds the classes with members on both sides.
    void addCaseClosure(RangeSet& set, bool unicode) {
      const CaseTable& table = caseTable(unicode);
      const std::vector<Range>& ranges = set.ranges();
      std::vector<std::uint8_t> sides(table.entryCount, 0);  // per class: 1 = a member inside, 2 = outside
      std::size_t r = 0;
      for (std::size_t i = 0; i < table.entryCount; ++i) {
        const CaseEntry& entry = table.entries[i];
        while (r < ranges.size() && ranges[r].hi < entry.codePoint) ++r;
        bool inside = r < ranges.size() && ranges[r].lo <= entry.codePoint;
        sides[entry.classIndex] = static_cast<std::uint8_t>(sides[entry.classIndex] | (inside ? 1 : 2));
      }
      std::vector<std::uint32_t> extra;
      for (std::size_t c = 0; c < sides.size(); ++c) {
        if (sides[c] != 3) continue;
        for (std::uint32_t j = table.classStart[c]; j < table.classStart[c + 1]; ++j) extra.push_back(table.members[j]);
      }
      if (extra.empty()) return;
      std::sort(extra.begin(), extra.end());
      RangeSet additions;
      for (std::uint32_t c : extra) additions.add(c);
      set.addAll(additions);
    }

    void caseEquivalents(std::uint32_t c, bool unicode, std::vector<std::uint32_t>& out) {
      out.clear();
      const CaseEntry* entry = findCaseEntry(c, unicode);
      if (!entry) {
        out.push_back(c);
        return;
      }
      const CaseTable& table = caseTable(unicode);
      for (std::uint32_t j = table.classStart[entry->classIndex]; j < table.classStart[entry->classIndex + 1]; ++j) out.push_back(table.members[j]);
    }

    bool inToggleSet(const std::uint32_t* boundaries, std::size_t count, std::uint32_t c) {
      std::size_t n = static_cast<std::size_t>(std::upper_bound(boundaries, boundaries + count, c) - boundaries);
      return (n & 1) != 0;
    }

    // Decodes a toggle list from regex_tables.cpp: delta-encoded base-32 numbers whose last digit
    // is '0'-'9' or 'A'-'V' and whose leading digits are 'W'-'Z', 'a'-'z', '!' or '#'.
    void addToggles(RangeSet& set, const char* toggles) {
      std::uint32_t value = 0, previous = 0, start = 0;
      bool inside = false;
      for (const char* p = toggles; *p != '\0'; ++p) {
        char ch = *p;
        std::uint32_t digit = 0;
        bool last = true;
        if (ch >= '0' && ch <= '9') digit = static_cast<std::uint32_t>(ch - '0');
        else if (ch >= 'A' && ch <= 'V') digit = static_cast<std::uint32_t>(ch - 'A' + 10);
        else if (ch >= 'W' && ch <= 'Z') digit = static_cast<std::uint32_t>(ch - 'W'), last = false;
        else if (ch >= 'a' && ch <= 'z') digit = static_cast<std::uint32_t>(ch - 'a' + 4), last = false;
        else digit = ch == '!' ? 30u : 31u, last = false;
        value = value * 32 + digit;
        if (!last) continue;
        std::uint32_t boundary = previous + value;
        previous = boundary;
        value = 0;
        if (inside) set.add(start, boundary - 1);
        else start = boundary;
        inside = !inside;
      }
      if (inside) set.add(start, kMaxCodePoint);
    }

    // General_Category leaf values in kGcRuns order, with their names and aliases.
    struct CategoryName {
      const char* name;
      std::uint32_t mask;
    };
    constexpr std::uint32_t gcBit(int index) { return 1u << index; }
    enum : int { Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po, Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co, Cn };
    constexpr std::uint32_t kLetters = gcBit(Lu) | gcBit(Ll) | gcBit(Lt) | gcBit(Lm) | gcBit(Lo);
    constexpr std::uint32_t kCased = gcBit(Lu) | gcBit(Ll) | gcBit(Lt);
    constexpr std::uint32_t kMarks = gcBit(Mn) | gcBit(Mc) | gcBit(Me);
    constexpr std::uint32_t kNumbers = gcBit(Nd) | gcBit(Nl) | gcBit(No);
    constexpr std::uint32_t kPunctuation = gcBit(Pc) | gcBit(Pd) | gcBit(Ps) | gcBit(Pe) | gcBit(Pi) | gcBit(Pf) | gcBit(Po);
    constexpr std::uint32_t kSymbols = gcBit(Sm) | gcBit(Sc) | gcBit(Sk) | gcBit(So);
    constexpr std::uint32_t kSeparators = gcBit(Zs) | gcBit(Zl) | gcBit(Zp);
    constexpr std::uint32_t kOthers = gcBit(Cc) | gcBit(Cf) | gcBit(Cs) | gcBit(Co) | gcBit(Cn);
    constexpr std::uint32_t kAllCategories = (1u << 30) - 1;
    constexpr CategoryName kCategoryNames[] = {
      {"C", kOthers}, {"Other", kOthers}, {"Cc", gcBit(Cc)}, {"Control", gcBit(Cc)}, {"cntrl", gcBit(Cc)}, {"Cf", gcBit(Cf)}, {"Format", gcBit(Cf)},
      {"Cn", gcBit(Cn)}, {"Unassigned", gcBit(Cn)}, {"Co", gcBit(Co)}, {"Private_Use", gcBit(Co)}, {"Cs", gcBit(Cs)}, {"Surrogate", gcBit(Cs)},
      {"L", kLetters}, {"Letter", kLetters}, {"LC", kCased}, {"Cased_Letter", kCased}, {"Ll", gcBit(Ll)}, {"Lowercase_Letter", gcBit(Ll)},
      {"Lm", gcBit(Lm)}, {"Modifier_Letter", gcBit(Lm)}, {"Lo", gcBit(Lo)}, {"Other_Letter", gcBit(Lo)}, {"Lt", gcBit(Lt)}, {"Titlecase_Letter", gcBit(Lt)},
      {"Lu", gcBit(Lu)}, {"Uppercase_Letter", gcBit(Lu)}, {"M", kMarks}, {"Mark", kMarks}, {"Combining_Mark", kMarks}, {"Mc", gcBit(Mc)},
      {"Spacing_Mark", gcBit(Mc)}, {"Me", gcBit(Me)}, {"Enclosing_Mark", gcBit(Me)}, {"Mn", gcBit(Mn)}, {"Nonspacing_Mark", gcBit(Mn)},
      {"N", kNumbers}, {"Number", kNumbers}, {"Nd", gcBit(Nd)}, {"Decimal_Number", gcBit(Nd)}, {"digit", gcBit(Nd)}, {"Nl", gcBit(Nl)},
      {"Letter_Number", gcBit(Nl)}, {"No", gcBit(No)}, {"Other_Number", gcBit(No)}, {"P", kPunctuation}, {"Punctuation", kPunctuation},
      {"punct", kPunctuation}, {"Pc", gcBit(Pc)}, {"Connector_Punctuation", gcBit(Pc)}, {"Pd", gcBit(Pd)}, {"Dash_Punctuation", gcBit(Pd)},
      {"Pe", gcBit(Pe)}, {"Close_Punctuation", gcBit(Pe)}, {"Pf", gcBit(Pf)}, {"Final_Punctuation", gcBit(Pf)}, {"Pi", gcBit(Pi)},
      {"Initial_Punctuation", gcBit(Pi)}, {"Po", gcBit(Po)}, {"Other_Punctuation", gcBit(Po)}, {"Ps", gcBit(Ps)}, {"Open_Punctuation", gcBit(Ps)},
      {"S", kSymbols}, {"Symbol", kSymbols}, {"Sc", gcBit(Sc)}, {"Currency_Symbol", gcBit(Sc)}, {"Sk", gcBit(Sk)}, {"Modifier_Symbol", gcBit(Sk)},
      {"Sm", gcBit(Sm)}, {"Math_Symbol", gcBit(Sm)}, {"So", gcBit(So)}, {"Other_Symbol", gcBit(So)}, {"Z", kSeparators}, {"Separator", kSeparators},
      {"Zl", gcBit(Zl)}, {"Line_Separator", gcBit(Zl)}, {"Zp", gcBit(Zp)}, {"Paragraph_Separator", gcBit(Zp)}, {"Zs", gcBit(Zs)},
      {"Space_Separator", gcBit(Zs)},
    };

    void addGeneralCategory(RangeSet& set, std::uint32_t mask) {
      for (std::size_t i = 0; i < kGcRunCount; ++i) {
        if ((mask & (1u << kGcRuns[i].category)) == 0) continue;
        std::uint32_t end = i + 1 < kGcRunCount ? kGcRuns[i + 1].start - 1 : kMaxCodePoint;
        set.add(kGcRuns[i].start, end);
      }
    }

    // Any, ASCII and Assigned: accepted alone and, in JavaScriptCore, also as gc= values.
    bool addSpecialProperty(RangeSet& set, const std::string& name) {
      if (name == "Any") set.add(0, kMaxCodePoint);
      else if (name == "ASCII") set.add(0, 0x7F);
      else if (name == "Assigned") addGeneralCategory(set, kAllCategories & ~gcBit(Cn));
      else return false;
      return true;
    }

    bool addCategory(RangeSet& set, const std::string& name) {
      for (const CategoryName& entry : kCategoryNames) {
        if (name != entry.name) continue;
        addGeneralCategory(set, entry.mask);
        return true;
      }
      return false;
    }

    // The u-mode set of \p{name} or \p{name=value}; nullopt when JavaScriptCore rejects it.
    std::optional<RangeSet> unicodeProperty(const std::string& name, const std::string* value) {
      RangeSet set;
      if (value) {
        if (name == "General_Category" || name == "gc") {
          if (addCategory(set, *value) || addSpecialProperty(set, *value)) return set;
          return std::nullopt;
        }
        bool extensions = name == "Script_Extensions" || name == "scx";
        if (!extensions && name != "Script" && name != "sc") return std::nullopt;
        for (std::size_t i = 0; i < kScriptCount; ++i) {
          const ScriptData& script = kScripts[i];
          if (*value != script.name && *value != script.code && !(script.alias && *value == script.alias)) continue;
          addToggles(set, extensions && script.scxToggles ? script.scxToggles : script.scToggles);
          return set;
        }
        return std::nullopt;
      }
      if (addCategory(set, name) || addSpecialProperty(set, name)) return set;
      for (std::size_t i = 0; i < kBinaryPropertyCount; ++i) {
        const BinaryPropertyData& property = kBinaryProperties[i];
        if (name != property.name && !(property.alias && name == property.alias)) continue;
        addToggles(set, property.toggles);
        return set;
      }
      return std::nullopt;
    }

    enum class NodeKind : std::uint8_t { Empty, Char, Class, Dot, Any, Seq, Alt, Group, Repeat, Assert, Look, BackRef };

    struct Node {
      NodeKind kind = NodeKind::Empty;
      std::uint32_t value = 0;  // Char: character; Class: class index; Group: capture number; Assert: Op; BackRef: groupLists offset
      std::uint32_t count = 0;  // BackRef: number of groups
      std::uint32_t min = 0;
      std::uint32_t max = 0;
      std::uint32_t firstGroup = 0;  // Repeat: captures inside are [firstGroup, groupEnd)
      std::uint32_t groupEnd = 0;
      bool greedy = true;
      bool ignoreCase = false;
      bool negative = false;
      bool behind = false;
      bool unicodeWord = false;
      std::vector<std::uint32_t> children;
    };

    struct PendingNamedReference {
      std::uint32_t node;
      std::string name;
    };

    struct GroupRecord {
      std::string name;
      std::vector<std::pair<std::uint32_t, std::uint32_t>> path;  // (disjunction, alternative) from the root
    };

    void appendUtf8(std::string& out, std::uint32_t c) { appendWtf8(out, c); }

    std::u16string toUtf16(std::string_view bytes) {
      std::u16string out;
      out.reserve(bytes.size());
      std::size_t i = 0;
      while (i < bytes.size()) {
        std::uint32_t cp = decodeWtf8(bytes, i);
        if (cp >= 0x10000) {
          out += static_cast<char16_t>(0xD800 + ((cp - 0x10000) >> 10));
          out += static_cast<char16_t>(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
          out += static_cast<char16_t>(cp);
        }
      }
      return out;
    }

    bool isDigit(std::uint32_t c) { return c >= '0' && c <= '9'; }
    bool isOctal(std::uint32_t c) { return c >= '0' && c <= '7'; }
    bool isHexDigit(std::uint32_t c) { return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
    std::uint32_t hexValue(std::uint32_t c) { return isDigit(c) ? c - '0' : (c | 0x20) - 'a' + 10; }
    bool isAsciiLetter(std::uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
    bool isPropertyCharacter(std::uint32_t c) { return isAsciiLetter(c) || isDigit(c) || c == '_' || c == '='; }

    // Under u an escaped ASCII character must be a SyntaxCharacter or `/` (JavaScriptCore lets
    // every non-ASCII character be escaped).
    bool isUnicodeIdentityEscape(std::uint32_t c) {
      if (c >= 0x80) return true;
      switch (c) {
        case '^': case '$': case '\\': case '.': case '*': case '+': case '?': case '(': case ')': case '[': case ']': case '{': case '}': case '|': case '/':
          return true;
        default:
          return false;
      }
    }

    constexpr std::uint32_t kEnd = 0xFFFFFFFFu;
    constexpr std::uint64_t kNoLimit = std::numeric_limits<std::uint64_t>::max();

    // JavaScriptCore parses a pattern up to twice (YarrParser::handleIllegalReferences): the first
    // pass reads every \N as a backreference and every \k<name> as a named, possibly forward,
    // reference. Illegal ones are errors under u or once group names exist; otherwise the second
    // pass reads them as Annex B escapes.
    struct PassConfig {
      std::uint64_t backReferenceLimit = kNoLimit;
      bool forwardReferencesAllowed = true;
    };

    class Parser {
    public:
      Parser(std::u16string_view source, bool unicode, bool ignoreCase, bool multiline, bool dotAll, PassConfig config)
          : src_(source), unicode_(unicode), ignoreCase_(ignoreCase), multiline_(multiline), dotAll_(dotAll), config_(config) {}

      std::uint32_t parse() {
        std::uint32_t root = parseDisjunction();
        if (!eof()) fail("unmatched parentheses");
        return root;
      }

      // Resolves the named references of the final pass to the groups carrying each name.
      void resolveNamedReferences() {
        for (const PendingNamedReference& pending : pendingReferences_) {
          std::vector<std::uint32_t> groups;
          for (std::uint32_t g = 1; g < groups_.size(); ++g)
            if (groups_[g].name == pending.name) groups.push_back(g);
          Node& node = nodes_[pending.node];
          node.value = static_cast<std::uint32_t>(groupLists_.size());
          node.count = static_cast<std::uint32_t>(groups.size());
          groupLists_.insert(groupLists_.end(), groups.begin(), groups.end());
        }
      }

      std::uint32_t captureCount() const { return static_cast<std::uint32_t>(groups_.size() - 1); }
      bool hasGroupNames() const { return hasGroupNames_; }
      bool hasName(const std::string& name) const {
        for (std::size_t g = 1; g < groups_.size(); ++g)
          if (groups_[g].name == name) return true;
        return false;
      }

      std::vector<Node> nodes_;
      std::vector<RangeSet> classSets_;
      std::vector<std::uint32_t> groupLists_;
      std::vector<GroupRecord> groups_{GroupRecord{}};  // index = capture number; [0] is the whole match
      std::uint64_t maxBackReference_ = 0;
      bool kIdentityEscapeSeen_ = false;
      std::vector<std::string> forwardNames_;

    private:
      bool eof() const { return pos_ >= src_.size(); }
      std::uint32_t peek(std::size_t ahead = 0) const { return pos_ + ahead < src_.size() ? static_cast<std::uint32_t>(src_[pos_ + ahead]) : kEnd; }
      std::uint32_t at(std::size_t index) const { return index < src_.size() ? static_cast<std::uint32_t>(src_[index]) : kEnd; }
      // One pattern character: a code point under u (surrogate pairs combine), else a code unit.
      std::uint32_t readChar() {
        std::uint32_t c = src_[pos_++];
        if (unicode_ && isHighSurrogate(c) && pos_ < src_.size() && isLowSurrogate(src_[pos_])) c = combineSurrogates(c, src_[pos_++]);
        return c;
      }

      std::uint32_t addNode(Node node) {
        nodes_.push_back(std::move(node));
        return static_cast<std::uint32_t>(nodes_.size() - 1);
      }
      std::uint32_t charNode(std::uint32_t c) {
        Node node;
        node.kind = NodeKind::Char;
        node.value = c;
        node.ignoreCase = ignoreCase_;
        return addNode(std::move(node));
      }
      std::uint32_t classNode(RangeSet set, bool negate) {
        if (ignoreCase_) addCaseClosure(set, unicode_);
        if (negate) set = set.complement(unicode_ ? kMaxCodePoint : 0xFFFF);
        Node node;
        node.kind = NodeKind::Class;
        node.value = static_cast<std::uint32_t>(classSets_.size());
        classSets_.push_back(std::move(set));
        return addNode(std::move(node));
      }
      std::uint32_t assertNode(Op op) {
        Node node;
        node.kind = NodeKind::Assert;
        node.value = static_cast<std::uint32_t>(op);
        node.unicodeWord = unicode_ && ignoreCase_;
        return addNode(std::move(node));
      }
      std::uint32_t backReferenceNode(std::uint32_t group) {
        Node node;
        node.kind = NodeKind::BackRef;
        node.value = static_cast<std::uint32_t>(groupLists_.size());
        node.count = 1;
        node.ignoreCase = ignoreCase_;
        groupLists_.push_back(group);
        return addNode(std::move(node));
      }
      std::uint32_t namedReferenceNode(std::string name) {
        Node node;
        node.kind = NodeKind::BackRef;
        node.ignoreCase = ignoreCase_;
        std::uint32_t index = addNode(std::move(node));
        pendingReferences_.push_back({index, std::move(name)});
        return index;
      }

      std::uint32_t parseDisjunction() {
        std::uint32_t disjunction = nextDisjunction_++;
        path_.emplace_back(disjunction, 0);
        std::vector<std::uint32_t> alternatives;
        alternatives.push_back(parseAlternative());
        while (peek() == '|') {
          ++pos_;
          ++path_.back().second;
          alternatives.push_back(parseAlternative());
        }
        path_.pop_back();
        if (alternatives.size() == 1) return alternatives[0];
        Node node;
        node.kind = NodeKind::Alt;
        node.children = std::move(alternatives);
        return addNode(std::move(node));
      }

      std::uint32_t parseAlternative() {
        std::vector<std::uint32_t> terms;
        while (!eof() && peek() != '|' && peek() != ')') parseTerm(terms);
        if (terms.size() == 1) return terms[0];
        Node node;
        node.kind = terms.empty() ? NodeKind::Empty : NodeKind::Seq;
        node.children = std::move(terms);
        return addNode(std::move(node));
      }

      void parseTerm(std::vector<std::uint32_t>& terms) {
        std::uint32_t c = peek();
        std::uint32_t firstGroup = nextGroup_;
        std::uint32_t atom = 0;
        bool quantifiable = true;
        const char* quantifierError = "nothing to repeat";
        switch (c) {
          case '^':
            ++pos_;
            atom = assertNode(multiline_ ? Op::AssertLineStart : Op::AssertStart);
            quantifiable = false;
            break;
          case '$':
            ++pos_;
            atom = assertNode(multiline_ ? Op::AssertLineEnd : Op::AssertEnd);
            quantifiable = false;
            break;
          case '\\':
            if (peek(1) == 'b' || peek(1) == 'B') {
              atom = assertNode(peek(1) == 'b' ? Op::AssertWord : Op::AssertNotWord);
              pos_ += 2;
              quantifiable = false;
            } else {
              ++pos_;
              atom = parseAtomEscape();
            }
            break;
          case '(':
            atom = parseGroup(quantifiable, quantifierError);
            break;
          case '.': {
            ++pos_;
            Node node;
            node.kind = dotAll_ ? NodeKind::Any : NodeKind::Dot;
            atom = addNode(std::move(node));
            break;
          }
          case '[':
            atom = parseClass();
            break;
          case '*':
          case '+':
          case '?':
            fail("nothing to repeat");
          case '{': {
            std::uint32_t min = 0, max = 0;
            if (parseBraceQuantifier(min, max)) fail("nothing to repeat");
            if (unicode_) fail("incomplete {} quantifier for Unicode pattern");
            ++pos_;
            atom = charNode('{');
            break;
          }
          case '}':
          case ']':
            if (unicode_) fail("unmatched ] or } bracket for Unicode pattern");
            ++pos_;
            atom = charNode(c);
            break;
          default:
            atom = charNode(readChar());
            break;
        }
        std::uint32_t min = 0, max = 0;
        std::uint32_t q = peek();
        if (q == '*') {
          ++pos_, min = 0, max = kInfinite;
        } else if (q == '+') {
          ++pos_, min = 1, max = kInfinite;
        } else if (q == '?') {
          ++pos_, min = 0, max = 1;
        } else if (q == '{') {
          if (!parseBraceQuantifier(min, max)) {
            if (unicode_) fail("incomplete {} quantifier for Unicode pattern");
            terms.push_back(atom);
            return;
          }
        } else {
          terms.push_back(atom);
          return;
        }
        if (!quantifiable) fail(quantifierError);
        bool greedy = true;
        if (peek() == '?') {
          ++pos_;
          greedy = false;
        }
        Node node;
        node.kind = NodeKind::Repeat;
        node.min = min;
        node.max = max;
        node.greedy = greedy;
        node.firstGroup = firstGroup;
        node.groupEnd = nextGroup_;
        node.children.push_back(atom);
        terms.push_back(addNode(std::move(node)));
      }

      // A decimal number at p, saturating at 2^64 - 1 as JavaScriptCore's does.
      std::uint64_t number(std::size_t& p) const {
        std::uint64_t value = 0;
        while (isDigit(at(p))) {
          std::uint64_t digit = at(p) - '0';
          value = value > (kNoLimit - digit) / 10 ? kNoLimit : value * 10 + digit;
          ++p;
        }
        return value;
      }

      // `{n}`, `{n,}` or `{n,m}` at pos_: consumes it and returns true; otherwise leaves pos_ and
      // returns false. Counts from 2^32 - 1 up are unbounded for matching.
      bool parseBraceQuantifier(std::uint32_t& min, std::uint32_t& max) {
        std::size_t p = pos_ + 1;
        if (!isDigit(at(p))) return false;
        std::uint64_t low = number(p), high = low;
        if (at(p) == ',') {
          ++p;
          high = isDigit(at(p)) ? number(p) : kNoLimit;
        }
        if (at(p) != '}') return false;
        pos_ = p + 1;
        if (low == kNoLimit) fail("number too large in {} quantifier");
        if (low > high) fail("numbers out of order in {} quantifier");
        min = low >= kInfinite ? kInfinite : static_cast<std::uint32_t>(low);
        max = high >= kInfinite ? kInfinite : static_cast<std::uint32_t>(high);
        return true;
      }

      std::uint32_t parseGroup(bool& quantifiable, const char*& quantifierError) {
        ++pos_;  // (
        if (++depth_ > kMaxNesting) throw RangeError("Out of memory: Invalid regular expression: too many nested disjunctions");
        std::uint32_t result = 0;
        if (peek() != '?') {
          result = finishCapture(openCapture(""));
        } else {
          std::uint32_t kind = peek(1);
          if (kind == ':') {
            pos_ += 2;
            result = parseDisjunction();
            expectClose();
          } else if (kind == '=' || kind == '!') {
            pos_ += 2;
            quantifiable = !unicode_;
            result = finishLook(false, kind == '!');
          } else if (kind == '<' && (peek(2) == '=' || peek(2) == '!')) {
            bool negative = peek(2) == '!';
            pos_ += 3;
            quantifiable = false;
            quantifierError = "invalid quantifier";
            result = finishLook(true, negative);
          } else if (kind == '<') {
            pos_ += 2;
            std::optional<std::string> name = tryGroupName();
            if (!name) fail("invalid group specifier name");
            if (kIdentityEscapeSeen_) fail("invalid \\k<> named backreference");
            for (std::uint32_t g = 1; g < groups_.size(); ++g)
              if (groups_[g].name == *name && mightBothParticipate(groups_[g].path, path_)) fail("duplicate group specifier name");
            hasGroupNames_ = true;
            result = finishCapture(openCapture(std::move(*name)));
          } else if (kind == 'i' || kind == 'm' || kind == 's' || kind == '-') {
            result = parseModifiers();
          } else {
            fail("unrecognized character after (?");
          }
        }
        --depth_;
        return result;
      }

      std::uint32_t openCapture(std::string name) {
        if (nextGroup_ > kMaxCaptures) fail("too many captures");
        groups_.push_back(GroupRecord{std::move(name), path_});
        return nextGroup_++;
      }

      std::uint32_t finishCapture(std::uint32_t number) {
        std::uint32_t body = parseDisjunction();
        expectClose();
        Node node;
        node.kind = NodeKind::Group;
        node.value = number;
        node.children.push_back(body);
        return addNode(std::move(node));
      }

      std::uint32_t finishLook(bool behind, bool negative) {
        std::uint32_t body = parseDisjunction();
        expectClose();
        Node node;
        node.kind = NodeKind::Look;
        node.behind = behind;
        node.negative = negative;
        node.children.push_back(body);
        return addNode(std::move(node));
      }

      // (?ims-ims:...) with pos_ at `?`. Like JavaScriptCore, everything up to the next `:` (or the
      // end of the pattern) is read as the flag run and its last problem is reported: a flag
      // repeated on its side of the dash, a second dash, or any other character. Only a clean run
      // is then checked for a flag on both sides or a bare dash.
      std::uint32_t parseModifiers() {
        pos_ += 1;
        unsigned add = 0, remove = 0;
        bool dash = false;
        const char* error = nullptr;
        auto bit = [](std::uint32_t c) -> unsigned { return c == 'i' ? 1u : c == 'm' ? 2u : c == 's' ? 4u : 0u; };
        for (; !eof() && peek() != ':'; ++pos_) {
          std::uint32_t c = peek();
          if (unsigned flag = bit(c)) {
            unsigned& side = dash ? remove : add;
            if (side & flag) error = "invalid regular expression modifier";
            side |= flag;
          } else if (c == '-') {
            if (dash) error = "invalid regular expression modifier";
            dash = true;
          } else {
            error = "unrecognized character after (?";
          }
        }
        if (!error && ((add & remove) != 0 || (dash && add == 0 && remove == 0))) error = "invalid regular expression modifier";
        if (eof()) fail(error ? error : "missing )");
        if (error) fail(error);
        ++pos_;
        bool savedIgnoreCase = ignoreCase_, savedMultiline = multiline_, savedDotAll = dotAll_;
        if (add & 1u) ignoreCase_ = true;
        if (add & 2u) multiline_ = true;
        if (add & 4u) dotAll_ = true;
        if (remove & 1u) ignoreCase_ = false;
        if (remove & 2u) multiline_ = false;
        if (remove & 4u) dotAll_ = false;
        std::uint32_t body = parseDisjunction();
        expectClose();
        ignoreCase_ = savedIgnoreCase, multiline_ = savedMultiline, dotAll_ = savedDotAll;
        return body;
      }

      void expectClose() {
        if (peek() != ')') fail("missing )");
        ++pos_;
      }

      // Duplicate group names are allowed only in different alternatives of one disjunction.
      static bool mightBothParticipate(const std::vector<std::pair<std::uint32_t, std::uint32_t>>& a, const std::vector<std::pair<std::uint32_t, std::uint32_t>>& b) {
        std::size_t n = std::min(a.size(), b.size());
        for (std::size_t i = 0; i < n; ++i) {
          if (a[i].first != b[i].first) return true;
          if (a[i].second != b[i].second) return false;
        }
        return true;
      }

      // One character of a group name (JavaScriptCore's tryConsumeIdentifierCharacter): a code
      // point with or without u, or a \u escape; any other escape is an error.
      std::uint32_t identifierCharacter() {
        std::uint32_t c = src_[pos_++];
        if (c == '\\') {
          if (peek() == 'u')
            if (std::optional<std::uint32_t> escaped = parseUnicodeEscape(true)) return *escaped;
          fail("invalid Unicode \\u escape");
        }
        if (isHighSurrogate(c) && pos_ < src_.size() && isLowSurrogate(src_[pos_])) c = combineSurrogates(c, src_[pos_++]);
        return c;
      }

      // RegExpIdentifierName through `>` (JavaScriptCore's tryConsumeGroupName): the name, or
      // nullopt with pos_ unchanged.
      std::optional<std::string> tryGroupName() {
        if (eof()) return std::nullopt;
        std::size_t save = pos_;
        std::uint32_t c = identifierCharacter();
        if (inToggleSet(kGroupNameStart, kGroupNameStartCount, c)) {
          std::string name;
          appendUtf8(name, c);
          while (!eof()) {
            c = identifierCharacter();
            if (c == '>') return name;
            if (!inToggleSet(kGroupNameContinue, kGroupNameContinueCount, c)) break;
            appendUtf8(name, c);
          }
        }
        pos_ = save;
        return std::nullopt;
      }

      // `\u` escape with pos_ at `u`: `\u{...}` when codePointMode (always in group names), else
      // four hex digits, joined with a following `\uDC00`-style trail under codePointMode.
      std::optional<std::uint32_t> parseUnicodeEscape(bool codePointMode) {
        std::size_t p = pos_ + 1;
        if (codePointMode && at(p) == '{') {
          ++p;
          std::uint32_t value = 0;
          bool any = false;
          while (isHexDigit(at(p))) {
            value = value * 16 + hexValue(at(p));
            if (value > kMaxCodePoint) fail("invalid Unicode code point \\u{} escape");
            any = true;
            ++p;
          }
          if (!any || at(p) != '}') fail("invalid Unicode code point \\u{} escape");
          pos_ = p + 1;
          return value;
        }
        auto hex4 = [&](std::size_t q, std::uint32_t& out) {
          for (std::size_t k = 0; k < 4; ++k)
            if (!isHexDigit(at(q + k))) return false;
          out = (hexValue(at(q)) << 12) | (hexValue(at(q + 1)) << 8) | (hexValue(at(q + 2)) << 4) | hexValue(at(q + 3));
          return true;
        };
        std::uint32_t value = 0;
        if (!hex4(p, value)) return std::nullopt;
        p += 4;
        std::uint32_t low = 0;
        if (codePointMode && isHighSurrogate(value) && at(p) == '\\' && at(p + 1) == 'u' && hex4(p + 2, low) && isLowSurrogate(low)) {
          value = combineSurrogates(value, low);
          p += 6;
        }
        pos_ = p;
        return value;
      }

      // LegacyOctalEscapeSequence with pos_ at its first digit (at most three digits, <= 0377).
      std::uint32_t parseLegacyOctal() {
        std::uint32_t value = src_[pos_++] - u'0';
        if (!isOctal(peek())) return value;
        bool small = value <= 3;
        value = value * 8 + (src_[pos_++] - u'0');
        if (small && isOctal(peek())) value = value * 8 + (src_[pos_++] - u'0');
        return value;
      }

      RangeSet classEscapeSet(std::uint32_t c) const {
        RangeSet set;
        switch (c | 0x20) {
          case 'd':
            set.add('0', '9');
            break;
          case 's':
            set.add(0x09, 0x0D), set.add(0x20), set.add(0xA0), set.add(0x1680), set.add(0x2000, 0x200A);
            set.add(0x2028, 0x2029), set.add(0x202F), set.add(0x205F), set.add(0x3000), set.add(0xFEFF);
            break;
          default:
            set.add('0', '9'), set.add('A', 'Z'), set.add('_'), set.add('a', 'z');
            if (unicode_ && ignoreCase_) set.add(0x017F), set.add(0x212A);
            break;
        }
        if (c == 'D' || c == 'S' || c == 'W') set = set.complement(unicode_ ? kMaxCodePoint : 0xFFFF);
        return set;
      }

      // \p{...} or \P{...} under u, with pos_ just after `p`.
      RangeSet parseProperty(bool negate) {
        if (peek() != '{') fail("invalid property expression");
        ++pos_;
        std::string text;
        for (;;) {
          std::uint32_t c = peek();
          if (c == '}') break;
          if (!isPropertyCharacter(c)) fail("invalid property expression");
          text += static_cast<char>(c);
          ++pos_;
        }
        ++pos_;
        std::size_t equals = text.find('=');
        std::optional<RangeSet> set;
        if (equals == std::string::npos) {
          set = unicodeProperty(text, nullptr);
        } else {
          std::string name = text.substr(0, equals), value = text.substr(equals + 1);
          if (!name.empty() && !value.empty() && value.find('=') == std::string::npos) set = unicodeProperty(name, &value);
        }
        if (!set) fail("invalid property expression");
        return negate ? set->complement(kMaxCodePoint) : std::move(*set);
      }

      std::uint32_t parseAtomEscape() {
        if (eof()) fail("\\ at end of pattern");
        std::uint32_t c = peek();
        switch (c) {
          case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
            ++pos_;
            return classNode(classEscapeSet(c), false);
          case 'p':
          case 'P':
            if (!unicode_) break;
            ++pos_;
            return classNode(parseProperty(c == 'P'), false);
          case 'k':
            return parseNamedReference();
          case '0':
            if (!isDigit(peek(1))) {
              ++pos_;
              return charNode(0);
            }
            if (unicode_) fail("invalid octal escape for Unicode pattern");
            return charNode(parseLegacyOctal());
          default:
            break;
        }
        if (c >= '1' && c <= '9') {
          std::size_t save = pos_;
          std::size_t p = pos_;
          std::uint64_t n = number(p);
          if (n <= config_.backReferenceLimit) {
            pos_ = p;
            maxBackReference_ = std::max(maxBackReference_, n);
            return backReferenceNode(n > kMaxCaptures ? kMaxCaptures + 1 : static_cast<std::uint32_t>(n));
          }
          pos_ = save;
          if (unicode_) fail("invalid backreference for Unicode pattern");
          if (c >= '8') {
            ++pos_;
            return charNode(c);
          }
          return charNode(parseLegacyOctal());
        }
        return charNode(parseCharacterEscape(false));
      }

      // \k with pos_ at `k`: \k<name> when the name exists (or may be defined later, first pass);
      // otherwise an identity escape, which JavaScriptCore rejects once group names exist.
      std::uint32_t parseNamedReference() {
        ++pos_;
        std::size_t save = pos_;
        if (peek() == '<') {
          ++pos_;
          if (std::optional<std::string> name = tryGroupName()) {
            if (hasName(*name)) return namedReferenceNode(std::move(*name));
            if (config_.forwardReferencesAllowed) {
              forwardNames_.push_back(*name);
              return namedReferenceNode(std::move(*name));
            }
          }
        }
        pos_ = save;
        if (unicode_) fail("invalid escaped character for Unicode pattern");
        kIdentityEscapeSeen_ = true;
        return charNode('k');
      }

      // CharacterEscape (and identity escapes) with pos_ just after the backslash.
      std::uint32_t parseCharacterEscape(bool inClass) {
        std::uint32_t c = peek();
        switch (c) {
          case 'f': ++pos_; return 0x0C;
          case 'n': ++pos_; return 0x0A;
          case 'r': ++pos_; return 0x0D;
          case 't': ++pos_; return 0x09;
          case 'v': ++pos_; return 0x0B;
          case 'c': {
            std::uint32_t letter = peek(1);
            if (isAsciiLetter(letter) || (inClass && !unicode_ && (isDigit(letter) || letter == '_'))) {
              pos_ += 2;
              return letter % 32;
            }
            if (unicode_) fail(letter == kEnd ? "invalid escaped character for Unicode pattern" : "invalid \\c escape for Unicode pattern");
            return '\\';  // Annex B: the backslash is literal and `c` is read next
          }
          case 'x':
            if (isHexDigit(peek(1)) && isHexDigit(peek(2))) {
              std::uint32_t value = hexValue(peek(1)) * 16 + hexValue(peek(2));
              pos_ += 3;
              return value;
            }
            if (unicode_) fail("invalid escaped character for Unicode pattern");
            ++pos_;
            return 'x';
          case 'u':
            if (std::optional<std::uint32_t> value = parseUnicodeEscape(unicode_)) return *value;
            if (unicode_) fail("invalid Unicode \\u escape");
            ++pos_;
            return 'u';
          default:
            break;
        }
        if (unicode_ && !isUnicodeIdentityEscape(c) && !(inClass && c == '-')) fail("invalid escaped character for Unicode pattern");
        return readChar();
      }

      struct ClassAtom {
        bool isSet = false;
        std::uint32_t c = 0;
        RangeSet set;
      };

      ClassAtom parseClassAtom() {
        if (eof()) fail("missing terminating ] for character class");
        ClassAtom atom;
        if (peek() != '\\') {
          atom.c = readChar();
          return atom;
        }
        ++pos_;
        if (eof()) fail("\\ at end of pattern");
        std::uint32_t c = peek();
        switch (c) {
          case 'd': case 'D': case 's': case 'S': case 'w': case 'W':
            ++pos_;
            atom.isSet = true;
            atom.set = classEscapeSet(c);
            return atom;
          case 'p':
          case 'P':
            if (!unicode_) break;
            ++pos_;
            atom.isSet = true;
            atom.set = parseProperty(c == 'P');
            return atom;
          case 'b':
            ++pos_;
            atom.c = 0x08;
            return atom;
          case 'k':
            if (unicode_) break;
            ++pos_;
            kIdentityEscapeSeen_ = true;
            atom.c = 'k';
            return atom;
          case '0':
            if (!isDigit(peek(1))) {
              ++pos_;
              atom.c = 0;
              return atom;
            }
            if (unicode_) fail("invalid octal escape for Unicode pattern");
            atom.c = parseLegacyOctal();
            return atom;
          default:
            break;
        }
        if (c >= '1' && c <= '9') {
          if (unicode_) fail("invalid octal escape for Unicode pattern");
          if (c >= '8') {
            ++pos_;
            atom.c = c;
          } else {
            atom.c = parseLegacyOctal();
          }
          return atom;
        }
        if (unicode_ && c == 'B') fail("invalid escaped character for Unicode pattern");
        atom.c = parseCharacterEscape(true);
        return atom;
      }

      std::uint32_t parseClass() {
        ++pos_;  // [
        bool negate = false;
        if (peek() == '^') {
          negate = true;
          ++pos_;
        }
        RangeSet set;
        for (;;) {
          if (eof()) fail("missing terminating ] for character class");
          if (peek() == ']') {
            ++pos_;
            break;
          }
          ClassAtom first = parseClassAtom();
          if (peek() == '-' && peek(1) != ']' && peek(1) != kEnd) {
            ++pos_;
            ClassAtom second = parseClassAtom();
            if (first.isSet || second.isSet) {
              if (unicode_) fail("invalid range in character class for Unicode pattern");
              if (first.isSet) set.addAll(first.set);
              else set.add(first.c);
              set.add('-');
              if (second.isSet) set.addAll(second.set);
              else set.add(second.c);
              continue;
            }
            if (first.c > second.c) fail("range out of order in character class");
            set.add(first.c, second.c);
            continue;
          }
          if (first.isSet) set.addAll(first.set);
          else set.add(first.c);
        }
        return classNode(std::move(set), negate);
      }

      std::u16string_view src_;
      std::size_t pos_ = 0;
      bool unicode_;
      bool ignoreCase_;
      bool multiline_;
      bool dotAll_;
      PassConfig config_;
      bool hasGroupNames_ = false;
      std::size_t depth_ = 0;
      std::uint32_t nextGroup_ = 1;
      std::uint32_t nextDisjunction_ = 0;
      std::vector<std::pair<std::uint32_t, std::uint32_t>> path_;
      std::vector<PendingNamedReference> pendingReferences_;
    };

    // First-character analysis for the search loop.
    struct FirstInfo {
      bool empty = true;  // can match without consuming input
      bool all = false;   // any code unit may start a match
      std::uint64_t units[4] = {};
      bool high = false;
    };

    class Compiler {
    public:
      Compiler(Program& program, const Parser& parser) : program_(program), parser_(parser) {}

      void compileRoot(std::uint32_t root) {
        for (const RangeSet& set : parser_.classSets_) program_.classes.push_back(toClass(set));
        program_.groupLists = parser_.groupLists_;
        groupOpenRegister_.assign(parser_.groups_.size(), 0);
        for (std::size_t g = 1; g < parser_.groups_.size(); ++g) groupOpenRegister_[g] = allocateRegister();
        emit(root, false);
        add(Op::Match);
        analyse(root);
      }

    private:
      const Node& node(std::uint32_t index) const { return parser_.nodes_[index]; }
      std::int32_t allocateRegister() { return static_cast<std::int32_t>(program_.registerCount++); }
      std::int32_t here() const { return static_cast<std::int32_t>(program_.code.size()); }
      std::size_t add(Op op, std::uint8_t flags = 0, std::int32_t a = 0, std::int32_t b = 0, std::int32_t c = 0, std::int32_t d = 0) {
        Inst inst;
        inst.op = op;
        inst.flags = flags;
        inst.a = a, inst.b = b, inst.c = c, inst.d = d;
        program_.code.push_back(inst);
        return program_.code.size() - 1;
      }

      static CharClass toClass(const RangeSet& set) {
        CharClass out;
        for (const Range& range : set.ranges()) {
          for (std::uint32_t c = range.lo; c <= range.hi && c < 128; ++c) out.ascii[c >> 6] |= 1ull << (c & 63);
          if (range.hi >= 128) out.ranges.push_back({std::max<std::uint32_t>(range.lo, 128), range.hi});
        }
        return out;
      }

      std::int32_t addClass(const RangeSet& set) {
        program_.classes.push_back(toClass(set));
        return static_cast<std::int32_t>(program_.classes.size() - 1);
      }

      // A single-character matcher for `index`, or false when it is not one.
      bool charMatcher(std::uint32_t index, Op& op, std::int32_t& operand) {
        const Node& n = node(index);
        switch (n.kind) {
          case NodeKind::Char:
            if (n.ignoreCase) {
              caseEquivalents(n.value, program_.unicode, scratch_);
              if (scratch_.size() > 1) {
                RangeSet set;
                for (std::uint32_t c : scratch_) set.add(c);
                op = Op::Class;
                operand = addClass(set);
                return true;
              }
            }
            op = Op::Char;
            operand = static_cast<std::int32_t>(n.value);
            return true;
          case NodeKind::Class:
            op = Op::Class;
            operand = static_cast<std::int32_t>(n.value);
            return true;
          case NodeKind::Dot:
            op = Op::Dot;
            operand = 0;
            return true;
          case NodeKind::Any:
            op = Op::Any;
            operand = 0;
            return true;
          default:
            return false;
        }
      }

      // A case-sensitive character that can join a literal string run.
      bool stringable(std::uint32_t index) const {
        const Node& n = node(index);
        if (n.kind != NodeKind::Char) return false;
        if (n.ignoreCase) {
          std::vector<std::uint32_t> equivalents;
          caseEquivalents(n.value, program_.unicode, equivalents);
          if (equivalents.size() > 1) return false;
        }
        if (program_.unicode && n.value >= 0xD800 && n.value <= 0xDFFF) return false;
        return true;
      }

      static void appendUnits(std::u16string& out, std::uint32_t c) {
        if (c >= 0x10000) {
          out += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
          out += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
        } else {
          out += static_cast<char16_t>(c);
        }
      }

      bool canBeEmpty(std::uint32_t index) const {
        const Node& n = node(index);
        switch (n.kind) {
          case NodeKind::Char: case NodeKind::Class: case NodeKind::Dot: case NodeKind::Any:
            return false;
          case NodeKind::Seq:
            for (std::uint32_t child : n.children)
              if (!canBeEmpty(child)) return false;
            return true;
          case NodeKind::Alt:
            for (std::uint32_t child : n.children)
              if (canBeEmpty(child)) return true;
            return false;
          case NodeKind::Group:
            return canBeEmpty(n.children[0]);
          case NodeKind::Repeat:
            return n.min == 0 || canBeEmpty(n.children[0]);
          default:
            return true;
        }
      }

      void emit(std::uint32_t index, bool backward) {
        const Node& n = node(index);
        std::uint8_t direction = backward ? kBackward : 0;
        switch (n.kind) {
          case NodeKind::Empty:
            return;
          case NodeKind::Char: case NodeKind::Class: case NodeKind::Dot: case NodeKind::Any: {
            Op op = Op::Fail;
            std::int32_t operand = 0;
            charMatcher(index, op, operand);
            add(op, direction, operand);
            return;
          }
          case NodeKind::Seq: {
            std::size_t count = n.children.size();
            std::size_t i = 0;
            while (i < count) {
              std::size_t k = backward ? count - 1 - i : i;
              std::size_t run = 0;
              while (i + run < count && stringable(n.children[backward ? count - 1 - (i + run) : i + run])) ++run;
              if (run >= 2) {
                std::u16string text;
                for (std::size_t r = 0; r < run; ++r) {
                  std::size_t child = backward ? k + 1 - run + r : k + r;
                  appendUnits(text, node(n.children[child]).value);
                }
                program_.strings.push_back(std::move(text));
                add(Op::String, direction, static_cast<std::int32_t>(program_.strings.size() - 1));
                i += run;
                continue;
              }
              emit(n.children[k], backward);
              ++i;
            }
            return;
          }
          case NodeKind::Alt: {
            std::vector<std::size_t> jumps;
            for (std::size_t i = 0; i < n.children.size(); ++i) {
              std::size_t fork = 0;
              bool last = i + 1 == n.children.size();
              if (!last) fork = add(Op::Fork);
              emit(n.children[i], backward);
              if (!last) {
                jumps.push_back(add(Op::Jump));
                program_.code[fork].a = here();
              }
            }
            for (std::size_t jump : jumps) program_.code[jump].a = here();
            return;
          }
          case NodeKind::Group: {
            std::int32_t reg = groupOpenRegister_[n.value];
            add(Op::SaveOpen, direction, static_cast<std::int32_t>(n.value), reg);
            emit(n.children[0], backward);
            add(Op::SaveClose, direction, static_cast<std::int32_t>(n.value), reg);
            return;
          }
          case NodeKind::Assert:
            add(static_cast<Op>(n.value), n.unicodeWord ? kUnicodeWord : 0);
            return;
          case NodeKind::Look: {
            std::int32_t reg = allocateRegister();
            std::uint8_t flags = n.negative ? kNegative : 0;
            std::size_t begin = add(Op::LookBegin, flags, reg);
            emit(n.children[0], n.behind);
            add(Op::LookEnd, flags, reg);
            program_.code[begin].b = here();
            return;
          }
          case NodeKind::BackRef:
            add(Op::BackRef, static_cast<std::uint8_t>(direction | (n.ignoreCase ? kIgnoreCase : 0)), static_cast<std::int32_t>(n.value), static_cast<std::int32_t>(n.count));
            return;
          case NodeKind::Repeat:
            emitRepeat(n, backward);
            return;
        }
      }

      void emitRepeat(const Node& n, bool backward) {
        std::uint8_t direction = backward ? kBackward : 0;
        std::uint8_t lazy = n.greedy ? 0 : kLazy;
        if (n.max == 0) return;
        if (n.min == kInfinite) {
          add(Op::Fail);
          return;
        }
        std::uint32_t child = n.children[0];
        if (n.min == 1 && n.max == 1) {
          emit(child, backward);
          return;
        }
        Op matcher = Op::Fail;
        std::int32_t operand = 0;
        if (charMatcher(child, matcher, operand)) {
          std::size_t at = add(Op::RepeatChar, static_cast<std::uint8_t>(direction | lazy), operand, static_cast<std::int32_t>(n.min), static_cast<std::int32_t>(n.max));
          program_.code[at].sub = matcher;
          return;
        }
        bool empty = canBeEmpty(child);
        bool captures = n.firstGroup < n.groupEnd;
        auto body = [&](std::int32_t positionRegister) {
          if (positionRegister >= 0) add(Op::MarkPos, 0, positionRegister);
          if (captures) add(Op::ResetCaps, 0, static_cast<std::int32_t>(n.firstGroup), static_cast<std::int32_t>(n.groupEnd));
          emit(child, backward);
        };
        if (n.min == 0 && (n.max == 1 || n.max == kInfinite)) {
          std::int32_t positionRegister = empty ? allocateRegister() : -1;
          std::int32_t head = here();
          std::size_t fork = add(n.greedy ? Op::Fork : Op::ForkPrefer);
          body(positionRegister);
          if (positionRegister >= 0) add(Op::CheckProgress, 0, positionRegister);
          if (n.max == kInfinite) add(Op::Jump, 0, head);
          program_.code[fork].a = here();
          return;
        }
        if (n.min == 1 && n.max == kInfinite && !empty) {
          std::int32_t head = here();
          body(-1);
          add(n.greedy ? Op::ForkPrefer : Op::Fork, 0, head);
          return;
        }
        std::int32_t counter = allocateRegister();
        std::int32_t positionRegister = empty ? allocateRegister() : -1;
        add(Op::LoopInit, 0, counter);
        std::int32_t head = here();
        std::size_t check = add(Op::LoopCheck, lazy, counter, static_cast<std::int32_t>(n.min), static_cast<std::int32_t>(n.max));
        body(positionRegister);
        add(Op::LoopTail, 0, counter, positionRegister, static_cast<std::int32_t>(n.min), head);
        program_.code[check].d = here();
      }

      // ---- search acceleration ----
      void addFirstUnit(FirstInfo& info, std::uint32_t unit) const {
        if (unit < 256) info.units[unit >> 6] |= 1ull << (unit & 63);
        else info.high = true;
      }

      void addFirstCodePoint(FirstInfo& info, std::uint32_t c) const {
        if (c >= 0x10000) addFirstUnit(info, 0xD800 + ((c - 0x10000) >> 10));
        else addFirstUnit(info, c);
      }

      FirstInfo first(std::uint32_t index) {
        const Node& n = node(index);
        FirstInfo info;
        switch (n.kind) {
          case NodeKind::Char:
            info.empty = false;
            if (n.ignoreCase) {
              caseEquivalents(n.value, program_.unicode, scratch_);
              for (std::uint32_t c : scratch_) addFirstCodePoint(info, c);
            } else {
              addFirstCodePoint(info, n.value);
            }
            return info;
          case NodeKind::Class: {
            info.empty = false;
            for (const Range& range : parser_.classSets_[n.value].ranges()) {
              for (std::uint32_t c = range.lo; c <= range.hi && c < 256; ++c) addFirstUnit(info, c);
              if (range.hi >= 256) info.high = true;
            }
            return info;
          }
          case NodeKind::Dot: case NodeKind::Any:
            info.empty = false;
            info.all = true;
            return info;
          case NodeKind::Seq:
            for (std::uint32_t child : n.children) {
              FirstInfo part = first(child);
              merge(info, part);
              if (!part.empty) {
                info.empty = false;
                return info;
              }
            }
            info.empty = true;
            return info;
          case NodeKind::Alt:
            info.empty = false;
            for (std::uint32_t child : n.children) {
              FirstInfo part = first(child);
              merge(info, part);
              if (part.empty) info.empty = true;
            }
            return info;
          case NodeKind::Group:
            return first(n.children[0]);
          case NodeKind::Repeat:
            if (n.max == 0) return info;
            info = first(n.children[0]);
            if (n.min == 0) info.empty = true;
            return info;
          case NodeKind::BackRef:
            info.all = true;
            return info;
          default:
            return info;
        }
      }

      static void merge(FirstInfo& into, const FirstInfo& from) {
        into.all = into.all || from.all;
        into.high = into.high || from.high;
        for (int i = 0; i < 4; ++i) into.units[i] |= from.units[i];
      }

      bool anchored(std::uint32_t index) const {
        const Node& n = node(index);
        switch (n.kind) {
          case NodeKind::Assert:
            return static_cast<Op>(n.value) == Op::AssertStart;
          case NodeKind::Seq:
            return !n.children.empty() && anchored(n.children[0]);
          case NodeKind::Alt:
            for (std::uint32_t child : n.children)
              if (!anchored(child)) return false;
            return true;
          case NodeKind::Group:
            return anchored(n.children[0]);
          default:
            return false;
        }
      }

      void analyse(std::uint32_t root) {
        program_.anchored = anchored(root);
        FirstInfo info = first(root);
        if (!info.empty && !info.all) {
          program_.firstFilter = true;
          for (int i = 0; i < 4; ++i) program_.firstUnits[i] = info.units[i];
          program_.firstHigh = info.high;
        }
        const Node& top = node(root);
        std::vector<std::uint32_t> leading;
        if (top.kind == NodeKind::Seq) leading = top.children;
        else leading.push_back(root);
        for (std::uint32_t child : leading) {
          if (!stringable(child)) break;
          appendUnits(program_.prefix, node(child).value);
        }
        if (program_.prefix.size() < 2) program_.prefix.clear();
        bool ascii = !program_.prefix.empty();
        for (char16_t unit : program_.prefix) ascii = ascii && unit < 128;
        if (ascii)
          for (char16_t unit : program_.prefix) program_.asciiPrefix += static_cast<char>(unit);
      }

      Program& program_;
      const Parser& parser_;
      std::vector<std::int32_t> groupOpenRegister_;
      std::vector<std::uint32_t> scratch_;
    };

    // RegExp.prototype.source as JavaScriptCore escapes it.
    std::string escapeSource(std::u16string_view pattern) {
      std::string out;
      if (pattern.empty()) return "(?:)";
      bool previousWasBackslash = false, inBrackets = false;
      for (std::size_t i = 0; i < pattern.size(); ++i) {
        std::uint32_t c = pattern[i];
        if (!previousWasBackslash) {
          if (inBrackets) {
            if (c == ']') inBrackets = false;
          } else if (c == '/') {
            out += '\\';
          } else if (c == '[') {
            inBrackets = true;
          }
        }
        if (isLineTerminator(c)) {
          if (!previousWasBackslash) out += '\\';
          out += c == 0x0A ? "n" : c == 0x0D ? "r" : c == 0x2028 ? "u2028" : "u2029";
        } else if (isHighSurrogate(c) && i + 1 < pattern.size() && isLowSurrogate(pattern[i + 1])) {
          appendUtf8(out, combineSurrogates(c, pattern[i + 1]));
          ++i;
          previousWasBackslash = false;
          continue;
        } else {
          appendUtf8(out, c);
        }
        previousWasBackslash = previousWasBackslash ? false : c == '\\';
      }
      return out;
    }
  }
  std::shared_ptr<const Program> compile(std::string_view pattern, std::string_view flags) {
    auto program = std::make_shared<Program>();
    for (char f : flags) {
      bool* slot = nullptr;
      switch (f) {
        case 'd': slot = &program->hasIndices; break;
        case 'g': slot = &program->global; break;
        case 'i': slot = &program->ignoreCase; break;
        case 'm': slot = &program->multiline; break;
        case 's': slot = &program->dotAll; break;
        case 'u': slot = &program->unicode; break;
        case 'v': slot = &program->unicodeSets; break;
        case 'y': slot = &program->sticky; break;
        default: break;
      }
      if (!slot || *slot) throw SyntaxError("Invalid flags supplied to RegExp constructor.");
      *slot = true;
    }
    if (program->unicode && program->unicodeSets) throw SyntaxError("Invalid flags supplied to RegExp constructor.");
    if (program->unicodeSets) throw SyntaxError("Invalid regular expression: rt::Regex does not support the v flag");
    if (program->hasIndices) program->flags += 'd';
    if (program->global) program->flags += 'g';
    if (program->ignoreCase) program->flags += 'i';
    if (program->multiline) program->flags += 'm';
    if (program->dotAll) program->flags += 's';
    if (program->unicode) program->flags += 'u';
    if (program->sticky) program->flags += 'y';

    std::u16string units = toUtf16(pattern);
    program->source = escapeSource(units);
    bool unicode = program->unicode;
    Parser first(units, unicode, program->ignoreCase, program->multiline, program->dotAll, PassConfig{});
    std::uint32_t root = first.parse();
    // YarrParser::handleIllegalReferences, in its order.
    PassConfig config;
    bool reparse = false;
    if (first.maxBackReference_ > first.captureCount()) {
      if (unicode) fail("invalid backreference for Unicode pattern");
      config.backReferenceLimit = first.captureCount();
      reparse = true;
    }
    if (first.kIdentityEscapeSeen_ && first.hasGroupNames()) fail("invalid \\k<> named backreference");
    if (std::any_of(first.forwardNames_.begin(), first.forwardNames_.end(), [&first](const std::string& name) { return !first.hasName(name); })) {
      if (unicode || first.hasGroupNames()) fail("invalid \\k<> named backreference");
      config.forwardReferencesAllowed = false;
      reparse = true;
    }
    std::optional<Parser> second;
    if (reparse) {
      second.emplace(units, unicode, program->ignoreCase, program->multiline, program->dotAll, config);
      root = second->parse();
    }
    Parser& parser = second ? *second : first;
    parser.resolveNamedReferences();
    program->captureCount = parser.captureCount();
    for (std::uint32_t g = 1; g < parser.groups_.size(); ++g) {
      const std::string& name = parser.groups_[g].name;
      if (name.empty()) continue;
      auto it = std::find_if(program->names.begin(), program->names.end(), [&](const NamedGroup& entry) { return entry.name == name; });
      if (it == program->names.end()) program->names.push_back(NamedGroup{name, {g}});
      else it->groups.push_back(g);
    }
    Compiler compiler(*program, parser);
    compiler.compileRoot(root);
    return program;
  }
}
