// The dotenv corpus is fixture data for the runtime snapshot boundary.
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/test.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace uos;
using namespace std::literals;

namespace {
  struct EnvCase {
    const char* name;
    std::string_view content;
  };

  // clang-format off
  const std::vector<EnvCase> CASES = {
    {"unterm_eof", "A=\"abc"sv},
    {"unterm_nl", "A=\"abc\nB=2\n"sv},
    {"unterm_sq", "A='abc\nB=2\n"sv},
    {"unterm_bt", "A=`abc\nB=2\n"sv},
    {"trail_spaces", "A=\"x\"   \nB=2"sv},
    {"hash_glued", "A=\"x\"#c\nB=2"sv},
    {"vt_ws", "A=\013 x \013\nB=2"sv},
    {"nbsp", "A=\302\240x\302\240\nB=2"sv},
    {"tabs_eq", "A\t=\tv\nB=2"sv},
    {"colon", "A :v\nB: v\nC:\tv\nD:v\nE : w\nF:"sv},
    {"punct_keys", "A$B=1\nA/B=2\nA+B=3\nA@B=4\nC=ok"sv},
    {"empty_key", "=noval\nB=2"sv},
    {"space_key", " = x\nB=2"sv},
    {"dq_escapes", "A=\"a\\tb\\rc\\\\d\\'e\\$f\""sv},
    {"sq_escaped", "A='a\\'b'\nB=2"sv},
    {"bt_escaped", "A=`a\\`b`\nB=2"sv},
    {"dq_inner", "A=\"a\"b\"c\"\nB=2"sv},
    {"exports", "export A=1\nexport\tB=2\nexport  C = 3\nexportD=4\nexport E\nexport F=6\nexport export G=7"sv},
    {"crlf", "A=1\015\nB=2\015\n"sv},
    {"dq_spaced_comment", "A=\" spaced \"  # c\nB=2"sv},
    {"spaces_only_eof", "A=   "sv},
    {"dup", "A=v\nA=w"sv},
    {"comments", "#A=1\n  #B=2\nC=3 #x\nD=#y\nE=a #b #c"sv},
    {"expansion", "X=1\nY=${X}\nZ=\"${X}\"\nW=$X\nV='${X}'"sv},
    {"dq_crlf", "A=\"line1\015\nline2\"\015\nB=2"sv},
    {"unq_spaces", "A=a b  c\nB=2"sv},
    {"unq_backslash_n", "A=val\\nmore\nB=2"sv},
    {"dq_backslash_n", "A=\"\\n\"\nB=2"sv},
    {"dq_escaped_close", "A=\"x\\\"\nB=2"sv},
    {"dq_escaped_close_later", "A=\"x\\\"\nB=\"y\"\nC=3"sv},
    {"bom", "\357\273\277A=1\nB=2"sv},
    {"bom_mid", "A=1\n\357\273\277B=2"sv},
    {"empty_file", ""sv},
    {"only_comment", "# x"sv},
    {"sp_before_quote", "A =   \"q\"  \nB=2"sv},
    {"long_dq", "A=\"xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"\nB=2"sv},
    {"long_dq_escapes", "A=\"ab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\nab\\n\"\nB=2"sv},
    {"long_unq", "A=yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy\nB=2"sv},
    {"long_sq", "A='zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz'\nB=2"sv},
    {"key_digits", "1=one\n12=twelve\nA=a"sv},
    {"lone_cr_unq", "A=a\015B=2\nC=3"sv},
    {"no_trailing_nl", "A=1\nB=2"sv},
    {"dq_only_quote", "A=\"\nB=2"sv},
    {"dq_empty_then", "A=\"\"x\nB=2"sv},
    {"sq_hash", "A='x#y'\nB=2"sv},
    {"unq_quote_end", "A=abc\"\nB=2"sv},
    {"ws_before_key", "   A=1\n\t B=2"sv},
    {"ff_key", "\014A=1\nB=2"sv},
    {"key_dash_start", "-A=1\n.B=2\n_C=3"sv},
    {"export_colon", "export A: 1"sv},
    {"json_value", "A={\"a\":1,\"b\":\"c\"}\nB=2"sv},
    {"dq_json", "A=\"{\\\"a\\\":1}\"\nB=2"sv},
    {"sq_newline_escape", "A='a\\nb'\nB=2"sv},
    {"trailing_ws_line", "A=1   \n   \nB=2"sv},
    {"backslash_eol_dq", "A=\"a\\\nb\"\nC=3"sv},
    {"comment_cr", "#c\015A=1\nB=2"sv},
    {"badkey_cr", "A$=1\015B=2\nC=3"sv},
    {"quote_junk_cr", "A=\"x\" junk\015B=2\nC=3"sv},
    {"double_cr", "A=1\015\015\nB=2"sv},
    {"sq_cr", "A='a\015'\nB=2"sv},
    {"dq_crlf_end", "A=\"a\015\n\"\nB=2"sv},
    {"lead_crlf", "\015\nA=1"sv},
    {"vt_trailing", "A=x\013\nB=2"sv},
    {"key_cr", "A\015=1\nB=2"sv},
    {"hash_cr", "A=1#c\015B=2\nC=3"sv},
    {"dq_three_bs", "A=\"\\\\\\n\""sv},
    {"bs_eof", "A=\"abc\\"sv},
    {"eq_eq", "A==b\nB=2"sv},
    {"colon_eq", "A: b=c"sv},
    {"colon_eof_spaces", "A:  "sv},
    {"colon_quote", "A: \"q\""sv},
    {"tab_eof", "A=\t"sv},
    {"empty_comment", "A= # c\nB=2"sv},
    {"cr_after_eq", "A=\015B=2\nC=3"sv},
    {"sp_cr_after_eq", "A= \015B=2\nC=3"sv},
    {"sq_then_hash_same_line", "A='x'#y\nB=2"sv},
    {"dq_with_hash_after_ws", "A=\"x\" # y \"z\"\nB=2"sv},
    {"key_only_eof", "A"sv},
    {"key_eq_eof", "A="sv},
    {"export_only_eof", "export A="sv},
    {"nul_in_value", "A=a\000b\nB=2"sv},
    {"multiple_eq_spaces", "A   =   b   \nB=2"sv},
    {"dq_unicode_escape", "A=\"\\u0041\""sv},
    {"sq_inner_dq", "A='a\"b'"sv},
    {"dq_inner_sq", "A=\"a'b\""sv},
    {"value_tab_hash", "A=x\t#c\nB=2"sv},
    {"existing_override", "PATHX=from-file\nNEW=1"sv},
    {"lf_before_eq", "A\n=1\nB=2"sv},
    {"sp_lf_before_eq", "A \n = 1\nB=2"sv},
    {"colon_lf", "A:\nv\nB=2"sv},
    {"colon_cr", "A:\015v\nB=2"sv},
    {"export_lf", "export\nA=1"sv},
    {"export_cr", "export\015A=1"sv},
    {"blank_lines", "\n\n  \n\tA=1"sv},
    {"vt_before_key", "A=1\n\013B=2"sv},
    {"tabs_colon", "A\t\t:\t\tb"sv},
    {"mixed_ws_value", "A= \t\013\014b"sv},
    {"lf_lf_before_eq", "A\n\n=1\nB=2"sv},
    {"key_lf_value_lf", "A\n=\n1\nB=2"sv},
    {"dq_after_lf_eq", "A\n=\"q\"\nB=2"sv},
    {"hash_before_eq", "A #c\n=1\nB=2"sv},
    {"export_lf_key_eq", "export A\n=1\nB=2"sv},
    {"sq_bs_crlf", "A='a\\\015\nb'\nC=3"sv},
    {"dq_bs_cr", "A=\"a\\\015b\"\nC=3"sv},
    {"dq_bs_crlf", "A=\"a\\\015\nb\"\nC=3"sv},
    {"bt_multiline", "A=`l1\nl2`\nB=2"sv},
    {"sq_dq_mixed", "A='x\"y'\nB=\"p'q\""sv},
    {"export_tab_key", "export\tTABEXP=t"sv},
    {"bom_only", "\357\273\277"sv},
    {"bom_comment", "\357\273\277#c\nA=1"sv},
    {"value_with_eq_and_hash", "A=a=b#c"sv},
    {"quote_then_eof", "A=\"x\""sv},
    {"two_quotes_line", "A=\"x\" \"y\"\nB=2"sv},
    {"dup_order", "A=1\nB=2\nA=3"sv},
    {"digits_after_names", "B=1\n5=x\nA=2\n3=y"sv},
    {"crlf_quoted_next", "A='q'\015\nB=2\015\n"sv},
    {"lone_cr_lines", "A=1\015B=2\015C=3"sv},
    {"colon_then_tab", "A:\tv"sv},
    {"hash_in_key_line", "A#B=1\nC=2"sv},
    {"empty_dq", "A=\"\"\nB=''\nC=``"sv},
    {"unterminated_last_line", "B=2\nA='abc"sv},
    {"file_envfile", "# comment\nPLAIN=value\nSPACED = spaced value  \nexport EXPORTED=yes\nDQ=\"double \\n quoted\"\nSQ='single \\n quoted'\nBT=`back tick`\nEMPTY=\nHASH=val # trailing comment\nHASHQ=\"val # not comment\"\nEXISTING=from-file\nMULTI=\"line1\nline2\"\nEXPAND=${PLAIN}-x\nEXPAND2=$PLAIN-y\nESC_DOLLAR=\\$PLAIN\nNOEQ\n  INDENTED=ind\nDUP=first\nDUP=second\nCOLON: notsupported\nEQ=a=b=c\nDQESC=\"a\\\"b\\\\c\\td\"\nSQEXP='${PLAIN}'\nUNICODE=\303\274n\303\257\nCRLF=crlf\nCR=withcr\015\n"sv},
    {"file_envfile2", "\357\273\277BOMKEY=bom\nA.B=dot\nA-B=dash\n1A=digit\na b=space\nQSP=\"  inner spaces  \"\nUNTERM=\"abc\nNEXT=after-unterm\nTRAIL=\"abc\" # c\nTRAIL2=\"abc\"def\nSQML='l1\nl2'\nEMPTYQ=\"\"\nINNER=a\"b\"c\nexport\nexport ONLYEXPORT\nTABS=\ttabbed\t\nCMTONLY=  # comment only\nNOSPACEHASH=a#b\nSQHASH='x' # c\nURL=https://x.y/a\077b=c&d=e#frag\nexport   SPACED_EXPORT=se\nDQNL=\"a\\nb\\\\nc\"\nBACKSLASH_END=abc\\\nONE_DQ=\"\nTWO=2\nPEM=\"-----BEGIN PUBLIC KEY-----\\nMIIB\\n-----END PUBLIC KEY-----\"\nPEM2=-----BEGIN PUBLIC KEY-----\\nMIIB\\n-----END PUBLIC KEY-----\nLAST=last\n"sv},
    {"file_envfile4", "SQCR='a\015\nb'\nBTCR=`a\015\nb`\nLONECR=\"a\015b\"\nexport=exp\nLEADQ=   \"lead\"\nTWOONLINE=\"a\" Y=b\nK\303\234=unicode\nBSQ=\"a\\\"\nAFTERBSQ=z\"\nEXISTEMPTY=fromfile\nFF=\014formfeed\nVT=a\013b\n   # indented comment\nK1=v1 # c\nK2=v2\nSEMI=a;b\nQ_IN_UNQ=a\"b\nEXPORTTAB=1\nexport\tTABEXP=t\nKEY_ONLY_SPACE= \nWIN=w\015\nNEXTWIN=nw\015\n"sv},
  };
  // clang-format on

  struct TempDir {
    std::string path = rt::fs::mkdtempSync("/tmp/uos-rt-env-test-");
    ~TempDir() { rt::fs::rmSync(path, {.recursive = true, .force = true}); }
  };

  struct ScopedProcessValue {
    const char* name;
    std::optional<std::string> previous;

    ScopedProcessValue(const char* key, const std::string& value) : name(key) {
      if (const char* current = std::getenv(name)) previous = current;
      ASSERT(::setenv(name, value.c_str(), 1) == 0);
    }

    ~ScopedProcessValue() {
      const int status = previous ? ::setenv(name, previous->c_str(), 1) : ::unsetenv(name);
      if (status != 0) std::abort();
    }
  };

  struct DotenvFixture {
    TempDir root;
    const std::string home = root.path + "/home";
    const std::string previous = rt::fs::realpathSync(".");
    ScopedProcessValue homeVariable{"HOME", home};

    DotenvFixture() {
      rt::fs::mkdirSync(home);
      ASSERT(::chdir(root.path.c_str()) == 0);
    }

    ~DotenvFixture() {
      if (::chdir(previous.c_str()) != 0) std::abort();
    }

    void write(std::string_view inCwd, std::string_view inHome) const {
      rt::fs::writeFileSync(".env", bytes(inCwd));
      rt::fs::writeFileSync(home + "/.env", bytes(inHome));
    }

    static std::span<const std::uint8_t> bytes(std::string_view content) {
      return {reinterpret_cast<const std::uint8_t*>(content.data()), content.size()};
    }
  };
}

TEST("the first-use OS snapshot ignores working-directory and home dotenv files") {
  DotenvFixture files;
  files.write("UOS_RT_ENV_TEST_INITIAL=from-cwd\nUOS_AI_TOKEN=synthetic-file-token\n",
              "UOS_RT_ENV_TEST_EMPTY=from-home\nDENO_TIMELINE=synthetic-file-timeline\n");
  const ScopedProcessValue initial("UOS_RT_ENV_TEST_INITIAL", "from-os");
  const ScopedProcessValue empty("UOS_RT_ENV_TEST_EMPTY", "");
  EXPECT_EQ(rt::env::get("UOS_RT_ENV_TEST_INITIAL"), std::optional<std::string>("from-os"));
  EXPECT_EQ(rt::env::get("UOS_RT_ENV_TEST_EMPTY"), std::optional<std::string>(""));
  EXPECT(!rt::env::has("UOS_AI_TOKEN"));
  EXPECT(!rt::env::has("DENO_TIMELINE"));
  const ScopedProcessValue later("UOS_RT_ENV_TEST_LATER", "x");
  EXPECT_EQ(rt::env::get("UOS_RT_ENV_TEST_LATER"), std::nullopt);
  rt::env::set("UOS_RT_ENV_TEST_JS", "js");
  EXPECT(std::getenv("UOS_RT_ENV_TEST_JS") == nullptr);
  rt::env::remove("UOS_RT_ENV_TEST_INITIAL");
  ASSERT(std::getenv("UOS_RT_ENV_TEST_INITIAL") != nullptr);
  EXPECT_EQ(std::string(std::getenv("UOS_RT_ENV_TEST_INITIAL")), "from-os");
  rt::env::remove("UOS_RT_ENV_TEST_JS");
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
  rt::env::ScopedEnvironment outer({{"KEEP", "overwritten"}, {"GONE", "g"}, {"KEEP", "k"}, {"EMPTY", ""}});
  EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("k"));
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"KEEP", "GONE", "EMPTY"}));
  {
    rt::env::ScopedOverride updates({{"KEEP", "changed"}, {"GONE", std::nullopt}, {"ADDED", "a"}});
    EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("changed"));
    EXPECT(!rt::env::has("GONE"));
    EXPECT_EQ(rt::env::get("ADDED"), std::optional<std::string>("a"));
    {
      const rt::env::ScopedOverride nested({{"KEEP", std::nullopt}, {"KEEP", ""}, {"EMPTY", "temporary"}, {"ADDED", std::nullopt}});
      EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>(""));
      EXPECT_EQ(rt::env::get("EMPTY"), std::optional<std::string>("temporary"));
      EXPECT(!rt::env::has("ADDED"));
    }
    EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("changed"));
    EXPECT_EQ(rt::env::get("EMPTY"), std::optional<std::string>(""));
    EXPECT_EQ(rt::env::get("ADDED"), std::optional<std::string>("a"));
  }
  EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("k"));
  EXPECT_EQ(rt::env::get("GONE"), std::optional<std::string>("g"));
  EXPECT(!rt::env::has("ADDED"));
  // A deleted-then-restored name moves to the end, as with JS delete + assign.
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"EMPTY", "KEEP", "GONE"}));
  {
    rt::env::ScopedEnvironment inner({});
    EXPECT(rt::env::keys().empty());
    rt::env::set("INNER", "i");
  }
  EXPECT(!rt::env::has("INNER"));
  EXPECT_EQ(rt::env::get("KEEP"), std::optional<std::string>("k"));
}

TEST("the recorded dotenv inputs leave the runtime view intact") {
  DotenvFixture files;
  const rt::env::ScopedEnvironment scope({{"HOME", files.home}, {"A", "explicit"}, {"B", ""}});
  const auto expected = rt::env::entries();
  for (const auto& entry : CASES) {
    files.write(entry.content, entry.content);
    const auto actual = rt::env::entries();
    if (actual != expected) std::fprintf(stderr, "    case %s\n", entry.name);
    EXPECT_EQ(actual, expected);
  }
  EXPECT_EQ(CASES.size(), std::size_t(124));
}

TEST("only the explicit view supplies values when files have conflicting assignments") {
  DotenvFixture files;
  files.write("JSONLY=file\nPATHX=file\nA=3\nB=2\n", "FROMFILE=f\n7=seven\nPATHX=home\n");
  const rt::env::ScopedEnvironment scope({{"HOME", files.home}, {"PATHX", "from-env"}});
  rt::env::set("JSONLY", "js");
  EXPECT_EQ(rt::env::get("JSONLY"), std::optional<std::string>("js"));
  EXPECT_EQ(rt::env::get("PATHX"), std::optional<std::string>("from-env"));
  rt::env::remove("PATHX");
  EXPECT_EQ(rt::env::get("PATHX"), std::nullopt);
  EXPECT_EQ(rt::env::keys(), std::vector<std::string>({"HOME", "JSONLY"}));
}

TEST("dotenv sections, control bytes and oversized inputs have no effect") {
  DotenvFixture files;
  const rt::env::ScopedEnvironment scope({{"HOME", files.home}});
  const auto expected = rt::env::entries();
  files.write("A=1\n[empty]\n", "A=x\x7fy"sv);
  EXPECT_EQ(rt::env::entries(), expected);
  files.write(std::string(256 * 1024 + 1, ' '), "; comment\nKEY='value' # comment\n");
  EXPECT_EQ(rt::env::entries(), expected);
}

TEST("environment lookup works with absent and directory-valued dotenv paths") {
  DotenvFixture files;
  const rt::env::ScopedEnvironment scope({{"HOME", files.home}, {"A", "explicit"}});
  const auto expected = rt::env::entries();
  EXPECT(!rt::fs::existsSync(".env"));
  EXPECT_EQ(rt::env::entries(), expected);
  rt::fs::mkdirSync(".env");
  rt::fs::mkdirSync(files.home + "/.env");
  EXPECT_EQ(rt::env::get("A"), std::optional<std::string>("explicit"));
  EXPECT_EQ(rt::env::entries(), expected);
}
