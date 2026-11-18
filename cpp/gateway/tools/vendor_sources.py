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
    "ada": ("ada.cpp",),
    "llhttp": ("llhttp.c", "api.c", "http.c"),
    "qcbor": ("src/qcbor_decode.c", "src/qcbor_encode.c", "src/qcbor_err_to_str.c", "src/UsefulBuf.c", "src/ieee754.c"),
    "quickjs-libregexp": ("libregexp.c", "libunicode.c"),
    "simdjson": ("simdjson.cpp",),
    "simdutf": ("simdutf.cpp",),
}
VERSIONS = {
    "ada": "4.0.0",
    "llhttp": "9.4.4",
    "qcbor": "1.6.1",
    "quickjs-libregexp": "0.18.0",
    "simdjson": "5.2.0",
    "simdutf": "9.4.0",
}
VENDOR_INCLUDES = {
    "qcbor": "third_party/qcbor/inc",
    "quickjs-libregexp": "third_party/quickjs-libregexp",
}
VENDOR_HEADERS = {
    "qcbor": {
        "inc/UsefulBuf.h", "inc/qcbor.h", "inc/qcbor/UsefulBuf.h", "inc/qcbor/qcbor.h", "inc/qcbor/qcbor_common.h",
        "inc/qcbor/qcbor_decode.h", "inc/qcbor/qcbor_encode.h", "inc/qcbor/qcbor_private.h", "inc/qcbor/qcbor_spiffy_decode.h",
        "src/ieee754.h",
    },
    "quickjs-libregexp": {"cutils.h", "libregexp-opcode.h", "libregexp.h", "libunicode-table.h", "libunicode.h"},
}
SIMDJSON_HEADER = "third_party/simdjson/simdjson.h"
VENDOR_MANIFESTS = {
    "ada": "a6085f89c35fc39b06802935130c4aeffb449b695e568f71b14db0bb7a6e403f",
    "llhttp": "b9b8ae35e0b43ea291899360441bf4b6e4acf5942253118158c0d95fe1c9b8eb",
    "qcbor": "c149f42818ce685d5b2fc99f67ae7dd617274b111b28f2ceee185f9fc9aa4fbe",
    "quickjs-libregexp": "698d914c326a35c7f009a663f079e56c61d51c7c03fc3e877f11c85f82b8bd0b",
    "simdjson": "e958e232e00f9de08ca1118888fbe15ddf75e5b9ffb0c4d5b606eb645909d834",
    "simdutf": "f7b0cb9c96b51c837550c4a68f0f5305833cc8649aa7f25897c5dab487c60481",
}
STRICT_WARNINGS = {"-Wall", "-Wextra", "-Wshadow", "-Wconversion", "-Werror"}


class Library(NamedTuple):
    name: str
    version: str
    sources: tuple[str, ...]
    consumers: tuple[str, ...]
    inputs: tuple[str, ...]

    @property
    def target(self):
        return "uos_vendor_" + self.name

    @property
    def archive(self):
        return "lib" + self.target + ".a"

    @property
    def headers(self):
        return tuple(path for path in self.inputs if path.endswith(".h"))


def selected(root=GATEWAY):
    active = json.loads((root / REGISTRY).read_text())
    if not isinstance(active, dict) or active.keys() - SOURCES.keys():
        raise ValueError("Unknown vendor static library declaration")
    for name, consumers in sorted(active.items()):
        if not isinstance(consumers, list) or not consumers or any(not isinstance(src, str) for src in consumers) or len(set(consumers)) != len(consumers):
            raise ValueError("Declare nonempty unique vendor consumers for " + name)
        if any(not re.fullmatch(r"(?:src|tests|bench|fuzz)/[a-zA-Z0-9_/.-]+\.cpp", source)
               or ".." in Path(source).parts or not (root / source).is_file() for source in consumers):
            raise ValueError("Invalid vendor consumer for " + name)
        directory = Path("third_party") / name
        provenance = directory / "PROVENANCE.md"
        manifest = directory / "SHA256SUMS"
        sources = SOURCES[name]
        headers = VENDOR_HEADERS.get(name, {name + ".h"})
        filenames = {*sources, *headers, "LICENSE"}
        entries = {}
        manifest_bytes = (root / manifest).read_bytes()
        if hashlib.sha256(manifest_bytes).hexdigest() != VENDOR_MANIFESTS[name]:
            raise ValueError("Selected vendor manifest differs from the qualified upstream release: " + name)
        for line in manifest_bytes.decode().splitlines():
            match = re.fullmatch(r"([0-9a-f]{64})  ([a-zA-Z0-9_/.-]+)", line)
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
        yield Library(name, VERSIONS[name], tuple((directory / source).as_posix() for source in sources), tuple(consumers), tuple(inputs))


def required(libraries, sources, targets=()):
    sources = set(map(str, sources))
    return tuple(lib for lib in libraries if lib.target in targets or sources.intersection(lib.consumers))


def vendor_flags(flags):
    return [arg for arg in flags if arg not in STRICT_WARNINGS and not arg.startswith("-Werror=")]


def c_dialect_flags(source):
    # QuickJS's cutils.h uses platform declarations exposed by the GNU C dialect.
    return ["-std=gnu11"] if source.startswith("third_party/quickjs-libregexp/") and source.endswith(".c") else []


def include_flags(library, root=GATEWAY):
    directory = VENDOR_INCLUDES.get(library.name)
    return ["-isystem", str(root / directory)] if directory else []


def simdjson_consumers(libraries):
    return {source for lib in libraries if lib.name == "simdjson" for source in lib.consumers}


def simdjson_flags(root=GATEWAY):
    return ["-DUOS_GATEWAY_SIMDJSON", "-isystem", str(root / Path(SIMDJSON_HEADER).parent)]


def main(args):
    libraries = tuple(selected())
    if args[0] == "cmake":
        for library in libraries:
            sources = " ".join(f'"{source}"' for source in library.sources)
            consumers = " ".join(f'"{source}"' for source in library.consumers)
            inputs = " ".join(f'"{path}"' for path in library.inputs)
            print(f'uos_register_vendor("{library.name}" SOURCES {sources} CONSUMERS {consumers} INPUTS {inputs})')
            if c_dialect_flags(library.sources[0]):
                print(f"set_property(TARGET {library.target} PROPERTY C_EXTENSIONS ON)")
            directory = VENDOR_INCLUDES.get(library.name)
            if directory:
                print(f'target_include_directories({library.target} SYSTEM PUBLIC "${{CMAKE_CURRENT_SOURCE_DIR}}/{directory}")')
    elif args[0] == "unit":
        print(" ".join(sorted(simdjson_consumers(libraries))))
        print(" ".join(source for lib in required(libraries, args[1:]) for source in lib.sources))
    else:
        raise ValueError("Expected cmake or unit")


if __name__ == "__main__":
    main(sys.argv[1:])
