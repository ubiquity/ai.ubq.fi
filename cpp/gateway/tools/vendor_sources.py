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
SOURCES = {
    "llhttp": ("llhttp.c", "api.c", "http.c"),
    "simdjson": ("simdjson.cpp",),
    "simdutf": ("simdutf.cpp",),
}
SIMDJSON_HEADER = "third_party/simdjson/simdjson.h"
VENDOR_MANIFESTS = {
    "llhttp": "b9b8ae35e0b43ea291899360441bf4b6e4acf5942253118158c0d95fe1c9b8eb",
    "simdjson": "e958e232e00f9de08ca1118888fbe15ddf75e5b9ffb0c4d5b606eb645909d834",
    "simdutf": "f7b0cb9c96b51c837550c4a68f0f5305833cc8649aa7f25897c5dab487c60481",
}
STRICT_WARNINGS = {"-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"}


class Library(NamedTuple):
    name: str
    sources: tuple[str, ...]
    consumers: tuple[str, ...]
    inputs: tuple[str, ...]

    @property
    def source(self):
        return self.sources[0]

    @property
    def target(self):
        return "uos_vendor_" + self.name

    @property
    def archive(self):
        return "lib" + self.target + ".a"


def selected(root=GATEWAY):
    active = json.loads((root / REGISTRY).read_text())
    if not isinstance(active, dict) or active.keys() - SOURCES.keys():
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
        sources = SOURCES[name]
        filenames = {*sources, name + ".h", "LICENSE"}
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
        yield Library(name, tuple((directory / source).as_posix() for source in sources), tuple(consumers), tuple(inputs))


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
            if len(library.sources) > 1:
                extra = " ".join('"' + source + '"' for source in library.sources[1:])
                print(f'target_sources("{library.target}" PRIVATE {extra})')
                print(f'set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS {extra})')
    elif args[0] == "unit":
        print(" ".join(sorted(simdjson_consumers(libraries))))
        print(" ".join(source for lib in required(libraries, args[1:]) for source in lib.sources))
    else:
        raise ValueError("Expected cmake or unit")


if __name__ == "__main__":
    main(sys.argv[1:])
