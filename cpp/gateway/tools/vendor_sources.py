#!/usr/bin/python3
"""Declare vendored static dependencies of exact first-party source closures."""
import hashlib
import json
from pathlib import Path
import re
import sys
from typing import NamedTuple


GATEWAY = Path(__file__).resolve().parents[1]
REGISTRY = Path("cmake/vendors.json")
NAMES = {"simdutf", "simdjson", "quickjs-libregexp"}
SIMDJSON_HEADER = "third_party/simdjson/simdjson.h"
VENDOR_MANIFESTS = {
    "quickjs-libregexp": "698d914c326a35c7f009a663f079e56c61d51c7c03fc3e877f11c85f82b8bd0b",
    "simdjson": "e958e232e00f9de08ca1118888fbe15ddf75e5b9ffb0c4d5b606eb645909d834",
    "simdutf": "f7b0cb9c96b51c837550c4a68f0f5305833cc8649aa7f25897c5dab487c60481",
}
QUICKJS_SOURCES = ("libregexp.c", "libunicode.c")
QUICKJS_FILES = {*QUICKJS_SOURCES, "LICENSE", "cutils.h", "libregexp.h", "libregexp-opcode.h",
                 "libunicode.h", "libunicode-table.h"}
STRICT_WARNINGS = {"-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"}


class Library(NamedTuple):
    name: str
    source: str
    consumers: tuple[str, ...]
    inputs: tuple[str, ...]
    additional_sources: tuple[str, ...] = ()

    @property
    def sources(self):
        return (self.source, *self.additional_sources)

    @property
    def target(self):
        return "uos_vendor_" + self.name

    @property
    def archive(self):
        return "lib" + self.target + ".a"


def selected(root=GATEWAY):
    active = json.loads((root / REGISTRY).read_text())
    if not isinstance(active, dict) or active.keys() - NAMES:
        raise ValueError("Unknown vendor static library declaration")
    for name, consumers in sorted(active.items()):
        if not isinstance(consumers, list) or not consumers or any(not isinstance(src, str) for src in consumers) or len(set(consumers)) != len(consumers):
            raise ValueError("Declare nonempty unique vendor consumers for " + name)
        if any(not re.fullmatch(r"(?:src|tests|bench)/[a-zA-Z0-9_/.-]+\.cpp", source)
               or ".." in Path(source).parts or not (root / source).is_file() for source in consumers):
            raise ValueError("Invalid vendor consumer for " + name)
        directory = Path("third_party") / name
        provenance = directory / "PROVENANCE.md"
        manifest = directory / "SHA256SUMS"
        source_filenames = QUICKJS_SOURCES if name == "quickjs-libregexp" else (name + ".cpp",)
        filenames = QUICKJS_FILES if name == "quickjs-libregexp" else {source_filenames[0], name + ".h", "LICENSE"}
        entries = {}
        manifest_bytes = (root / manifest).read_bytes()
        if hashlib.sha256(manifest_bytes).hexdigest() != VENDOR_MANIFESTS[name]:
            raise ValueError("Selected vendor manifest differs from the qualified upstream release: " + name)
        for line in manifest_bytes.decode().splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  ([a-zA-Z0-9_.-]+)", line)
            if not match or match[2] not in filenames or match[2] in entries:
                raise ValueError("Invalid upstream SHA256SUMS for " + name)
            entries[match[2]] = match[1]
        if entries.keys() != filenames:
            raise ValueError("Incomplete upstream SHA256SUMS for " + name)
        inputs = [provenance.as_posix(), manifest.as_posix()]
        if not (root / provenance).is_file():
            raise ValueError("Missing upstream provenance for " + name)
        for filename, expected in sorted(entries.items()):
            relative = directory / filename
            path = root / relative
            if path.resolve() != path or not path.is_file():
                raise ValueError("Noncanonical vendor input: " + str(relative))
            with path.open("rb") as handle:
                if hashlib.file_digest(handle, "sha256").hexdigest() != expected:
                    raise ValueError("Supplied vendor input bytes differ: " + str(relative))
            inputs.append(relative.as_posix())
        source, *additional = ((directory / filename).as_posix() for filename in source_filenames)
        yield Library(name, source, tuple(consumers), tuple(inputs), tuple(additional))


def required(libraries, sources, targets=()):
    sources = set(map(str, sources))
    return tuple(lib for lib in libraries if lib.target in targets or sources.intersection(lib.consumers))


def vendor_flags(flags):
    return [arg for arg in flags if arg not in STRICT_WARNINGS and not arg.startswith("-Werror=")]


def simdjson_consumers(libraries):
    return {source for lib in libraries if lib.name == "simdjson" for source in lib.consumers}


def simdjson_flags(root=GATEWAY):
    return ["-DUOS_GATEWAY_SIMDJSON", "-isystem", str(root / Path(SIMDJSON_HEADER).parent)]


def main(args):
    libraries = tuple(selected())
    if args[0] == "cmake":
        for library in libraries:
            args = [library.name, library.source, *library.consumers]
            print("uos_register_vendor(" + " ".join('"' + value + '"' for value in args) + ")")
            if library.additional_sources:
                print(f'target_sources("{library.target}" PRIVATE ' + " ".join(f'"{source}"' for source in library.additional_sources) + ")")
                print('set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS '
                      + " ".join(f'"{path}"' for path in library.inputs) + ")")
    elif args[0] == "unit":
        print(" ".join(sorted(simdjson_consumers(libraries))))
        print(" ".join(source for lib in required(libraries, args[1:]) for source in lib.sources))
    else:
        raise ValueError("Expected cmake or unit")


if __name__ == "__main__":
    main(sys.argv[1:])
