"""Linux gateway process samples. All sizes from procfs are KiB."""

from pathlib import Path
import re
import socket
import subprocess
from contextlib import contextmanager

from driver import require

MAPPING = re.compile(r"^[0-9a-f]+-[0-9a-f]+ [^\n]*", re.M)
FIELDS = ("trim_return", "arenas", "allocated_before", "free_before", "mmap_before",
          "allocated_after", "free_after", "mmap_after")


def kib(text, key):
    return int(re.search(rf"^{key}:\s*(\d+) kB$", text, re.M)[1])


def sample(pid):
    path = Path(f"/proc/{pid}")
    status = (path / "status").read_text()
    maps = (path / "smaps").read_text()
    headers = list(MAPPING.finditer(maps))
    labels = [match[0].split(maxsplit=5) for match in headers]
    result = {field: kib(status, field) for field in ("VmRSS", "RssAnon", "VmHWM")}
    result.update(heap_rss=0, heap_size=0, anonymous_mappings=[], anonymous_kib=0)
    for index, label in enumerate(labels):
        end = headers[index + 1].start() if index + 1 < len(headers) else len(maps)
        body = maps[headers[index].end():end]
        name = label[5] if len(label) > 5 else ""
        anon = kib(body, "Anonymous")
        result["anonymous_kib"] += anon
        if name == "[heap]":
            result.update(heap_rss=kib(body, "Rss"), heap_size=kib(body, "Size"))
        elif not name or name.startswith(("[anon:", "[stack")):
            result["anonymous_mappings"].append({"name": name or "anonymous", "permissions": label[1],
                "size": kib(body, "Size"), "rss": kib(body, "Rss"), "anonymous": anon})
    return result


def gateway_pid(child, package, profiled):
    if not profiled:
        return child.pid
    children = Path(f"/proc/{child.pid}/task/{child.pid}/children").read_text().split()
    return next(int(pid) for pid in children if (Path("/proc") / pid / "exe").resolve() == package / "bin/uos-gateway")


@contextmanager
def trim_probe(out, env):
    lib = out / "trim.so"
    source = Path(__file__).resolve().parents[1] / "codex_memory/trim.cpp"
    subprocess.run(["c++", "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-fPIC",
                    "-shared", "-pthread", source, "-o", lib], env=env, check=True, timeout=30)
    reader, child = socket.socketpair(type=socket.SOCK_SEQPACKET)
    with reader, child:
        reader.settimeout(5)
        yield reader, child, {**env, "LD_PRELOAD": str(lib), "UOS_MEMORY_TRIM_FD": str(child.fileno())}


def trim(reader):
    reader.sendall(b"t")
    values = dict(zip(FIELDS, map(int, reader.recv(256).split()), strict=True))
    require(values["trim_return"] in (0, 1) and values["arenas"] > 0, "Invalid glibc trim receipt")
    return values
