// process.env and explicit environment files. The Bun 1.4 vectors retain
// their inputs. Updated expectations are covered in DEVIATIONS-FROM-BUN.md.
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/test.hpp"
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace uos;
using namespace std::literals;
using Entries = std::vector<std::pair<std::string_view, std::string_view>>;

namespace {
  struct EnvCase {
    const char* name;
    std::string_view content;
    Entries entries;
    bool rejects = false;
  };

  // clang-format off
  const std::vector<EnvCase> CASES = {
    {"unterm_eof", "A=\"abc"sv, {}, true},
    {"unterm_nl", "A=\"abc\nB=2\n"sv, {}, true},
    {"unterm_sq", "A='abc\nB=2\n"sv, {}, true},
    {"unterm_bt", "A=`abc\nB=2\n"sv, {}, true},
    {"trail_spaces", "A=\"x\"   \nB=2"sv, {{"A"sv, "x"sv}, {"B"sv, "2"sv}}},
    {"hash_glued", "A=\"x\"#c\nB=2"sv, {{"A"sv, "x"sv}, {"B"sv, "2"sv}}},
    {"vt_ws", "A=\013 x \013\nB=2"sv, {}, true},
    {"nbsp", "A=\302\240x\302\240\nB=2"sv, {{"A"sv, "\302\240x\302\240"sv}, {"B"sv, "2"sv}}},
    {"tabs_eq", "A\t=\tv\nB=2"sv, {{"A"sv, "v"sv}, {"B"sv, "2"sv}}},
    {"colon", "A :v\nB: v\nC:\tv\nD:v\nE : w\nF:"sv, {}, true},
    {"punct_keys", "A$B=1\nA/B=2\nA+B=3\nA@B=4\nC=ok"sv, {}, true},
    {"empty_key", "=noval\nB=2"sv, {}, true},
    {"space_key", " = x\nB=2"sv, {}, true},
    {"dq_escapes", "A=\"a\\tb\\rc\\\\d\\'e\\$f\""sv, {{"A"sv, "atbrc\\d'e$f"sv}}},
    {"sq_escaped", "A='a\\'b'\nB=2"sv, {{"A"sv, "a'b"sv}, {"B"sv, "2"sv}}},
    {"bt_escaped", "A=`a\\`b`\nB=2"sv, {{"A"sv, "a`b"sv}, {"B"sv, "2"sv}}},
    {"dq_inner", "A=\"a\"b\"c\"\nB=2"sv, {}, true},
    {"exports", "export A=1\nexport\tB=2\nexport  C = 3\nexportD=4\nexport E\nexport F=6\nexport export G=7"sv, {}, true},
    {"crlf", "A=1\015\nB=2\015\n"sv, {{"A"sv, "1"sv}, {"B"sv, "2"sv}}},
    {"dq_spaced_comment", "A=\" spaced \"  # c\nB=2"sv, {{"A"sv, " spaced "sv}, {"B"sv, "2"sv}}},
    {"spaces_only_eof", "A=   "sv, {{"A"sv, ""sv}}},
    {"dup", "A=v\nA=w"sv, {}, true},
    {"comments", "#A=1\n  #B=2\nC=3 #x\nD=#y\nE=a #b #c"sv, {{"C"sv, "3"sv}, {"D"sv, ""sv}, {"E"sv, "a"sv}}},
    {"expansion", "X=1\nY=${X}\nZ=\"${X}\"\nW=$X\nV='${X}'"sv, {{"X"sv, "1"sv}, {"Y"sv, "${X}"sv}, {"Z"sv, "${X}"sv}, {"W"sv, "$X"sv}, {"V"sv, "${X}"sv}}},
    {"dq_crlf", "A=\"line1\015\nline2\"\015\nB=2"sv, {}, true},
    {"unq_spaces", "A=a b  c\nB=2"sv, {{"A"sv, "a b  c"sv}, {"B"sv, "2"sv}}},
    {"unq_backslash_n", "A=val\\nmore\nB=2"sv, {{"A"sv, "val\\nmore"sv}, {"B"sv, "2"sv}}},
    {"dq_backslash_n", "A=\"\\n\"\nB=2"sv, {{"A"sv, "n"sv}, {"B"sv, "2"sv}}},
    {"dq_escaped_close", "A=\"x\\\"\nB=2"sv, {}, true},
    {"dq_escaped_close_later", "A=\"x\\\"\nB=\"y\"\nC=3"sv, {}, true},
    {"bom", "\357\273\277A=1\nB=2"sv, {{"A"sv, "1"sv}, {"B"sv, "2"sv}}},
    {"bom_mid", "A=1\n\357\273\277B=2"sv, {}, true},
    {"empty_file", ""sv, {}},
    {"only_comment", "# x"sv, {}},
    {"sp_before_quote", "A =   \"q\"  \nB=2"sv, {{"A"sv, "q"sv}, {"B"sv, "2"sv}}},
    {"long_dq", "A=\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\nB=2"sv, {{"A"sv, "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"sv}, {"B"sv, "2"sv}}},
    {"long_dq_escapes", "A=\"ab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\n\"\nB=2"sv, {{"A"sv, "abnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabnabn"sv}, {"B"sv, "2"sv}}},
    {"long_unq", "A=yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\nB=2"sv, {{"A"sv, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"sv}, {"B"sv, "2"sv}}},
    {"long_sq", "A='zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'\nB=2"sv, {{"A"sv, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"sv}, {"B"sv, "2"sv}}},
    {"key_digits", "1=one\n12=twelve\nA=a"sv, {{"1"sv, "one"sv}, {"12"sv, "twelve"sv}, {"A"sv, "a"sv}}},
    {"lone_cr_unq", "A=a\015B=2\nC=3"sv, {}, true},
    {"no_trailing_nl", "A=1\nB=2"sv, {{"A"sv, "1"sv}, {"B"sv, "2"sv}}},
    {"dq_only_quote", "A=\"\nB=2"sv, {}, true},
    {"dq_empty_then", "A=\"\"x\nB=2"sv, {}, true},
    {"sq_hash", "A='x#y'\nB=2"sv, {{"A"sv, "x#y"sv}, {"B"sv, "2"sv}}},
    {"unq_quote_end", "A=abc\"\nB=2"sv, {{"A"sv, "abc\""sv}, {"B"sv, "2"sv}}},
    {"ws_before_key", "   A=1\n\t B=2"sv, {{"A"sv, "1"sv}, {"B"sv, "2"sv}}},
    {"ff_key", "\014A=1\nB=2"sv, {}, true},
    {"key_dash_start", "-A=1\n.B=2\n_C=3"sv, {{"-A"sv, "1"sv}, {".B"sv, "2"sv}, {"_C"sv, "3"sv}}},
    {"export_colon", "export A: 1"sv, {}, true},
    {"json_value", "A={\"a\":1,\"b\":\"c\"}\nB=2"sv, {{"A"sv, "{\"a\":1,\"b\":\"c\"}"sv}, {"B"sv, "2"sv}}},
    {"dq_json", "A=\"{\\\"a\\\":1}\"\nB=2"sv, {{"A"sv, "{\"a\":1}"sv}, {"B"sv, "2"sv}}},
    {"sq_newline_escape", "A='a\\nb'\nB=2"sv, {{"A"sv, "anb"sv}, {"B"sv, "2"sv}}},
    {"trailing_ws_line", "A=1   \n   \nB=2"sv, {{"A"sv, "1"sv}, {"B"sv, "2"sv}}},
    {"backslash_eol_dq", "A=\"a\\\nb\"\nC=3"sv, {}, true},
    {"comment_cr", "#c\015A=1\nB=2"sv, {}, true},
    {"badkey_cr", "A$=1\015B=2\nC=3"sv, {}, true},
    {"quote_junk_cr", "A=\"x\" junk\015B=2\nC=3"sv, {}, true},
    {"double_cr", "A=1\015\015\nB=2"sv, {}, true},
    {"sq_cr", "A='a\015'\nB=2"sv, {}, true},
    {"dq_crlf_end", "A=\"a\015\n\"\nB=2"sv, {}, true},
    {"lead_crlf", "\015\nA=1"sv, {{"A"sv, "1"sv}}},
    {"vt_trailing", "A=x\013\nB=2"sv, {}, true},
    {"key_cr", "A\015=1\nB=2"sv, {}, true},
    {"hash_cr", "A=1#c\015B=2\nC=3"sv, {}, true},
    {"dq_three_bs", "A=\"\\\\\\n\""sv, {{"A"sv, "\\n"sv}}},
    {"bs_eof", "A=\"abc\\"sv, {}, true},
    {"eq_eq", "A==b\nB=2"sv, {{"A"sv, "=b"sv}, {"B"sv, "2"sv}}},
    {"colon_eq", "A: b=c"sv, {}, true},
    {"colon_eof_spaces", "A:  "sv, {}, true},
    {"colon_quote", "A: \"q\""sv, {}, true},
    {"tab_eof", "A=\t"sv, {{"A"sv, ""sv}}},
    {"empty_comment", "A= # c\nB=2"sv, {{"A"sv, ""sv}, {"B"sv, "2"sv}}},
    {"cr_after_eq", "A=\015B=2\nC=3"sv, {}, true},
    {"sp_cr_after_eq", "A= \015B=2\nC=3"sv, {}, true},
    {"sq_then_hash_same_line", "A='x'#y\nB=2"sv, {{"A"sv, "x"sv}, {"B"sv, "2"sv}}},
    {"dq_with_hash_after_ws", "A=\"x\" # y \"z\"\nB=2"sv, {{"A"sv, "x"sv}, {"B"sv, "2"sv}}},
    {"key_only_eof", "A"sv, {}, true},
    {"key_eq_eof", "A="sv, {{"A"sv, ""sv}}},
    {"export_only_eof", "export A="sv, {}, true},
    {"nul_in_value", "A=a\000b\nB=2"sv, {}, true},
    {"multiple_eq_spaces", "A   =   b   \nB=2"sv, {{"A"sv, "b"sv}, {"B"sv, "2"sv}}},
    {"dq_unicode_escape", "A=\"\\u0041\""sv, {{"A"sv, "u0041"sv}}},
    {"sq_inner_dq", "A='a\"b'"sv, {{"A"sv, "a\"b"sv}}},
    {"dq_inner_sq", "A=\"a'b\""sv, {{"A"sv, "a'b"sv}}},
    {"value_tab_hash", "A=x\t#c\nB=2"sv, {{"A"sv, "x"sv}, {"B"sv, "2"sv}}},
    {"existing_override", "PATHX=from-file\nNEW=1"sv, {{"PATHX"sv, "from-file"sv}, {"NEW"sv, "1"sv}}},
    {"lf_before_eq", "A\n=1\nB=2"sv, {}, true},
    {"sp_lf_before_eq", "A \n = 1\nB=2"sv, {}, true},
    {"colon_lf", "A:\nv\nB=2"sv, {}, true},
    {"colon_cr", "A:\015v\nB=2"sv, {}, true},
    {"export_lf", "export\nA=1"sv, {}, true},
    {"export_cr", "export\015A=1"sv, {}, true},
    {"blank_lines", "\n\n  \n\tA=1"sv, {{"A"sv, "1"sv}}},
    {"vt_before_key", "A=1\n\013B=2"sv, {}, true},
    {"tabs_colon", "A\t\t:\t\tb"sv, {}, true},
    {"mixed_ws_value", "A= \t\013\014b"sv, {}, true},
    {"lf_lf_before_eq", "A\n\n=1\nB=2"sv, {}, true},
    {"key_lf_value_lf", "A\n=\n1\nB=2"sv, {}, true},
    {"dq_after_lf_eq", "A\n=\"q\"\nB=2"sv, {}, true},
    {"hash_before_eq", "A #c\n=1\nB=2"sv, {}, true},
    {"export_lf_key_eq", "export A\n=1\nB=2"sv, {}, true},
    {"sq_bs_crlf", "A='a\\\015\nb'\nC=3"sv, {}, true},
    {"dq_bs_cr", "A=\"a\\\015b\"\nC=3"sv, {}, true},
    {"dq_bs_crlf", "A=\"a\\\015\nb\"\nC=3"sv, {}, true},
    {"bt_multiline", "A=`l1\nl2`\nB=2"sv, {}, true},
    {"sq_dq_mixed", "A='x\"y'\nB=\"p'q\""sv, {{"A"sv, "x\"y"sv}, {"B"sv, "p'q"sv}}},
    {"export_tab_key", "export\tTABEXP=t"sv, {}, true},
    {"bom_only", "\357\273\277"sv, {}},
    {"bom_comment", "\357\273\277#c\nA=1"sv, {{"A"sv, "1"sv}}},
    {"value_with_eq_and_hash", "A=a=b#c"sv, {{"A"sv, "a=b"sv}}},
    {"quote_then_eof", "A=\"x\""sv, {{"A"sv, "x"sv}}},
    {"two_quotes_line", "A=\"x\" \"y\"\nB=2"sv, {}, true},
    {"dup_order", "A=1\nB=2\nA=3"sv, {}, true},
    {"digits_after_names", "B=1\n5=x\nA=2\n3=y"sv, {{"3"sv, "y"sv}, {"5"sv, "x"sv}, {"B"sv, "1"sv}, {"A"sv, "2"sv}}},
    {"crlf_quoted_next", "A='q'\015\nB=2\015\n"sv, {{"A"sv, "q"sv}, {"B"sv, "2"sv}}},
    {"lone_cr_lines", "A=1\015B=2\015C=3"sv, {}, true},
    {"colon_then_tab", "A:\tv"sv, {}, true},
    {"hash_in_key_line", "A#B=1\nC=2"sv, {}, true},
    {"empty_dq", "A=\"\"\nB=''\nC=``"sv, {{"A"sv, ""sv}, {"B"sv, ""sv}, {"C"sv, ""sv}}},
    {"unterminated_last_line", "B=2\nA='abc"sv, {}, true},
    {"file_envfile", "# comment\nPLAIN=value\nSPACED = spaced value  \nexport EXPORTED=yes\nDQ=\"double \\n quoted\"\nSQ='single \\n quoted'\nBT=`back tick`\nEMPTY=\nHASH=val # trailing comment\nHASHQ=\"val # not comment\"\nEXISTING=from-file\nMULTI=\"line1\nline2\"\nEXPAND=${PLAIN}-x\nEXPAND2=$PLAIN-y\nESC_DOLLAR=\\$PLAIN\nNOEQ\n  INDENTED=ind\nDUP=first\nDUP=second\nCOLON: notsupported\nEQ=a=b=c\nDQESC=\"a\\\"b\\\\c\\td\"\nSQEXP='${PLAIN}'\nUNICODE=\303\274n\303\257\nCRLF=crlf\nCR=withcr\015\n"sv, {}, true},
    {"file_envfile2", "\357\273\277BOMKEY=bom\nA.B=dot\nA-B=dash\n1A=digit\na b=space\nQSP=\"  inner spaces  \"\nUNTERM=\"abc\nNEXT=after-unterm\nTRAIL=\"abc\" # c\nTRAIL2=\"abc\"def\nSQML='l1\nl2'\nEMPTYQ=\"\"\nINNER=a\"b\"c\nexport\nexport ONLYEXPORT\nTABS=\ttabbed\t\nCMTONLY=  # comment only\nNOSPACEHASH=a#b\nSQHASH='x' # c\nURL=https://x.y/a\077b=c&d=e#frag\nexport   SPACED_EXPORT=se\nDQNL=\"a\\nb\\\\nc\"\nBACKSLASH_END=abc\\\nONE_DQ=\"\nTWO=2\nPEM=\"-----BEGIN PUBLIC KEY-----\\nMIIB\\n-----END PUBLIC KEY-----\"\nPEM2=-----BEGIN PUBLIC KEY-----\\nMIIB\\n-----END PUBLIC KEY-----\nLAST=last\n"sv, {}, true},
    {"file_envfile4", "SQCR='a\015\nb'\nBTCR=`a\015\nb`\nLONECR=\"a\015b\"\nexport=exp\nLEADQ=   \"lead\"\nTWOONLINE=\"a\" Y=b\nK\303\234=unicode\nBSQ=\"a\\\"\nAFTERBSQ=z\"\nEXISTEMPTY=fromfile\nFF=\014formfeed\nVT=a\013b\n   # indented comment\nK1=v1 # c\nK2=v2\nSEMI=a;b\nQ_IN_UNQ=a\"b\nEXPORTTAB=1\nexport\tTABEXP=t\nKEY_ONLY_SPACE= \nWIN=w\015\nNEXTWIN=nw\015\n"sv, {}, true},
  };
  // clang-format on

  json::array asJson(const std::vector<std::pair<std::string, std::string>>& entries) {
    json::array out;
    for (const auto& [name, value] : entries) out.push_back(json::array{json::string(name), json::string(value)});
    return out;
  }
  json::array asJson(const Entries& entries) {
    json::array out;
    for (const auto& [name, value] : entries) out.push_back(json::array{json::string(name), json::string(value)});
    return out;
  }

  std::string errorOf(auto body) {
    try {
      body();
    } catch (const rt::fs::ErrnoError& error) {
      return error.name + "|" + error.message + "|" + error.code + "|" + std::to_string(error.errno_.value_or(0)) + "|" + error.syscall + "|" + error.path.value_or("<none>");
    } catch (const rt::JsError& error) {
      return error.name + "|" + error.message;
    }
    return "no error";
  }

  struct TempDir {
    std::string path = rt::fs::mkdtempSync("/tmp/uos-rt-env-test-");
    ~TempDir() { rt::fs::rmSync(path, {.recursive = true, .force = true}); }
  };
}

TEST("the view is the OS environment at first use; JS writes never reach libc") {
  ::setenv("UOS_RT_ENV_TEST_INITIAL", "from-os", 1);
  EXPECT_EQ(rt::env::get("UOS_RT_ENV_TEST_INITIAL"), std::optional<std::string>("from-os"));
  ::setenv("UOS_RT_ENV_TEST_LATER", "x", 1);
  EXPECT_EQ(rt::env::get("UOS_RT_ENV_TEST_LATER"), std::nullopt);
  rt::env::set("UOS_RT_ENV_TEST_JS", "js");
  EXPECT(std::getenv("UOS_RT_ENV_TEST_JS") == nullptr);
  rt::env::remove("UOS_RT_ENV_TEST_INITIAL");
  EXPECT_EQ(std::string(std::getenv("UOS_RT_ENV_TEST_INITIAL")), "from-os");
  rt::env::remove("UOS_RT_ENV_TEST_JS");
  ::unsetenv("UOS_RT_ENV_TEST_INITIAL");
  ::unsetenv("UOS_RT_ENV_TEST_LATER");
}

TEST("process.env reads, writes, deletes and enumeration order") {
  rt::env::ScopedEnvironment scope({{"PATH", "/usr/bin:/bin"}, {"A", "1"}, {"B", "2"}, {"C", "3"}});
  EXPECT_EQ(rt::env::get("A"), std::optional<std::string>("1"));
  EXPECT_EQ(rt::env::get("NOPE"), std::nullopt);
  EXPECT(!rt::env::has("NOPE"));
  EXPECT(rt::env::has("A"));
  EXPECT_EQ(rt::env::get("path"), std::nullopt);
  rt::env::set("NEWVAR", "x");
  rt::env::set("ZB", "1");
  rt::env::set("5", "five");
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"5", "PATH", "A", "B", "C", "NEWVAR", "ZB"}));
  // String(42), String(undefined), String(null), String({ a: 1 }) as JS stores them.
  rt::env::set("NUM", "42");
  rt::env::set("UND", "undefined");
  EXPECT(rt::env::remove("NEWVAR"));
  EXPECT(rt::env::remove("NEVER"));
  EXPECT_EQ(rt::env::get("NEWVAR"), std::nullopt);
  rt::env::set("B", "reassigned-existing");
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"5", "PATH", "A", "B", "C", "ZB", "NUM", "UND"}));
  rt::env::set("EMPTY", "");
  EXPECT_EQ(rt::env::get("EMPTY"), std::optional<std::string>(""));
  EXPECT(rt::env::has("EMPTY"));
  rt::env::set("A=B", "v");
  EXPECT_EQ(rt::env::get("A=B"), std::optional<std::string>("v"));
  rt::env::set("NULVAL", "a\0b"sv);
  EXPECT_EQ(rt::env::get("NULVAL"), std::optional<std::string>(std::string("a\0b", 3)));
  rt::env::set("4294967294", "max index");
  rt::env::set("4294967295", "not an index");
  rt::env::set("01", "not canonical");
  rt::env::set("10", "ten");
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"5", "10", "4294967294", "PATH", "A", "B", "C", "ZB", "NUM", "UND", "EMPTY", "A=B", "NULVAL", "4294967295", "01"}));
  EXPECT(rt::env::entries().front() == (std::pair<std::string, std::string>("5", "five")));
}

TEST("ScopedEnvironment and ScopedOverride restore the previous view") {
  rt::env::ScopedEnvironment outer({{"KEEP", "k"}, {"GONE", "g"}});
  {
    rt::env::ScopedOverride updates({{"KEEP", "changed"}, {"GONE", std::nullopt}, {"ADDED", "a"}});
    EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("changed"));
    EXPECT(!rt::env::has("GONE"));
    EXPECT_EQ(rt::env::get("ADDED"), std::optional<std::string>("a"));
  }
  EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("k"));
  EXPECT_EQ(rt::env::get("GONE"), std::optional<std::string>("g"));
  EXPECT(!rt::env::has("ADDED"));
  // A deleted-then-restored name moves to the end, as with JS delete + assign.
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"KEEP", "GONE"}));
  {
    rt::env::ScopedEnvironment inner({});
    EXPECT(rt::env::keys().empty());
    rt::env::set("INNER", "i");
  }
  EXPECT(!rt::env::has("INNER"));
  EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("k"));
}

TEST("loadEnvFile handles every recorded input as a flat assignment file") {
  TempDir dir;
  int index = 0;
  for (const auto& entry : CASES) {
    const std::string file = dir.path + "/case" + std::to_string(index++) + ".env";
    rt::fs::writeFileSync(file, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(entry.content.data()), entry.content.size()));
    rt::env::ScopedEnvironment scope({});
    if (entry.rejects) {
      const std::string error = errorOf([&] { rt::env::loadEnvFile(file); });
      if (error != "SyntaxError|Invalid environment file") std::fprintf(stderr, "    case %s\n", entry.name);
      EXPECT_EQ(error, "SyntaxError|Invalid environment file");
      EXPECT(rt::env::entries().empty());
      continue;
    }
    rt::env::loadEnvFile(file);
    const auto actual = asJson(rt::env::entries());
    const auto expected = asJson(entry.entries);
    if (actual != expected) std::fprintf(stderr, "    case %s\n", entry.name);
    EXPECT_EQ(actual, expected);
  }
  EXPECT_EQ(CASES.size(), std::size_t(124));
}

TEST("loadEnvFile keeps existing variables and defaults to .env when requested") {
  TempDir dir;
  const std::string file = dir.path + "/existing.env";
  rt::fs::writeFileSync(file, "JSONLY=file\nPATHX=file\nA=3\nB=2\n");
  rt::env::ScopedEnvironment scope({{"PATHX", "from-env"}});
  rt::env::set("JSONLY", "js");
  rt::env::remove("PATHX");
  rt::env::loadEnvFile(file);
  EXPECT_EQ(asJson(rt::env::entries()), json::parse(R"([["JSONLY","js"],["PATHX","file"],["A","3"],["B","2"]])").as_array());

  rt::fs::writeFileSync(dir.path + "/.env", "FROMFILE=f\n7=seven\nPATHX=file\n");
  rt::env::ScopedEnvironment second({{"PATH", "/usr/bin:/bin"}, {"PATHX", "from-env"}});
  const std::string previous = rt::fs::realpathSync(".");
  ASSERT(::chdir(dir.path.c_str()) == 0);
  rt::env::loadEnvFile();
  ASSERT(::chdir(previous.c_str()) == 0);
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"7", "PATH", "PATHX", "FROMFILE"}));
  EXPECT_EQ(rt::env::get("PATHX"), std::optional<std::string>("from-env"));
}

TEST("parseEnvFile rejects sections, control bytes and oversized input") {
  EXPECT_THROWS_MESSAGE(rt::env::parseEnvFile("A=1\n[empty]\n"), rt::SyntaxError, "Invalid environment file");
  EXPECT_THROWS_MESSAGE(rt::env::parseEnvFile("A=x\x7fy"sv), rt::SyntaxError, "Invalid environment file");
  EXPECT_THROWS_MESSAGE(rt::env::parseEnvFile(std::string(256 * 1024 + 1, ' ')), rt::RangeError, "Environment file is too large");
  EXPECT_EQ(asJson(rt::env::parseEnvFile("; comment\nKEY='value' # comment\n")), json::parse(R"([["KEY","value"]])").as_array());
}

TEST("loadEnvFile errors are Bun's readFileSync errors") {
  TempDir dir;
  EXPECT_EQ(errorOf([&] { rt::env::loadEnvFile(dir.path + "/nope/x.env"); }),
            "Error|ENOENT: no such file or directory, open '" + dir.path + "/nope/x.env'|ENOENT|-2|open|" + dir.path + "/nope/x.env");
  EXPECT_EQ(errorOf([&] { rt::env::loadEnvFile(dir.path); }), "Error|EISDIR: illegal operation on a directory, read|EISDIR|-21|read|<none>");
  EXPECT_EQ(errorOf([&] { rt::env::loadEnvFile(""); }), "Error|ENOENT: no such file or directory, open|ENOENT|-2|open|<none>");
}
