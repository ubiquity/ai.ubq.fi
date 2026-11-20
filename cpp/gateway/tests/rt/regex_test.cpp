// rt::Regex against JavaScriptCore. Differential vectors (tests/rt/regex_vectors.inc) were
// recorded from Bun 1.4.0; the hand-written cases below cover API contracts the vectors do not.
//
// Vector format: each group is one JSON document [name, cases]. Every JSON string holds the
// WTF-8 bytes of a JS string, one character (U+0000..U+00FF) per byte. A case is either
//   [pattern, flags, {"e": message}]                      construction throws SyntaxError
//   [pattern, flags, source|0, flags|0, captures, subjects] (0 = same as the input)
// and a subject is an object: "s" the subject (or {"r": [[piece, count]...]} = repeated pieces),
// "x" re.exec(s) (absent = null), "a" [...s.matchAll(re)] (g only; absent = none),
// "p" s.split(re) (absent = [s]), "t" s.replace(re, template) and "c" s.replace(re, callback)
// (absent = s), "f" exec results for lastIndex = 0..length+1 (g/y, short subjects),
// "l" s.split(re, 0..3). A match is [[start, end] | null...] in UTF-16 units, or
// {"m": spans, "g": [[name, span|null]...]} with named groups. Long results are stored as
// {"h": FNV-1a 64 hex, "n": count} of the serialisations built by matchText/piecesHash.
// Groups: inventory (every regex in src/, serve.ts, lib/jev_compaction/ and the dynamic RegExp
// sites, with realistic and generated subjects), tests (literals in tests/), battery (semantics),
// syntax (errors, JSC error precedence, source escaping), fuzz (random patterns). Two groups use
// their own layout: "properties" = [delta-encoded sample, [pattern, flags, hash]...], the hash of
// sample.replace(re, "") for every \p{..} JSC accepts; "case" = [flags, members, block hashes]
// for every case-insensitive equivalence class (see runProperties and runCase).
#include "uos/rt/regex.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/test.hpp"
#include "src/rt/regex_internal.hpp"
#include "tests/helpers/advancing_regex_clock.hpp"
#include "third_party/quickjs-libregexp/libregexp.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;

  const char* const kVectorGroups[] = {
#include "tests/rt/regex_vectors.inc"
  };

  // ---- Test-local codecs, independent of the engine's -------------------------------------
  std::string bytesOf(const json::string& text) {
    std::string out;
    const auto* p = reinterpret_cast<const unsigned char*>(text.data());
    for (std::size_t i = 0; i < text.size();) {
      if (p[i] < 0x80) {
        out += static_cast<char>(p[i]);
        i += 1;
      } else {
        out += static_cast<char>(((p[i] & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu));
        i += 2;
      }
    }
    return out;
  }

  std::u16string toUnits(std::string_view s) {
    std::u16string out;
    for (std::size_t i = 0; i < s.size();) {
      auto b = static_cast<unsigned char>(s[i]);
      auto at = [&](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(s[i + k]) & 0x3Fu); };
      std::uint32_t c;
      if (b < 0x80) {
        c = b, i += 1;
      } else if (b < 0xE0) {
        c = ((b & 0x1Fu) << 6) | at(1), i += 2;
      } else if (b < 0xF0) {
        c = ((b & 0x0Fu) << 12) | (at(1) << 6) | at(2), i += 3;
      } else {
        c = ((b & 0x07u) << 18) | (at(1) << 12) | (at(2) << 6) | at(3), i += 4;
      }
      if (c >= 0x10000) {
        out += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
        out += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
      } else {
        out += static_cast<char16_t>(c);
      }
    }
    return out;
  }

  std::string fromUnits(std::u16string_view units) {
    std::string out;
    for (std::size_t i = 0; i < units.size(); ++i) {
      std::uint32_t c = units[i];
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < units.size() && units[i + 1] >= 0xDC00 && units[i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (units[i + 1] - 0xDC00u);
        ++i;
      }
      if (c < 0x80) {
        out += static_cast<char>(c);
      } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | (c >> 6)), out += static_cast<char>(0x80 | (c & 0x3F));
      } else if (c < 0x10000) {
        out += static_cast<char>(0xE0 | (c >> 12)), out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
      } else {
        out += static_cast<char>(0xF0 | (c >> 18)), out += static_cast<char>(0x80 | ((c >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F)), out += static_cast<char>(0x80 | (c & 0x3F));
      }
    }
    return out;
  }

  std::string slice(const std::u16string& units, std::size_t start, std::size_t end) { return fromUnits(std::u16string_view(units).substr(start, end - start)); }

  // Byte offset of UTF-16 index k in the canonical WTF-8 of `units`; inside a pair it rounds down.
  std::size_t byteOffset(const std::u16string& units, std::size_t k) {
    if (k > 0 && k < units.size() && units[k] >= 0xDC00 && units[k] <= 0xDFFF && units[k - 1] >= 0xD800 && units[k - 1] <= 0xDBFF) --k;
    return slice(units, 0, k).size();
  }

  std::string subjectOf(const json::value& spec) {
    if (spec.is_string()) return bytesOf(spec.get_string());
    std::string out;
    for (const json::value& piece : spec.at("r").as_array()) {
      std::string text = bytesOf(piece.as_array().at(0).as_string());
      for (std::int64_t n = piece.as_array().at(1).as_int64(); n > 0; --n) out += text;
    }
    return out;
  }

  std::uint64_t fnv(std::string_view bytes, std::uint64_t h = 0xcbf29ce484222325ull) {
    for (char c : bytes) h = (h ^ static_cast<unsigned char>(c)) * 0x100000001b3ull;
    return h;
  }
  std::string hex64(std::uint64_t h) {
    char text[17];
    std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
    return text;
  }

  // Only the outcomes changed by libregexp are overlaid on the original Bun vectors.
  void applyLibraryExpectations(json::value& doc, std::string_view original) {
    static const json::value changes = json::parse(
#include "tests/legacy/regex_quickjs.inc"
    );
    json::array& group = doc.as_array();
    const auto* change = changes.as_object().if_contains(group.at(0).as_string());
    if (!change) return;
    ASSERT_EQ(hex64(fnv(original)), bytesOf(change->at("hash").as_string()));
    auto& cases = group.at(1).as_array();
    const auto& expectations = change->as_object();
    if (const auto* errors = expectations.if_contains("errors")) {
      for (const auto& error : errors->as_array()) {
        const auto& message = error.at(0).as_string();
        for (const auto& index : error.at(1).as_array()) {
          auto& row = cases.at(static_cast<std::size_t>(index.as_int64())).as_array();
          row.resize(3);
          row[2] = json::object{{"e", message}};
        }
      }
    }
    if (const auto* accepted = expectations.if_contains("accepted")) {
      for (const auto& entry : accepted->as_array()) {
        auto& row = cases.at(static_cast<std::size_t>(entry.at(0).as_int64())).as_array();
        row[2] = entry.at(1);
        row.push_back(entry.at(2));
        row.push_back(entry.at(3));
        row.push_back(json::array{});
      }
    }
    if (const auto* properties = expectations.if_contains("properties")) {
      for (const auto& entry : properties->as_array())
        cases.at(static_cast<std::size_t>(entry.at(0).as_int64())).as_array().at(2) = entry.at(1);
    }
  }

  std::string display(std::string_view s) {
    std::string out = "\"";
    for (char c : s) {
      auto b = static_cast<unsigned char>(c);
      if (b == '"' || b == '\\') {
        out += '\\', out += c;
      } else if (b >= 0x20 && b < 0x7F) {
        out += c;
      } else {
        char hex[8];
        std::snprintf(hex, sizeof hex, "\\x%02X", b);
        out += hex;
      }
    }
    if (out.size() > 240) out = out.substr(0, 240) + "...";
    return out + "\"";
  }

  // ---- Checking ----------------------------------------------------------------------------
  struct Run {
    std::string group;
    std::string where;
    int cases = 0, subjects = 0, checks = 0, failures = 0;
    std::vector<std::string> unsupported;

    void check(bool ok, const std::string& what) {
      ++checks;
      if (ok) return;
      ++failures;
      if (failures <= 60) rt::test::report(__FILE__, __LINE__, group + ": " + where + ": " + what, false);
    }
  };

  std::string spanText(const std::optional<rt::RegexSpan>& span) { return span ? std::to_string(span->start) + "-" + std::to_string(span->end) : "u"; }
  std::string spanText(const json::value& span) {
    return span.is_null() ? "u" : std::to_string(span.at(0).as_int64()) + "-" + std::to_string(span.at(1).as_int64());
  }
  std::string matchText(const rt::RegexMatch& m) {
    std::string t;
    for (std::size_t i = 0; i < m.spans.size(); ++i) t += (i ? "," : "") + spanText(m.spans[i]);
    for (const rt::RegexGroup& g : m.groups) t += "|" + g.name + "=" + spanText(g.span);
    return t;
  }
  const json::array& spansOf(const json::value& match) { return match.is_object() ? match.at("m").as_array() : match.as_array(); }

  // Compares one match against its expected encoding (null or absent = no match).
  void checkMatch(Run& run, const std::string& api, const std::optional<rt::RegexMatch>& actual, const json::value* expected, const std::string& subject,
                  const std::u16string& units) {
    bool expectMatch = expected && !expected->is_null();
    if (actual.has_value() != expectMatch) {
      run.check(false, api + ": expected " + (expectMatch ? "a match" : "null") + ", got " + (actual ? "match " + matchText(*actual) : "null"));
      return;
    }
    if (!expectMatch) {
      run.check(true, api);
      return;
    }
    const json::array& spans = spansOf(*expected);
    std::string want, got = matchText(*actual);
    for (std::size_t i = 0; i < spans.size(); ++i) want += (i ? "," : "") + spanText(spans[i]);
    if (expected->is_object())
      for (const json::value& g : expected->at("g").as_array()) want += "|" + bytesOf(g.at(0).as_string()) + "=" + spanText(g.at(1));
    run.check(got == want, api + ": spans " + got + ", expected " + want);
    if (got != want) return;
    bool ok = actual->captures.size() == spans.size() && actual->index == actual->spans[0]->start && actual->input == subject;
    for (std::size_t i = 0; ok && i < spans.size(); ++i) {
      const std::optional<rt::RegexSpan>& span = actual->spans[i];
      if (!span) {
        ok = !actual->captures[i] && !(*actual)[i];
        continue;
      }
      ok = actual->captures[i] == slice(units, span->start, span->end) && span->byteStart == byteOffset(units, span->start) &&
           span->byteEnd == byteOffset(units, span->end);
    }
    ok = ok && actual->byteIndex == actual->spans[0]->byteStart && !(*actual)[spans.size()] && actual->endIndex() == actual->spans[0]->end;
    for (const rt::RegexGroup& g : actual->groups) {
      ok = ok && actual->group(g.name) == g.value && actual->groupSpan(g.name) == g.span;
      if (g.span) ok = ok && g.value == slice(units, g.span->start, g.span->end);
    }
    run.check(ok, api + ": captures, byte offsets or groups disagree with the spans");
  }

  void checkMatches(Run& run, const std::string& api, const std::vector<rt::RegexMatch>& actual, const json::value* expected, const std::string& subject,
                    const std::u16string& units) {
    if (expected && expected->is_object()) {
      std::string text;
      for (std::size_t i = 0; i < actual.size(); ++i) text += (i ? ";" : "") + matchText(actual[i]);
      run.check(static_cast<std::int64_t>(actual.size()) == expected->at("n").as_int64() && hex64(fnv(text)) == bytesOf(expected->at("h").as_string()),
                api + ": hashed match list differs (" + std::to_string(actual.size()) + " matches)");
      return;
    }
    std::size_t count = expected ? expected->as_array().size() : 0;
    if (actual.size() != count) {
      std::string got;
      for (const rt::RegexMatch& m : actual) got += " [" + matchText(m) + "]";
      run.check(false, api + ": " + std::to_string(actual.size()) + " matches, expected " + std::to_string(count) + ";" + got);
      return;
    }
    for (std::size_t i = 0; i < count; ++i) checkMatch(run, api + "[" + std::to_string(i) + "]", actual[i], &expected->as_array()[i], subject, units);
  }

  using Pieces = std::vector<std::optional<std::string>>;
  std::string piecesHash(const Pieces& pieces) {
    std::string bytes;
    for (const auto& piece : pieces) {
      if (piece) bytes += *piece;
      else bytes += '\xFE';
      bytes += '\xFF';
    }
    return hex64(fnv(bytes));
  }
  std::string piecesText(const Pieces& pieces) {
    std::string t = "[";
    for (std::size_t i = 0; i < pieces.size(); ++i) t += (i ? "," : "") + (pieces[i] ? display(*pieces[i]) : std::string("undefined"));
    return t + "]";
  }
  void checkPieces(Run& run, const std::string& api, const Pieces& actual, const json::value* expected, const std::string& subject) {
    if (expected && expected->is_object()) {
      run.check(static_cast<std::int64_t>(actual.size()) == expected->at("n").as_int64() && piecesHash(actual) == bytesOf(expected->at("h").as_string()),
                api + ": hashed pieces differ (" + std::to_string(actual.size()) + " pieces)");
      return;
    }
    Pieces want;
    if (!expected) {
      want.emplace_back(subject);
    } else {
      for (const json::value& v : expected->as_array()) want.push_back(v.is_null() ? std::nullopt : std::optional<std::string>(bytesOf(v.as_string())));
    }
    run.check(actual == want, api + ": " + piecesText(actual) + ", expected " + piecesText(want));
  }

  void checkString(Run& run, const std::string& api, const std::string& actual, const json::value* expected, const std::string& subject) {
    if (expected && expected->is_object()) {
      run.check(hex64(fnv(actual)) == bytesOf(expected->at("h").as_string()), api + ": hashed output differs: " + display(actual));
      return;
    }
    std::string want = expected ? bytesOf(expected->as_string()) : subject;
    run.check(actual == want, api + ": " + display(actual) + ", expected " + display(want));
  }

  // AdvanceStringIndex.
  std::size_t advance(const std::u16string& units, std::size_t index, bool unicode) {
    if (unicode && index + 1 < units.size() && units[index] >= 0xD800 && units[index] <= 0xDBFF && units[index + 1] >= 0xDC00 && units[index + 1] <= 0xDFFF)
      return index + 2;
    return index + 1;
  }

  Pieces splitAny(std::string_view s, const rt::Regex& re, std::uint32_t limit = 0xFFFFFFFFu) {
    if (re.captureCount() > 0) return rt::splitWithCaptures(s, re, limit);
    Pieces out;
    for (std::string& piece : rt::split(s, re, limit)) out.emplace_back(std::move(piece));
    return out;
  }

  // The replacement template the vector script used for the k-th subject of a case.
  std::string templateFor(std::size_t k, std::size_t length, const std::string& name) {
    std::string n = name.empty() ? "n" : name;
    if (length > 12) return "<$&$1$<" + n + ">>";
    if (k % 3 == 0) return "[$&|$`|$']";
    if (k % 3 == 1) return "[$1|$2|$10|$01|$001|$0|$00]";
    return "[$$|$|$<" + n + ">|$<zz>|$<" + n + "|$<]";
  }

  void checkMatching(Run& run, const rt::Regex& re, const json::object& entry, const std::string& s, const std::u16string& units) {
    const json::value* x = entry.if_contains("x");
    const json::value* all = entry.if_contains("a");
    std::optional<rt::RegexMatch> m = re.exec(s);
    checkMatch(run, "exec", m, x, s, units);
    bool matched = x && !x->is_null();
    run.check(re.test(s) == matched, "test");
    std::int64_t index = matched ? spansOf(*x)[0].at(0).as_int64() : -1;
    run.check(rt::search(s, re) == index, "search");
    if (!re.global()) {
      checkMatch(run, "match", rt::match(s, re), x, s, units);
    } else {
      std::vector<rt::RegexMatch> list = rt::matchAll(s, re);
      checkMatches(run, "matchAll", list, all, s, units);
      std::optional<std::vector<std::string>> strings = rt::matchGlobal(s, re);
      bool same = strings.has_value() == !list.empty() && (!strings || strings->size() == list.size());
      for (std::size_t i = 0; same && strings && i < list.size(); ++i) same = (*strings)[i] == list[i].str();
      run.check(same, "matchGlobal agrees with matchAll");
      rt::Regex loop = re;
      loop.lastIndex = 0;
      std::vector<rt::RegexMatch> stepped;
      while (std::optional<rt::RegexMatch> next = loop.exec(s)) {
        run.check(loop.lastIndex == next->endIndex(), "exec loop lastIndex");
        if (next->str().empty()) loop.lastIndex = advance(units, loop.lastIndex, re.unicode());
        stepped.push_back(std::move(*next));
        if (stepped.size() > units.size() + 1) break;
      }
      run.check(loop.lastIndex == 0, "exec loop ends with lastIndex 0");
      checkMatches(run, "exec loop", stepped, all, s, units);
    }
    if (const json::value* sweep = entry.if_contains("f")) {
      const json::array& results = sweep->as_array();
      for (std::size_t li = 0; li < results.size(); ++li) {
        rt::Regex copy = re;
        copy.lastIndex = li;
        std::optional<rt::RegexMatch> r = copy.exec(s);
        checkMatch(run, "exec at lastIndex " + std::to_string(li), r, &results[li], s, units);
        run.check(copy.lastIndex == (r ? r->endIndex() : 0), "lastIndex after exec at " + std::to_string(li));
        copy.lastIndex = li;
        bool found = copy.test(s);
        run.check(found == r.has_value() && copy.lastIndex == (r ? r->endIndex() : 0), "test at lastIndex " + std::to_string(li));
      }
    }
  }

  void checkReplaceAndSplit(Run& run, const rt::Regex& re, const json::object& entry, std::size_t k, const std::string& firstName, bool replace,
                            const std::string& s, const std::u16string& units) {
    checkPieces(run, "split", splitAny(s, re), entry.if_contains("p"), s);
    if (const json::value* limits = entry.if_contains("l"))
      for (std::uint32_t n = 0; n < 4; ++n) checkPieces(run, "split limit " + std::to_string(n), splitAny(s, re, n), &limits->as_array()[n], s);
    if (!replace) return;
    std::string tmpl = templateFor(k, units.size(), firstName);
    checkString(run, "replace " + display(tmpl), rt::replace(s, re, tmpl), entry.if_contains("t"), s);
    std::size_t captures = re.captureCount();
    auto callback = [&](const rt::RegexMatch& m) {
      std::string out = "<" + std::to_string(m.index) + ":" + m.str() + ":";
      out += captures >= 1 ? m[1].value_or("U") : "N";
      if (!firstName.empty()) out += "|" + m.group(firstName).value_or("U");
      return out + ">";
    };
    checkString(run, "replace callback", rt::replace(s, re, callback), entry.if_contains("c"), s);
    if (re.global()) {
      checkString(run, "replaceAll", rt::replaceAll(s, re, tmpl), entry.if_contains("t"), s);
      checkString(run, "replaceAll callback", rt::replaceAll(s, re, callback), entry.if_contains("c"), s);
    } else {
      bool threw = false;
      try {
        (void)rt::replaceAll(s, re, tmpl);
      } catch (const rt::TypeError& error) {
        threw = error.message == "String.prototype.replaceAll argument must not be a non-global regular expression";
      }
      run.check(threw, "replaceAll on a non-global regex throws TypeError");
    }
  }

  // The first group name in pattern order, read the way the vector script reads it.
  std::string firstGroupName(std::string_view pattern, const std::string& flags) {
    std::string plain;
    for (char f : flags)
      if (f != 'g' && f != 'y') plain += f;
    rt::Regex probe(std::string(pattern) + "|", plain);
    std::optional<rt::RegexMatch> m = probe.exec("");
    return m && !m->groups.empty() ? m->groups[0].name : "";
  }

  bool isUnsupported(const rt::SyntaxError& error) { return error.message.find("rt::Regex") != std::string::npos; }

  // Runs one vector group. `light` groups record matching only; in "fuzz" every fifth case
  // also records replace outputs.
  void runGroup(std::string_view name) {
    bool light = name == "tests" || name == "syntax";
    for (const char* text : kVectorGroups) {
      json::value doc = json::parse(text);
      if (bytesOf(doc.at(0).as_string()) == name) applyLibraryExpectations(doc, text);
      const json::array& top = doc.as_array();
      if (bytesOf(top.at(0).as_string()) != name) continue;
      Run run;
      run.group = std::string(name);
      std::size_t index = 0;
      for (const json::value& item : top.at(1).as_array()) {
        const json::array& c = item.as_array();
        std::string pattern = bytesOf(c.at(0).as_string()), flags = bytesOf(c.at(1).as_string());
        run.where = "new RegExp(" + display(pattern) + ", " + display(flags) + ")";
        ++run.cases;
        if (c.size() == 3) {
          std::string want = bytesOf(c.at(2).at("e").as_string());
          try {
            rt::Regex re(pattern, flags);
            run.check(false, "constructed, expected SyntaxError: " + want);
          } catch (const rt::SyntaxError& error) {
            run.check(error.message == want, "SyntaxError " + display(error.message) + ", expected " + display(want));
          }
          ++index;
          continue;
        }
        std::optional<rt::Regex> compiled;
        try {
          compiled.emplace(pattern, flags);
        } catch (const rt::SyntaxError& error) {
          if (isUnsupported(error)) run.unsupported.push_back("/" + pattern + "/" + flags + ": " + error.message);
          else run.check(false, "threw SyntaxError " + display(error.message));
          ++index;
          continue;
        }
        const rt::Regex& re = *compiled;
        std::string source = c.at(2).is_string() ? bytesOf(c.at(2).as_string()) : pattern;
        std::string canonical = c.at(3).is_string() ? bytesOf(c.at(3).as_string()) : flags;
        run.check(re.source() == source, "source " + display(re.source()) + ", expected " + display(source));
        run.check(re.flags() == canonical, "flags " + display(re.flags()) + ", expected " + display(canonical));
        run.check(re.toString() == "/" + source + "/" + canonical, "toString");
        run.check(re.captureCount() == static_cast<std::size_t>(c.at(4).as_int64()), "captureCount " + std::to_string(re.captureCount()));
        std::string firstName = firstGroupName(pattern, flags);
        bool replace = name != "fuzz" || index % 5 == 0;
        std::size_t k = 0;
        for (const json::value& subject : c.at(5).as_array()) {
          const json::object& entry = subject.as_object();
          std::string s = subjectOf(entry.at("s"));
          std::u16string units = toUnits(s);
          run.where = "/" + re.source() + "/" + re.flags() + " on " + display(s);
          ++run.subjects;
          checkMatching(run, re, entry, s, units);
          if (!light) checkReplaceAndSplit(run, re, entry, k, firstName, replace, s, units);
          ++k;
        }
        ++index;
      }
      std::printf("    %s: %d cases, %d subjects, %d checks, %d failures\n", run.group.c_str(), run.cases, run.subjects, run.checks, run.failures);
      for (const std::string& u : run.unsupported) std::printf("    unsupported (documented): %s\n", u.c_str());
      if (run.failures > 60) rt::test::report(__FILE__, __LINE__, std::to_string(run.failures - 60) + " further failures not shown", false);
      if (name == "inventory" || name == "tests") EXPECT(run.unsupported.empty());
      EXPECT(run.cases > 0);
      return;
    }
    rt::test::report(__FILE__, __LINE__, "no vector group " + std::string(name), false);
  }

  std::optional<json::value> findGroup(std::string_view name) {
    for (const char* text : kVectorGroups) {
      json::value doc = json::parse(text);
      if (bytesOf(doc.at(0).as_string()) == name) applyLibraryExpectations(doc, text);
      if (bytesOf(doc.as_array().at(0).as_string()) == name) return doc.as_array().at(1);
    }
    rt::test::report(__FILE__, __LINE__, "no vector group " + std::string(name), false);
    return std::nullopt;
  }

  // Code points delta-encoded in base 32: '0'-'9', 'A'-'V' end a number; 'W'-'Z', 'a'-'z', '!', '#'
  // lead it (the generator's encoding, decoded independently of regex_tables.cpp).
  std::vector<std::uint32_t> decodeDeltas(std::string_view text) {
    static const std::string_view final = "0123456789ABCDEFGHIJKLMNOPQRSTUV", more = "WXYZabcdefghijklmnopqrstuvwxyz!#";
    std::vector<std::uint32_t> out;
    std::uint32_t value = 0, previous = 0;
    for (char c : text) {
      std::size_t f = final.find(c);
      if (f != std::string_view::npos) {
        previous += value * 32 + static_cast<std::uint32_t>(f);
        out.push_back(previous);
        value = 0;
      } else {
        value = value * 32 + static_cast<std::uint32_t>(more.find(c));
      }
    }
    return out;
  }

  std::string fromCodePoints(const std::vector<std::uint32_t>& codePoints) {
    std::u16string units;
    for (std::uint32_t c : codePoints) {
      if (c >= 0x10000) {
        units += static_cast<char16_t>(0xD800 + ((c - 0x10000) >> 10));
        units += static_cast<char16_t>(0xDC00 + ((c - 0x10000) & 0x3FF));
      } else {
        units += static_cast<char16_t>(c);
      }
    }
    return fromUnits(units);
  }

  // Every \p{..} JavaScriptCore accepts under u: str.replace(re, "") over a string holding both
  // sides of every boundary of every property set, compared by hash.
  void runProperties() {
    std::optional<json::value> group = findGroup("properties");
    if (!group) return;
    const json::array& items = group->as_array();
    std::string subject = fromCodePoints(decodeDeltas(bytesOf(items.at(0).as_string())));
    subject += fromUnits(u"!\xD800!\xDBFF!\xDC00!\xDFFF!");
    int checks = 0, failures = 0;
    for (std::size_t i = 1; i < items.size(); ++i) {
      const json::array& entry = items[i].as_array();
      std::string pattern = bytesOf(entry.at(0).as_string()), flags = bytesOf(entry.at(1).as_string());
      std::string want = bytesOf(entry.at(2).as_string());
      ++checks;
      std::string got;
      try {
        rt::Regex re(pattern, flags);
        got = hex64(fnv(rt::replace(subject, re, "")));
      } catch (const rt::JsError& error) {
        got = error.toString();
      }
      if (got != want && ++failures <= 30) rt::test::report(__FILE__, __LINE__, "properties: /" + pattern + "/" + flags + ": " + got + ", expected " + want, false);
    }
    std::printf("    properties: %d sets over %zu code points, %d failures\n", checks, toUnits(subject).size(), failures);
    EXPECT(checks > 600);
  }

  // Case-insensitive matching of every member of every equivalence class, as a literal and as a
  // one-character class, against all members (hashed per block of 64).
  void runCase() {
    std::optional<json::value> group = findGroup("case");
    if (!group) return;
    for (const json::value& item : group->as_array()) {
      const json::array& entry = item.as_array();
      std::string flags = bytesOf(entry.at(0).as_string());
      bool unicode = flags.find('u') != std::string::npos;
      std::vector<std::uint32_t> candidates;
      for (const json::value& c : entry.at(1).as_array()) candidates.push_back(static_cast<std::uint32_t>(c.as_int64()));
      std::string subject = fromCodePoints(candidates);
      const json::array& hashes = entry.at(2).as_array();
      int failures = 0;
      for (std::size_t block = 0; block * 64 < candidates.size(); ++block) {
        std::uint64_t h = 0xcbf29ce484222325ull;
        for (std::size_t k = block * 64; k < candidates.size() && k < block * 64 + 64; ++k) {
          char escape[16];
          if (unicode) std::snprintf(escape, sizeof escape, "\\u{%x}", candidates[k]);
          else std::snprintf(escape, sizeof escape, "\\u%04x", candidates[k]);
          rt::Regex literal(escape, flags), single(std::string("[") + escape + "]", flags);
          h = fnv(rt::replace(subject, literal, ""), h);
          h = fnv("\xFF", h);
          h = fnv(rt::replace(subject, single, ""), h);
          h = fnv("\xFE", h);
        }
        if (hex64(h) != bytesOf(hashes.at(block).as_string()) && ++failures <= 20)
          rt::test::report(__FILE__, __LINE__, "case /" + flags + ": block " + std::to_string(block) + " (from U+" + std::to_string(candidates[block * 64]) + ") differs", false);
      }
      std::printf("    case /%s: %zu code points, %zu blocks, %d failures\n", flags.c_str(), candidates.size(), hashes.size(), failures);
    }
  }
}

TEST("differential: production regex inventory (src/, serve.ts, lib/jev_compaction/)") { runGroup("inventory"); }
TEST("differential: regex literals in tests/") { runGroup("tests"); }
TEST("differential: semantic battery") { runGroup("battery"); }
TEST("differential: syntax errors and source") { runGroup("syntax"); }
TEST("differential: random patterns") { runGroup("fuzz"); }
TEST("differential: Unicode property escapes") { runProperties(); }
TEST("differential: case-insensitive equivalence classes") { runCase(); }

// ---- API contracts (expected values recorded from Bun 1.4.0) ----------------------------------

namespace {
  std::vector<std::size_t> indexesOf(const std::vector<rt::RegexMatch>& matches) {
    std::vector<std::size_t> out;
    for (const rt::RegexMatch& m : matches) out.push_back(m.index);
    return out;
  }
}

TEST("construction: SyntaxError text, flags, limits") {
#if 0  // Original Bun diagnostics and bounds retained for reference.
  EXPECT_THROWS_MESSAGE(rt::Regex("(", ""), rt::SyntaxError, "Invalid regular expression: missing )");
  EXPECT_THROWS_MESSAGE(rt::Regex("a", "gg"), rt::SyntaxError, "Invalid flags supplied to RegExp constructor.");
  EXPECT_THROWS_MESSAGE(rt::Regex("a", "uv"), rt::SyntaxError, "Invalid flags supplied to RegExp constructor.");
  EXPECT_THROWS_MESSAGE(rt::Regex("\\2(a)", "u"), rt::SyntaxError, "Invalid regular expression: invalid backreference for Unicode pattern");
  EXPECT_THROWS_MESSAGE(rt::Regex("\\2(a)^*", "u"), rt::SyntaxError, "Invalid regular expression: nothing to repeat");
  EXPECT_THROWS_MESSAGE(rt::Regex("a{99999999999999999999}", ""), rt::SyntaxError, "Invalid regular expression: number too large in {} quantifier");
  // The v flag is the one ECMAScript feature rt::Regex does not implement (docs/rt/regex.md).
  EXPECT_THROWS_MESSAGE(rt::Regex("[a--b]", "v"), rt::SyntaxError, "Invalid regular expression: rt::Regex does not support the v flag");
  std::string captures;
  for (int i = 0; i < 32768; ++i) captures += "()";
  EXPECT_EQ(rt::Regex(captures, "").captureCount(), 32768u);
  EXPECT_THROWS_MESSAGE(rt::Regex(captures + "()", ""), rt::SyntaxError, "Invalid regular expression: too many captures");
  // JavaScriptCore allows about 30500 nested groups (stack dependent); rt::Regex stops at 500.
  std::string nested(500, '(');
  nested += std::string(500, ')');
  EXPECT_EQ(rt::Regex(nested, "").captureCount(), 500u);
  EXPECT_THROWS_MESSAGE(rt::Regex("(" + nested + ")", ""), rt::RangeError, "Out of memory: Invalid regular expression: too many nested disjunctions");
#else
  EXPECT_THROWS_MESSAGE(rt::Regex("("), rt::SyntaxError, "expecting ')'");
  EXPECT_THROWS_MESSAGE(rt::Regex("a", "gg"), rt::SyntaxError, "invalid regular expression flags");
  EXPECT_THROWS_MESSAGE(rt::Regex("a", "uv"), rt::SyntaxError, "invalid regular expression flags");
  EXPECT_THROWS_MESSAGE(rt::Regex("\\2(a)", "u"), rt::SyntaxError, "back reference out of range in regular expression");
  EXPECT_THROWS_MESSAGE(rt::Regex("\\2(a)^*", "u"), rt::SyntaxError, "back reference out of range in regular expression");
  EXPECT(!rt::Regex("a{99999999999999999999}").test("aaa"));
  EXPECT(rt::Regex("[a--b]", "v").test("a"));
  std::string captures;
  for (int i = 0; i < 254; ++i) captures += "()";
  EXPECT_EQ(rt::Regex(captures).captureCount(), 254u);
  EXPECT_THROWS_MESSAGE(rt::Regex(captures + "()"), rt::SyntaxError, "too many captures");
  while (captures.size() < rt::limits::kRegexPatternBytes) captures += "()";
  EXPECT_THROWS_MESSAGE(rt::Regex(captures), rt::SyntaxError, "too many captures");
  EXPECT_THROWS_MESSAGE(rt::Regex(captures + "()"), rt::RangeError, "regular expression pattern is too large");
  const std::string nestedCaptures = std::string(500, '(') + std::string(500, ')');
  for (const auto& pattern : {nestedCaptures, "(" + nestedCaptures + ")"}) {
    // Instrumented builds can reach the native stack guard before the capture limit.
    try {
      (void)rt::Regex(pattern);
      EXPECT(false);
    } catch (const rt::SyntaxError& error) {
      EXPECT_EQ(error.message, "too many captures");
    } catch (const rt::RangeError& error) {
      EXPECT_EQ(error.message, "stack overflow");
    }
  }
  std::string nested;
  for (int i = 0; i < 40; ++i) nested += "(?:";
  EXPECT(rt::Regex(nested + "x" + std::string(40, ')')).test("x"));
  nested.clear();
  for (int i = 0; i < 1000; ++i) nested += "(?:";
  EXPECT_THROWS_MESSAGE(rt::Regex(nested + "x" + std::string(1000, ')')), rt::RangeError, "stack overflow");
#endif
}

TEST("source, flags, toString and flag accessors") {
  rt::Regex re("a/b\n", "yusmigd");
  EXPECT_EQ(re.source(), "a\\/b\\n");
  EXPECT_EQ(re.flags(), "dgimsuy");
  EXPECT_EQ(re.toString(), "/a\\/b\\n/dgimsuy");
  EXPECT(re.hasIndices() && re.global() && re.ignoreCase() && re.multiline() && re.dotAll() && re.unicode() && re.sticky() && !re.unicodeSets());
  EXPECT_EQ(rt::Regex("", "").source(), "(?:)");
  EXPECT_EQ(rt::Regex("(a)(?<n>b)(?:c)", "").captureCount(), 2u);
}

TEST("lastIndex: test and exec on g, y and plain regexes") {
  rt::Regex g("a", "g");
  std::vector<std::pair<bool, std::size_t>> steps;
  for (int i = 0; i < 4; ++i) {
    bool found = g.test("aba");
    steps.emplace_back(found, g.lastIndex);
  }
  EXPECT(steps == (std::vector<std::pair<bool, std::size_t>>{{true, 1}, {true, 3}, {false, 0}, {true, 1}}));
  rt::Regex plain("a", "");
  plain.lastIndex = 2;
  EXPECT_EQ(plain.exec("aaa")->index, 0u);
  EXPECT_EQ(plain.lastIndex, 2u);
  rt::Regex sticky("a", "y");
  sticky.lastIndex = 1;
  EXPECT_EQ(sticky.exec("ba")->index, 1u);
  EXPECT_EQ(sticky.lastIndex, 2u);
  sticky.lastIndex = 0;
  EXPECT(!sticky.exec("ba"));
  EXPECT_EQ(sticky.lastIndex, 0u);
  g.lastIndex = 5;
  EXPECT(!g.exec("aa"));
  EXPECT_EQ(g.lastIndex, 0u);
  // const access behaves like a fresh literal: it reads lastIndex and never writes it.
  const rt::Regex& view = g;
  g.lastIndex = 1;
  EXPECT_EQ(view.exec("aaa")->index, 1u);
  EXPECT(view.test("aaa"));
  EXPECT_EQ(g.lastIndex, 1u);
  static const rt::Regex hoisted("a", "g");
  EXPECT_EQ(hoisted.exec("ba")->index, 1u);
  EXPECT_EQ(hoisted.exec("ba")->index, 1u);
  // Copies share the compiled program but not lastIndex.
  rt::Regex copy = g;
  copy.lastIndex = 0;
  EXPECT(copy.test("a") && copy.lastIndex == 1 && g.lastIndex == 1);
}

TEST("String.prototype methods and lastIndex") {
  rt::Regex g("a", "g");
  g.lastIndex = 1;
  EXPECT(indexesOf(rt::matchAll("aaa", g)) == (std::vector<std::size_t>{1, 2}));
  EXPECT_EQ(g.lastIndex, 1u);
  EXPECT(rt::matchGlobal("aaa", g) == (std::vector<std::string>{"a", "a", "a"}));
  EXPECT_EQ(g.lastIndex, 0u);
  EXPECT(!rt::matchGlobal("bbb", g).has_value());
  g.lastIndex = 1;
  EXPECT_EQ(rt::replace("aaa", g, "b"), "bbb");
  EXPECT_EQ(g.lastIndex, 0u);
  rt::Regex y("a", "y");
  y.lastIndex = 1;
  EXPECT_EQ(rt::replace("aaa", y, "b"), "aba");
  EXPECT_EQ(y.lastIndex, 2u);
  y.lastIndex = 3;
  EXPECT_EQ(rt::replace("aaa", y, "b"), "aaa");
  EXPECT_EQ(y.lastIndex, 0u);
  g.lastIndex = 2;
  EXPECT_EQ(rt::search("baa", g), 1);
  EXPECT_EQ(g.lastIndex, 2u);
  y.lastIndex = 2;
  EXPECT_EQ(rt::search("baa", y), -1);
  EXPECT_EQ(rt::search("aab", y), 0);
  EXPECT_EQ(y.lastIndex, 2u);
  y.lastIndex = 1;
  EXPECT(rt::split("babab", y) == (std::vector<std::string>{"b", "b", "b"}));
  EXPECT_EQ(y.lastIndex, 1u);
  EXPECT_THROWS_MESSAGE(rt::matchAll("a", rt::Regex("a", "")), rt::TypeError, "String.prototype.matchAll argument must not be a non-global regular expression");
  EXPECT_THROWS_MESSAGE(rt::replaceAll("a", rt::Regex("a", ""), "b"), rt::TypeError, "String.prototype.replaceAll argument must not be a non-global regular expression");
  EXPECT_THROWS(rt::match("a", g), std::logic_error);
  EXPECT_THROWS(rt::matchGlobal("a", rt::Regex("a", "")), std::logic_error);
  EXPECT_THROWS(rt::split("a1b", rt::Regex("(\\d)", "")), std::logic_error);
}

TEST("exec result: captures, groups, indices and byte offsets") {
  static const rt::Regex mail("(?<w>\\w+)@(?<d>\\w+)", "");
  std::string subject = "me@host!";
  std::optional<rt::RegexMatch> m = mail.exec(subject);
  ASSERT(m.has_value());
  EXPECT_EQ(m->size(), 3u);
  EXPECT_EQ(m->str(), "me@host");
  EXPECT_EQ(m->group("w"), std::optional<std::string>("me"));
  EXPECT_EQ(m->group("d"), std::optional<std::string>("host"));
  EXPECT_EQ(m->group("nope"), std::nullopt);
  EXPECT_EQ(m->groupSpan("d")->start, 3u);
  EXPECT_EQ((*m)[3], std::nullopt);
  EXPECT(m->input.data() == subject.data() && m->input.size() == subject.size());
  std::optional<rt::RegexMatch> alt = rt::Regex("(a)|(b)", "").exec("b");
  EXPECT_EQ((*alt)[1], std::nullopt);
  EXPECT_EQ((*alt)[2], std::optional<std::string>("b"));
  EXPECT(!alt->spans[1].has_value());
  EXPECT(alt->groups.empty());
  // "a😀b😀": UTF-16 indices 1 and 4, byte offsets 1 and 6.
  std::vector<rt::RegexMatch> astral = rt::matchAll("a\xF0\x9F\x98\x80" "b\xF0\x9F\x98\x80", rt::Regex("\xF0\x9F\x98\x80", "g"));
  ASSERT(astral.size() == 2);
  EXPECT(astral[0].index == 1 && astral[0].endIndex() == 3 && astral[0].byteIndex == 1 && astral[0].byteEndIndex() == 5);
  EXPECT(astral[1].index == 4 && astral[1].byteIndex == 6);
  // Without u a match can start inside a pair: the byte offset rounds down to the pair.
  std::optional<rt::RegexMatch> half = rt::Regex("\\uDE00", "").exec("a\xF0\x9F\x98\x80" "b");
  ASSERT(half.has_value());
  EXPECT(half->index == 2 && half->endIndex() == 3 && half->byteIndex == 1 && half->byteEndIndex() == 5);
  EXPECT_EQ(half->str(), "\xED\xB8\x80");
}

TEST("replace: callbacks, templates and canonical WTF-8 output") {
  rt::Regex either("(?<x>a)|(?<y>b)", "g");
  std::string log;
  std::string out = rt::replace("ab", either, [&log](const rt::RegexMatch& m) {
    log += std::to_string(m.index) + ":" + m.str() + ":" + m[1].value_or("U") + ":" + m.group("x").value_or("U") + m.group("y").value_or("U") + ";";
    return std::string("<") + m.str() + ">";
  });
  EXPECT_EQ(out, "<a><b>");
  EXPECT_EQ(log, "0:a:a:aU;1:b:U:Ub;");
  std::string (*upper)(const rt::RegexMatch&) = [](const rt::RegexMatch& m) { return m.str() == "a" ? std::string("A") : m.str(); };
  EXPECT_EQ(rt::replaceAll("banana", rt::Regex("a", "g"), upper), "bAnAnA");
  std::function<std::string(const rt::RegexMatch&)> wrapped = [](const rt::RegexMatch& m) { return "[" + m.str() + "]"; };
  EXPECT_EQ(rt::replace("x1y", rt::Regex("\\d", ""), wrapped), "x[1]y");
  EXPECT_EQ(rt::replace("ab", rt::Regex("(?<n>a)", ""), "[$<n>][$<m>][$<n]"), "[a][][$<n]b");
  EXPECT_EQ(rt::replace("ab", rt::Regex("(a)", ""), "[$<n>]"), "[$<n>]b");
  EXPECT_EQ(rt::replace("ab", rt::Regex("(a)", ""), "$0$00$1$01$10$2$$"), "$0$00aaa0$2$b");
  EXPECT_EQ(rt::replace("abc", rt::Regex("x*", "g"), "-"), "-a-b-c-");
  EXPECT_EQ(rt::replace("\xF0\x9F\x98\x80", rt::Regex("(?:)", "gu"), "-"), "-\xF0\x9F\x98\x80-");
  EXPECT_EQ(rt::replace("\xF0\x9F\x98\x80", rt::Regex("(?:)", "g"), "-"), "-\xED\xA0\xBD-\xED\xB8\x80-");
  // Lone surrogates that become adjacent join into one four-byte sequence, as JS strings do.
  EXPECT_EQ(rt::replace("\xED\xA0\xBDx\xED\xB8\x80", rt::Regex("x", ""), ""), "\xF0\x9F\x98\x80");
  EXPECT_EQ(rt::replace("\xED\xA0\xBDx", rt::Regex("x", ""), [](const rt::RegexMatch&) { return std::string("\xED\xB8\x80"); }), "\xF0\x9F\x98\x80");
  EXPECT_EQ(rt::replace("ax", rt::Regex("x", ""), "\xED\xB8\x80"), "a\xED\xB8\x80");
}

TEST("split: captures, limits and empty input") {
  EXPECT(rt::splitWithCaptures("a1b2c", rt::Regex("(\\d)(x)?", "")) ==
         (std::vector<std::optional<std::string>>{"a", "1", std::nullopt, "b", "2", std::nullopt, "c"}));
  EXPECT(rt::splitWithCaptures("a1b2c", rt::Regex("(\\d)", ""), 2) == (std::vector<std::optional<std::string>>{"a", "1"}));
  EXPECT(rt::split("abc", rt::Regex("(?:)", "u")) == (std::vector<std::string>{"a", "b", "c"}));
  EXPECT(rt::split("", rt::Regex("x", "")) == (std::vector<std::string>{""}));
  EXPECT(rt::split("", rt::Regex("(?:)", "")).empty());
  EXPECT(rt::split("a,b", rt::Regex(",", ""), 0).empty());
}

TEST("re-entrancy: a callback may use the regex it is called from") {
  static rt::Regex word("\\w+", "g");
  std::string out = rt::replace("ab cd", word, [](const rt::RegexMatch& m) {
    // The outer replace has already found every match; the inner calls see and move lastIndex
    // exactly as in JS (the second matchAll starts at 2, where the first test left it).
    const auto count = rt::matchAll(m.str() + " x", word).size();
    return std::to_string(count) + std::string(word.test("zz") ? "t" : "f");
  });
  EXPECT_EQ(out, "2t 1f");
  EXPECT_EQ(word.lastIndex, 0u);
  std::vector<std::thread> threads;
  std::atomic<int> agree{0};
  static const rt::Regex shared("(\\d+)-(\\d+)", "g");
  for (int t = 0; t < 2; ++t)
    threads.emplace_back([&agree] {
      for (int i = 0; i < 200; ++i)
        if (rt::replace("1-2 33-44", shared, "$2-$1") == "2-1 44-33") ++agree;
    });
  for (std::thread& thread : threads) thread.join();
  EXPECT_EQ(agree.load(), 400);
}

TEST("long backtracking searches retain accepted inputs and reject oversized subjects") {
  // Bun: /^(?:a|b)*$/.test("a".repeat(1e6)) is true, with 2e6 it is false (backtracking memory).
  static const rt::Regex alternation("^(?:a|b)*$", "");
  EXPECT(alternation.test(std::string(1000000, 'a')));
#if 0  // Original Bun behavior on an exhausted search.
  EXPECT(!alternation.test(std::string(2000000, 'a')));
#else
  EXPECT_THROWS_MESSAGE(alternation.test(std::string(2000000, 'a')), rt::RangeError, "regular expression subject is too large");
#endif
#if 0  // Original Bun behavior for a long character-class repeat.
  // Single-character repeats keep no per-character state and have no such limit.
  EXPECT(rt::Regex("^[ab]*$", "").test(std::string(4000000, 'a')));
#else
  const rt::Regex characterClass("^[ab]*$");
  EXPECT(characterClass.test(std::string(1000000, 'a')));
  EXPECT_THROWS_MESSAGE(characterClass.test(std::string(4000000, 'a')), rt::RangeError, "regular expression subject is too large");
#endif
}

TEST("regex pattern and subject byte limits admit their exact boundaries") {
  std::string pattern(rt::limits::kRegexPatternBytes, 'a');
  EXPECT(rt::Regex(pattern).test(pattern));
  pattern.push_back('a');
  EXPECT_THROWS_MESSAGE(rt::Regex(pattern), rt::RangeError, "regular expression pattern is too large");

  const rt::Regex anchored("^a");
  std::string subject(rt::limits::kRegexSubjectBytes, 'a');
  EXPECT(anchored.test(subject));
  subject.push_back('a');
  EXPECT_THROWS_MESSAGE(anchored.test(subject), rt::RangeError, "regular expression subject is too large");
}

TEST("execution budget rejects an unfinished match") {
  const rt::Regex pattern("^(?:a|aa)*b$");
  const std::string subject(52, 'a');
  const auto expected = static_cast<long>(rt::limits::kRegexWorkBudget.pollsFor(subject.size()));
  for (const auto step : {std::chrono::milliseconds(0), std::chrono::milliseconds(20)}) {
    uos::test::AdvancingRegexClock clock(step);
    EXPECT_THROWS_MESSAGE(pattern.test(subject), rt::RangeError, "regular expression execution limit exceeded");
    // The clock is read at matcher construction, at search entry, and for
    // each interrupt except the last one, which runs out of work first.
    EXPECT_EQ(clock.reads() - 1, expected);
  }
  EXPECT(pattern.test("aab"));
}

TEST("regexp engine hooks enforce the work budget and retain the allocation ceiling") {
  using Limits = rt::regex_detail::Limits;
  const auto before = Limits::Clock::now();
  Limits budget(rt::limits::kRegexMatchBackstop, 2);
  const auto after = Limits::Clock::now();
  EXPECT(budget.deadline >= before + rt::limits::kRegexMatchBackstop);
  EXPECT(budget.deadline <= after + rt::limits::kRegexMatchBackstop);
  const auto compileStart = Limits::Clock::now();
  const Limits compilation(rt::limits::kRegexCompileBackstop);
  const auto compileEnd = Limits::Clock::now();
  EXPECT(compilation.deadline >= compileStart + rt::limits::kRegexCompileBackstop);
  EXPECT(compilation.deadline <= compileEnd + rt::limits::kRegexCompileBackstop);
  const rt::Regex credentialKey(R"re(\b(?=\w*(?:key|token|secret|password))(?=(\w*))\1(?:[.-]\w*){0,16}(["']?)\s*(?=[:=]))re", "gi");
  constexpr auto bytes = rt::limits::kRegexSubjectBytes;
  EXPECT_EQ(credentialKey.program().workBudget.pollsFor(bytes), rt::limits::kSupervisorRegexWorkBudget.pollsFor(bytes));
  EXPECT_EQ(rt::Regex("key", "gi").program().workBudget.pollsFor(bytes), rt::limits::kRegexWorkBudget.pollsFor(bytes));
  budget.deadline = Limits::Clock::time_point::max();
  EXPECT_EQ(lre_check_timeout(&budget), 0);
  EXPECT_EQ(lre_check_timeout(&budget), 1);
  EXPECT_EQ(lre_check_timeout(&budget), 1);
  EXPECT(!budget.backstopExceeded);
  EXPECT_EQ(budget.pollsRemaining, 0u);
  Limits wall(rt::limits::kRegexMatchBackstop, 2);
  wall.deadline = Limits::Clock::now();
  EXPECT_EQ(lre_check_timeout(&wall), 1);
  EXPECT(wall.backstopExceeded);

  constexpr std::size_t maxAllocation = 72 * 1024 * 1024;
  const std::unique_ptr<void, decltype(&std::free)> memory(lre_realloc(&budget, nullptr, maxAllocation), std::free);
  EXPECT(memory);
  const std::unique_ptr<void, decltype(&std::free)> over(lre_realloc(&budget, nullptr, maxAllocation + 1), std::free);
  EXPECT(!over);
}

TEST("regex work within the budget survives the former time limits") {
  using namespace std::chrono_literals;
  uos::test::AdvancingRegexClock slow(20ms);
  const std::string literal(rt::limits::kRegexPatternBytes, 'a');
  EXPECT(rt::Regex(literal).test(literal));

  const rt::Regex characterClass("^[ab]*$");
  const std::string subject(rt::limits::kRegexSubjectBytes, 'a');
  const auto matchStarted = slow.elapsed();
  EXPECT(characterClass.test(subject));
  EXPECT(slow.elapsed() - matchStarted > 250ms);

  const rt::Regex dataPrefix("^data:", "iu");
  const std::string imageUrl = "https://example.com/" + std::string(5 * 1024 * 1024 + 1, 'a');
  const auto imageStarted = slow.elapsed();
  EXPECT(!dataPrefix.test(imageUrl));
  EXPECT(slow.elapsed() - imageStarted > 1800ms);

  const rt::Regex boundary(R"(\r\n\r\n|\n\n|\r\r)");
  const std::string frame = std::string(4 * 1024 * 1024, 'a') + "\r\n\r\n";
  const auto frameStarted = slow.elapsed();
  EXPECT_EQ(rt::search(frame, boundary), 4 * 1024 * 1024);
  EXPECT(slow.elapsed() - frameStarted > 1800ms);
}

TEST("regex wall-clock backstops have distinct errors") {
  const rt::Regex characterClass("^[ab]*$");
  const std::string subject(rt::limits::kRegexSubjectBytes, 'a');
  {
    uos::test::AdvancingRegexClock expired(rt::limits::kRegexMatchBackstop / 2);
    EXPECT_THROWS_MESSAGE(characterClass.test(subject), rt::RangeError, "regular expression execution wall-clock backstop exceeded");
  }
  {
    uos::test::AdvancingRegexClock expired(rt::limits::kRegexCompileBackstop);
    EXPECT_THROWS_MESSAGE(rt::Regex("a"), rt::RangeError, "regular expression compilation wall-clock backstop exceeded");
  }
}

TEST("Unicode sets, named captures, and UTF-16 indices") {
  const std::string subject = "😀-😃";
  const rt::Regex letters("(?<face>[\\q{😀|😃}])", "vyd");
  const auto first = letters.exec(subject);
  ASSERT(first.has_value());
  EXPECT_EQ(letters.flags(), "dvy");
  EXPECT(letters.unicodeSets() && !letters.unicode());
  EXPECT_EQ(first->group("face"), std::optional<std::string>("😀"));
  EXPECT_EQ(first->groupSpan("face"), (std::optional<rt::RegexSpan>{{0, 2, 0, 4}}));
  EXPECT_EQ(rt::replace(subject, rt::Regex("(?<face>\\p{Emoji})", "gv"), "$<face>$<face>"), "😀😀-😃😃");
  EXPECT_EQ(indexesOf(rt::matchAll(subject, rt::Regex("", "gv"))), (std::vector<std::size_t>{0, 2, 3, 5}));
  EXPECT_EQ(rt::split(subject, rt::Regex("[\\q{😀|😃}]", "vy")), (std::vector<std::string>{"", "-", ""}));
}

TEST("embedded NUL remains part of the compiled pattern and subject") {
  const rt::Regex pattern(std::string_view("^a\0b$", 5));
  EXPECT(pattern.test(std::string_view("a\0b", 3)));
  EXPECT(!pattern.test("a"));
  EXPECT(!pattern.test("ab"));
}

TEST("image data URL captures survive large Base64 payloads") {
  constexpr std::string_view source = R"(^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$)";
  constexpr std::string_view prefix = "DATA:IMAGE/PNG;BASE64,";
  const rt::Regex pattern(source, "iu");
  std::string subject = std::string(prefix) + std::string(4 * 1024 * 1024 + 2, 'A') + "==";
  const auto match = pattern.exec(subject);
  ASSERT(match.has_value());
  EXPECT_EQ(pattern.source(), source);
  EXPECT(!pattern.sticky());
  EXPECT_EQ(match->captures.size(), 3u);
  EXPECT_EQ(match->captures[1], std::optional<std::string>("IMAGE/PNG"));
  EXPECT_EQ(match->spans[2], (std::optional<rt::RegexSpan>{{prefix.size(), subject.size(), prefix.size(), subject.size()}}));
  EXPECT(std::string_view(*match->captures[2]) == std::string_view(subject).substr(prefix.size()));
  subject.back() = '!';
  EXPECT(!pattern.test(subject));
}

TEST("the fixed image data URL pattern retains the encoded image allowance") {
  const rt::Regex image(R"(^data:([^;,]+);base64,([a-z0-9+/]*={0,2})$)", "iu");
  std::string subject = "DATA:IMAGE/PNG;BASE64,";
  subject.resize(rt::limits::kImageRegexSubjectBytes, 'A');
  uos::test::AdvancingRegexClock slow(std::chrono::seconds(2));
  EXPECT(image.test(subject));
  subject.push_back('A');
  EXPECT_THROWS_MESSAGE(image.test(subject), rt::RangeError, "regular expression subject is too large");
}

TEST("the fixed stored base64 pattern retains its encoded byte allowance and padding rules") {
  const rt::Regex standard(R"(^(?:[A-Za-z0-9+/]{4})*(?:[A-Za-z0-9+/]{2}==|[A-Za-z0-9+/]{3}=)?$)");
  std::string subject(rt::limits::kBase64InputBytes, 'A');
  subject.back() = '=';
  uos::test::AdvancingRegexClock slow(std::chrono::seconds(2));
  EXPECT(standard.test(subject));
  subject.back() = '-';
  EXPECT(!standard.test(subject));
  subject.push_back('A');
  EXPECT_THROWS_MESSAGE(standard.test(subject), rt::RangeError, "regular expression subject is too large");
}

TEST("the base64 alphabet replacements admit encoded input at the byte limit") {
  rt::Regex dash("-", "g");
  std::string input(rt::limits::kBase64InputBytes, 'A');
  input.back() = '-';
  dash.lastIndex = 5;
  uos::test::AdvancingRegexClock slow(std::chrono::seconds(2));
  const auto translated = rt::replace(input, dash, "+");
  EXPECT_EQ(translated.size(), input.size());
  EXPECT_EQ(translated.back(), '+');
  EXPECT_EQ(dash.lastIndex, 0u);
  input.push_back('A');
  EXPECT_THROWS_MESSAGE(rt::replace(input, dash, "+"), rt::RangeError, "regular expression subject is too large");
}
