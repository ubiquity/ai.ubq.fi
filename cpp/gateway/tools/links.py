#!/usr/bin/env python3
"""Resolve the unit.sh link list of a cpp/gateway test file.

  links.py [--with-test] <test.cpp>...    the .cpp sources that follow the test file on the unit.sh
                                          command line, paths relative to cpp/gateway, one line per test
  links.py --explain <test.cpp>...        where the list comes from, plus env, skip and notes
  links.py --shell <test.cpp>             bash assignments read by tools/run-tests.sh
  links.py --check [<test.cpp>...]        resolve every test (default: all of them), report problems
  links.py --closure [--diff] <test.cpp>  minimal link set from the cached objects' symbol tables (nm,
                                          read-only), to fill the registry; --includes: from #includes
  links.py --list-tests                   every tests/**/*_test.cpp, sorted
  links.py --hashes <test.cpp>            SHA-256 of the test, linked sources, runtime and local includes

Resolution order:
  1. an entry in tests/LINKS.txt that gives a source list (the registry overrides the comment);
  2. the declaration in the test's leading comment (first 40 lines):
       // Links: <sources>                  continued on following comment lines that start with a path
       // ... Links <a>, <b> and <c>.       prose form
       // Links the sources of tests/<other>_test.cpp [but <x>.cpp and <y>.cpp].
     Brace groups (src/storage/{kv,native_kv}.cpp) are expanded; text from "(" on is prose;
  3. for tests/rt/*_test.cpp only: no extra sources.
Every src/rt/*.cpp is linked by unit.sh itself and is never printed. Format of the registry and
everything else: docs/TESTING.md.
"""
import argparse
import glob
import hashlib
import os
import re
import shlex
import sys

GATEWAY = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REGISTRY = os.path.join(GATEWAY, "tests", "LINKS.txt")
HEADER_LINES = 40
FIELDS = ("env", "skip", "note")


class LinkError(Exception):
    pass


def rel(path):
    return os.path.relpath(os.path.abspath(path), GATEWAY).replace(os.sep, "/")


def exists(relpath):
    return os.path.isfile(os.path.join(GATEWAY, relpath))


def normalize_test(arg):
    """Accept a path relative to the current directory or to cpp/gateway, absolute, or `tests/`-relative."""
    candidates = [arg, os.path.join(GATEWAY, arg), os.path.join(GATEWAY, "tests", arg)]
    if not arg.endswith(".cpp"):
        candidates += [c + ".cpp" for c in candidates]
    for candidate in candidates:
        if os.path.isfile(candidate):
            return rel(candidate)
    return rel(os.path.join(GATEWAY, arg)) if not os.path.isabs(arg) else rel(arg)


def all_tests():
    found = glob.glob(os.path.join(GATEWAY, "tests", "**", "*_test.cpp"), recursive=True)
    return sorted(rel(path) for path in found)


def expand_braces(token):
    match = re.search(r"\{([^{}]*)\}", token)
    if not match:
        return [token]
    out = []
    for alternative in match.group(1).split(","):
        out += expand_braces(token[: match.start()] + alternative + token[match.end():])
    return out


def expand_token(token):
    out = []
    for path in expand_braces(token):
        if "*" in path or "?" in path:
            if path.startswith("src/rt/"):
                continue
            out += sorted(rel(p) for p in glob.glob(os.path.join(GATEWAY, path)))
        else:
            out.append(os.path.normpath(path).replace(os.sep, "/"))
    return out


# ---------------------------------------------------------------- registry (tests/LINKS.txt)

class Entry:
    def __init__(self, test, line):
        self.test = test
        self.line = line
        self.sources = None  # None: keep the test's own declaration ("(header)")
        self.raw_sources = []
        self.env = []
        self.skip = None
        self.notes = []


def read_registry(path=REGISTRY):
    entries = {}
    if not os.path.isfile(path):
        return entries
    current = None
    with open(path, encoding="utf-8") as handle:
        for number, raw in enumerate(handle, 1):
            text = raw.rstrip("\n")
            stripped = text.strip()
            if not stripped or stripped.startswith("#"):
                continue
            if not text[0].isspace():
                match = re.match(r"^(\S+?):(?:\s+(.*))?$", text)
                if not match:
                    raise LinkError("%s:%d: expected `<test path>: <sources...>`" % (rel(path), number))
                current = Entry(match.group(1), number)
                if current.test in entries:
                    raise LinkError("%s:%d: duplicate entry for %s" % (rel(path), number, current.test))
                entries[current.test] = current
                current.raw_sources.append(match.group(2) or "")
                continue
            if current is None:
                raise LinkError("%s:%d: indented line outside an entry" % (rel(path), number))
            field = re.match(r"^\s+(%s):\s*(.*)$" % "|".join(FIELDS), text)
            if not field:
                current.raw_sources.append(stripped)
            elif field.group(1) == "env":
                for assignment in shlex.split(field.group(2)):
                    if "=" not in assignment or not re.match(r"^[A-Za-z_][A-Za-z0-9_]*=", assignment):
                        raise LinkError("%s:%d: env wants KEY=VALUE, got %r" % (rel(path), number, assignment))
                    current.env.append(assignment)
            elif field.group(1) == "skip":
                current.skip = field.group(2).strip() or "skipped in tests/LINKS.txt"
            else:
                current.notes.append(field.group(2).strip())
    for entry in entries.values():
        words = " ".join(entry.raw_sources).split()
        if words == ["(header)"]:
            entry.sources = None
        elif words in ([], ["(none)"]):
            entry.sources = []
        else:
            entry.sources = []
            for word in words:
                entry.sources += expand_token(word)
    return entries


# ---------------------------------------------------------------- header declarations

PATH_START = re.compile(r"^(?:src|tests|include)/")


class Declaration:
    def __init__(self, line):
        self.line = line
        self.sources = []
        self.inherit = None
        self.excludes = []
        self.incomplete = None


def classify(token):
    """('path', path, sentence_end, comma) | ('and',) | ('open',) | ('word',)"""
    if token.startswith("("):
        return ("open",)
    if token == "and":
        return ("and",)
    core = token.rstrip(",;:")
    comma = core != token
    sentence_end = core.endswith(".") and core.rstrip(".").endswith(".cpp")
    core = core.rstrip(".") if sentence_end else core
    if PATH_START.match(core) and core.endswith(".cpp") and re.match(r"^[A-Za-z0-9_./{},*?-]+$", core):
        return ("path", core, sentence_end, comma)
    return ("word",)


def comment_body(raw):
    match = re.match(r"^\s*//(.*)$", raw)
    return match.group(1) if match else None


def parse_declaration(lines):
    """lines: the file's leading lines. Returns a Declaration or None."""
    for index, raw in enumerate(lines[:HEADER_LINES]):
        body = comment_body(raw)
        if body is None:
            continue
        found = re.search(r"\bLinks\b(:?)", body)
        if not found:
            continue
        rest = body[found.end():]
        declaration = Declaration(index + 1)
        inherit = re.match(r"^\s+the\s+sources\s+of\s+(\S+)(.*)$", rest)
        if inherit:
            declaration.inherit = inherit.group(1).rstrip(".,;:")
            tail = inherit.group(2)
            but = re.match(r"^\s*(?:,\s*)?(?:but|except|minus|without)\s+(.*)$", tail)
            if but:
                for token in but.group(1).split():
                    core = token.rstrip(".,;:")
                    if core.endswith(".cpp"):
                        declaration.excludes.append(core)
            return declaration
        if not found.group(1) and not re.match(r"^\s+(?:src|tests)/", rest):
            continue  # "Links" as an ordinary word
        line_index = index
        text = rest
        while True:
            open_end = False
            pending_and = False
            stopped = False
            for token in text.split():
                kind = classify(token)
                if kind[0] == "path":
                    for path in expand_token(kind[1]):
                        declaration.sources.append(path)
                    pending_and = False
                    if kind[2]:
                        stopped = True
                        break
                    open_end = True
                elif kind[0] == "and":
                    pending_and = True
                    open_end = True
                elif kind[0] == "open":
                    stopped = True
                    break
                else:
                    if pending_and:
                        declaration.incomplete = "prose after `and` on line %d: %s" % (line_index + 1, text.strip())
                    stopped = True
                    break
            if stopped or not open_end:
                break
            line_index += 1
            if line_index >= len(lines):
                break
            following = comment_body(lines[line_index])
            if following is None:
                break
            first = following.split()[:1]
            if not first or not (PATH_START.match(first[0]) or first[0] == "and"):
                if pending_and:
                    declaration.incomplete = "list ends with `and` on line %d" % line_index
                break
            text = following
        return declaration
    return None


def read_head(test):
    with open(os.path.join(GATEWAY, test), encoding="utf-8", errors="replace") as handle:
        lines = []
        for line in handle:
            lines.append(line.rstrip("\n"))
            if len(lines) >= HEADER_LINES + 20:
                break
    return lines


# ---------------------------------------------------------------- resolution

class Resolution:
    def __init__(self, test):
        self.test = test
        self.origin = None
        self.sources = []
        self.env = []
        self.skip = None
        self.notes = []
        self.error = None
        self.warnings = []


def resolve(test, registry, check=True, stack=()):
    result = Resolution(test)
    if test in stack:
        raise LinkError("circular `Links the sources of` chain: %s" % " -> ".join(stack + (test,)))
    if not exists(test):
        raise LinkError("no such test file: %s" % test)
    entry = registry.get(test)
    if entry is not None:
        result.env = list(entry.env)
        result.skip = entry.skip
        result.notes = list(entry.notes)
    if entry is not None and entry.sources is not None:
        result.origin = "registry (tests/LINKS.txt:%d)" % entry.line
        sources = list(entry.sources)
    else:
        declaration = parse_declaration(read_head(test))
        if declaration is None:
            if test.startswith("tests/rt/"):
                result.origin = "rt default (no declaration)"
                sources = []
            else:
                raise LinkError("%s has no link declaration in its first %d lines and no tests/LINKS.txt entry" % (test, HEADER_LINES))
        elif declaration.inherit:
            other = normalize_test(declaration.inherit)
            inherited = resolve(other, registry, check, stack + (test,))
            sources = [s for s in inherited.sources if not any(s == x or s.endswith("/" + x) for x in declaration.excludes)]
            removed = [x for x in declaration.excludes if not any(s == x or s.endswith("/" + x) for s in inherited.sources)]
            if removed:
                result.warnings.append("exclusions match nothing: %s" % " ".join(removed))
            result.origin = "header line %d: the sources of %s%s" % (
                declaration.line, other, (" but " + " ".join(declaration.excludes)) if declaration.excludes else "")
        else:
            if declaration.incomplete:
                raise LinkError("%s: declaration on line %d is incomplete (%s); add a tests/LINKS.txt entry"
                                % (test, declaration.line, declaration.incomplete))
            result.origin = "header line %d" % declaration.line
            sources = declaration.sources
    seen = set()
    for source in sources:
        if source == test or source in seen or source.startswith("src/rt/"):
            continue
        seen.add(source)
        if source.endswith("_test.cpp"):
            result.warnings.append("lists another test file: %s" % source)
        result.sources.append(source)
    if check:
        missing = [s for s in result.sources if not exists(s)]
        if missing:
            raise LinkError("%s: listed sources do not exist: %s" % (test, " ".join(missing)))
    return result


# ---------------------------------------------------------------- closure helpers (for the registry)

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def includes_of(relpath, cache={}):
    if relpath not in cache:
        try:
            with open(os.path.join(GATEWAY, relpath), encoding="utf-8", errors="replace") as handle:
                text = handle.read()
        except OSError:
            text = ""
        found = []
        for name in INCLUDE_RE.findall(text):
            for base in (os.path.dirname(os.path.join(GATEWAY, relpath)), os.path.join(GATEWAY, "include"), GATEWAY):
                candidate = os.path.join(base, name)
                if os.path.isfile(candidate):
                    found.append(rel(candidate))
                    break
        cache[relpath] = found
    return cache[relpath]


def source_of(header):
    if header.startswith("include/uos/rt/") or not header.endswith(".hpp"):
        return None
    if header.startswith("include/uos/"):
        candidate = "src/" + header[len("include/uos/"):-len(".hpp")] + ".cpp"
    elif header.startswith("tests/helpers/"):
        candidate = header[:-len(".hpp")] + ".cpp"
    else:
        return None
    return candidate if exists(candidate) else None


def input_paths(test, sources, vendors=None):
    """Build inputs under cpp/gateway, including the runtime unit.sh always links."""
    queue = [test] + list(sources)
    queue += sorted(rel(p) for p in glob.glob(os.path.join(GATEWAY, "src", "rt", "*.cpp")))
    import vendor_sources

    if vendors is None:
        vendors = tuple(vendor_sources.selected())
    vendor_inputs = vendor_sources.required(vendors, queue)
    queue.extend(relative for library in vendor_inputs for relative in library.inputs)
    seen = {"tools/vendor_sources.py", vendor_sources.REGISTRY.as_posix()}
    while queue:
        current = queue.pop()
        if current in seen:
            continue
        seen.add(current)
        queue.extend(includes_of(current))
    seen.update("tools/" + name for name in ("unit.sh", "unit.mk", "env.sh", "cxx.sh", "links.py"))
    return sorted(seen)


def input_hashes(test, sources):
    for path in input_paths(test, sources):
        with open(os.path.join(GATEWAY, path), "rb") as handle:
            digest = hashlib.file_digest(handle, "sha256").hexdigest()
        yield "%s  %s" % (digest, path)


def include_closure(test):
    """Every module .cpp whose header the test reaches through #include, following each linked .cpp.
    A wide over-approximation (headers include each other across the import cycles)."""
    queue = [test]
    seen = {test}
    sources = []
    while queue:
        current = queue.pop(0)
        if current.endswith(".inc"):
            continue
        for header in includes_of(current):
            if header in seen or header.startswith("include/uos/rt/"):
                continue
            seen.add(header)
            queue.append(header)
            source = source_of(header)
            if source and source not in seen:
                seen.add(source)
                sources.append(source)
                queue.append(source)
    return sorted(sources)


def build_settings():
    """UOS_OUT and the SDK's nm, as tools/env.sh defines them."""
    import subprocess
    script = '. "%s/tools/env.sh" >/dev/null 2>&1; printf "%%s\\n%%s\\n" "$UOS_OUT" "$UOS_SDK"' % GATEWAY
    out = subprocess.run(["bash", "-c", script], capture_output=True, text=True).stdout.split("\n")
    nm = os.path.join(out[1], "host-tools", "bin", "nm") if len(out) > 1 and out[1] else "nm"
    return out[0], (nm if os.path.isfile(nm) else "nm")


def symbol_closure(test):
    """The smallest set of module objects that defines every symbol the test object needs, read with
    nm from the cached objects (read-only; nothing is compiled). Returns (sources, unresolved uos
    symbols) or None when the test has no cached object (build it once with unit.sh --check)."""
    import subprocess
    out_dir, nm = build_settings()

    def object_of(source):
        return os.path.join(out_dir, "obj", source[:-len(".cpp")] + ".o")

    tables = {}

    def table(source):
        if source not in tables:
            listing = subprocess.run([nm, "-P", object_of(source)], capture_output=True, text=True).stdout
            strong, weak, undefined = set(), set(), set()
            for line in listing.splitlines():
                parts = line.split()
                if len(parts) < 2:
                    continue
                if parts[1] == "U":
                    undefined.add(parts[0])
                elif parts[1] in "TDBRCui":
                    strong.add(parts[0])
                elif parts[1] in "WV":
                    weak.add(parts[0])
            tables[source] = (strong, weak, undefined)
        return tables[source]

    if not os.path.isfile(object_of(test)):
        return None
    candidates = sorted(rel(p) for p in glob.glob(os.path.join(GATEWAY, "src", "**", "*.cpp"), recursive=True))
    candidates += sorted(rel(p) for p in glob.glob(os.path.join(GATEWAY, "tests", "helpers", "*.cpp")))
    provided, strong_by, weak_by = set(), {}, {}
    for source in candidates:
        if not os.path.isfile(object_of(source)):
            continue
        strong, weak, _ = table(source)
        if source.startswith("src/rt/"):
            provided |= strong | weak
            continue
        for name in strong:
            strong_by.setdefault(name, []).append(source)
        for name in weak:
            weak_by.setdefault(name, []).append(source)
    strong, weak, undefined = table(test)
    have = strong | weak
    pending = set(undefined)
    chosen, unresolved = [], set()
    while pending:
        name = pending.pop()
        if name in have or name in provided:
            continue
        definers = strong_by.get(name) or []
        if not definers:
            definers = weak_by.get(name) or []
            if len(definers) != 1:  # inline or template code emitted by many objects: no module needed
                if not definers:
                    unresolved.add(name)
                continue
        pick = definers[0]
        if pick in chosen:
            continue
        chosen.append(pick)
        s, w, u = table(pick)
        have |= s | w
        pending |= u - have
    names = sorted(n for n in unresolved if n not in have and n.startswith("_ZN3uos"))
    try:
        names = subprocess.run(["c++filt"], input="\n".join(names), capture_output=True, text=True).stdout.split("\n")
    except OSError:
        pass
    return chosen, [n for n in names if n]


# ---------------------------------------------------------------- command line

def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("tests", nargs="*")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--explain", action="store_true")
    mode.add_argument("--shell", action="store_true")
    mode.add_argument("--check", action="store_true")
    mode.add_argument("--closure", action="store_true")
    mode.add_argument("--list-tests", action="store_true")
    mode.add_argument("--hashes", action="store_true")
    parser.add_argument("--with-test", action="store_true", help="print the test file first")
    parser.add_argument("--diff", action="store_true", help="with --closure: compare with the resolved list")
    parser.add_argument("--includes", action="store_true", help="with --closure: follow #includes instead of symbols")
    parser.add_argument("--no-exist-check", action="store_true")
    args = parser.parse_args(argv)

    if args.list_tests:
        print("\n".join(all_tests()))
        return 0
    try:
        registry = read_registry()
    except LinkError as error:
        if args.shell:
            print("origin=''\nsources=()\nenv_assign=()\nskip=''\nwarnings=''\nerror=%s" % shlex.quote(str(error)))
            return 0
        print("links.py: %s" % error, file=sys.stderr)
        return 2
    tests = [normalize_test(t) for t in args.tests]
    check = not args.no_exist_check

    if args.check:
        problems = 0
        for test in tests or all_tests():
            try:
                result = resolve(test, registry, check)
                flag = "SKIP " if result.skip else "ok   "
                print("%s %s  [%s] %d sources%s" % (flag, test, result.origin, len(result.sources),
                                                     ("; " + "; ".join(result.warnings)) if result.warnings else ""))
            except LinkError as error:
                problems += 1
                print("ERROR %s  %s" % (test, error))
        stale = [t for t in registry if not exists(t)]
        for test in stale:
            print("STALE tests/LINKS.txt entry for a missing file: %s" % test)
        return 1 if problems else 0

    if not tests:
        parser.error("name at least one test file")

    if args.closure:
        for test in tests:
            found = None if args.includes else symbol_closure(test)
            if found is None:
                if not args.includes:
                    print("%s: no cached object; using the include closure (an over-approximation)" % test, file=sys.stderr)
                computed, unresolved = include_closure(test), []
            else:
                computed, unresolved = found
            for name in unresolved:
                print("%s: unresolved (declared, not defined in any module object): %s" % (test, name), file=sys.stderr)
            if not args.diff:
                print(" ".join(computed))
                continue
            try:
                listed = resolve(test, registry, check=False).sources
            except LinkError as error:
                print("%s: %s" % (test, error))
                continue
            print("%s\n  missing (in closure, not listed): %s\n  extra (listed, not in closure): %s" % (
                test, " ".join(s for s in computed if s not in listed) or "-",
                " ".join(s for s in listed if s not in computed) or "-"))
        return 0

    status = 0
    for test in tests:
        try:
            result = resolve(test, registry, check)
        except LinkError as error:
            if args.shell:
                print("origin=''\nsources=()\nenv_assign=()\nskip=''\nwarnings=''\nerror=%s" % shlex.quote(str(error)))
                return 0
            print("links.py: %s" % error, file=sys.stderr)
            status = 1
            continue
        if args.hashes:
            print("\n".join(input_hashes(test, result.sources)))
        elif args.shell:
            print("origin=%s" % shlex.quote(result.origin))
            print("sources=(%s)" % " ".join(shlex.quote(s) for s in result.sources))
            print("env_assign=(%s)" % " ".join(shlex.quote(a) for a in result.env))
            print("skip=%s" % shlex.quote(result.skip or ""))
            print("warnings=%s" % shlex.quote("; ".join(result.warnings)))
            print("error=''")
        elif args.explain:
            print("%s\n  origin:  %s\n  sources: %s" % (test, result.origin, " ".join(result.sources) or "(none)"))
            if result.env:
                print("  env:     %s" % " ".join(result.env))
            if result.skip:
                print("  skip:    %s" % result.skip)
            for note in result.notes:
                print("  note:    %s" % note)
            for warning in result.warnings:
                print("  warning: %s" % warning)
        else:
            print(" ".join(([test] if args.with_test else []) + result.sources))
    return status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
