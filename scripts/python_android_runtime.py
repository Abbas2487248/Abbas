#!/usr/bin/env python3
"""Prepare a checksum-pinned CPython Android runtime for the APK packager."""

import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import urllib.request
import zipfile

VERSION = "3.14.10"
ARCHIVES = {
    "arm64-v8a": (
        "aarch64-linux-android",
        "423c80b957330cf0ebb92b9b20f2294c292144a441bdb0ceb8c69d14928e569b",
        183,
    ),
    "x86_64": (
        "x86_64-linux-android",
        "6ebda1fc3f28efc5861cad397254e8ce5d81ba4f83a3cb6a0c124c3826a0e14c",
        62,
    ),
}
SYSTEM_LIBRARIES = {
    "libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so",
    "libandroid.so", "libjnigraphics.so", "libEGL.so", "libGLESv2.so",
    "libOpenSLES.so", "libaaudio.so", "libmediandk.so", "libnativewindow.so",
    "libvulkan.so", "libsync.so", "libunwind.so",
}


def _run(*args):
    return subprocess.run([str(arg) for arg in args], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout


def _download_archive(abi, cache):
    triple, expected, _ = ARCHIVES[abi]
    filename = f"python-{VERSION}-{triple}.tar.gz"
    archive = cache / filename
    cache.mkdir(parents=True, exist_ok=True)
    if not archive.is_file():
        url = f"https://www.python.org/ftp/python/{VERSION}/{filename}"
        temporary = archive.with_suffix(archive.suffix + ".part")
        try:
            with urllib.request.urlopen(url, timeout=90) as response, temporary.open("wb") as output:
                shutil.copyfileobj(response, output)
            temporary.replace(archive)
        finally:
            temporary.unlink(missing_ok=True)
    digest = hashlib.sha256(archive.read_bytes()).hexdigest()
    if digest != expected:
        archive.unlink(missing_ok=True)
        raise ValueError(f"CPython {VERSION} {abi} archive checksum mismatch")
    return archive


def _safe_extract(archive, destination):
    destination.mkdir(parents=True, exist_ok=True)
    root = destination.resolve()
    with tarfile.open(archive, "r:gz") as source:
        members = source.getmembers()
        for member in members:
            relative = Path(member.name)
            if relative.is_absolute() or ".." in relative.parts:
                raise ValueError("Unsafe path in official CPython archive")
            target = (root / relative).resolve()
            if target != root and root not in target.parents:
                raise ValueError("CPython archive entry escapes extraction root")
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                stream = source.extractfile(member)
                if stream is None:
                    raise ValueError("Unreadable CPython archive member")
                with stream, target.open("wb") as output:
                    shutil.copyfileobj(stream, output)
            elif member.issym() or member.islnk():
                # Symlinks are intentionally omitted. Required runtime libraries
                # are packaged by their real filenames, not compatibility aliases.
                continue
            else:
                raise ValueError("Unsupported entry in official CPython archive")


def _make_stdlib_zip(stdlib, destination):
    skipped = {"__pycache__", "test", "tests", "tkinter", "idlelib"}
    count = 0
    with zipfile.ZipFile(destination, "w", compression=zipfile.ZIP_DEFLATED,
                         compresslevel=9) as output:
        for path in sorted(stdlib.rglob("*")):
            if not path.is_file() or path.suffix in {".pyc", ".pyo"}:
                continue
            relative = path.relative_to(stdlib)
            if any(part in skipped for part in relative.parts):
                continue
            if "lib-dynload" in relative.parts:
                continue
            output.write(path, relative.as_posix())
            count += 1
    if count < 500:
        raise ValueError(f"CPython standard library archive looks incomplete ({count} files)")
    return count


def prepare_python_runtime(abi, ndk_bin, linker, work):
    if abi not in ARCHIVES:
        raise ValueError(f"Unsupported CPython Android ABI: {abi}")
    work = Path(work)
    cache = Path(__file__).resolve().parents[1] / "target" / "python-android-cache"
    archive = _download_archive(abi, cache)
    extracted = work / "source"
    _safe_extract(archive, extracted)
    prefix = extracted / "prefix"
    include = prefix / "include" / "python3.14"
    library = prefix / "lib" / "libpython3.14.so"
    dynload = prefix / "lib" / "python3.14" / "lib-dynload"
    if not (include / "Python.h").is_file() or not library.is_file() or not dynload.is_dir():
        raise ValueError("Official CPython archive is missing required runtime files")

    native = work / "native"
    native.mkdir(parents=True, exist_ok=True)
    for path in sorted((prefix / "lib").glob("*.so")):
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, native / path.name)
    extension_count = 0
    for path in sorted(dynload.glob("*.so")):
        if path.is_file() and not path.is_symlink():
            shutil.copy2(path, native / path.name)
            extension_count += 1
    if extension_count < 50:
        raise ValueError(f"CPython native extension set is incomplete ({extension_count})")

    launcher_source = work / "python_launcher.c"
    launcher_source.write_text(
        "#include <Python.h>\n"
        "int main(int argc, char **argv) { return Py_BytesMain(argc, argv); }\n",
        encoding="utf-8",
    )
    launcher = native / "libfesterm_python_exec.so"
    _run(linker, "-Wall", "-Wextra", "-Werror", "-O2", "-fPIE", "-pie",
         f"-I{include}", launcher_source, f"-L{prefix / 'lib'}",
         "-Wl,-rpath,$ORIGIN", f"-Wl,-rpath-link,{prefix / 'lib'}",
         "-lpython3.14", "-o", launcher)
    launcher.chmod(0o755)

    readelf = Path(ndk_bin) / "llvm-readelf"
    header = _run(readelf, "-h", launcher)
    _, _, expected_machine = ARCHIVES[abi]
    machine = "AArch64" if expected_machine == 183 else "Advanced Micro Devices X86-64"
    if "DYN" not in header or machine not in header:
        raise ValueError(f"CPython launcher is not a {abi} Android PIE")
    dynamic = _run(readelf, "-d", launcher)
    if "libpython3.14.so" not in dynamic or "$ORIGIN" not in dynamic:
        raise ValueError("CPython launcher is missing libpython or its relative library path")

    bundled = {path.name for path in native.glob("*.so")}
    for path in sorted(native.glob("*.so")):
        details = _run(readelf, "-d", path)
        for name in __import__("re").findall(r"Shared library: \[([^]]+)\]", details):
            if name not in bundled and name not in SYSTEM_LIBRARIES:
                raise ValueError(f"Unresolved Android CPython dependency {name} in {path.name}")

    ensurepip = prefix / "lib" / "python3.14" / "ensurepip"
    bundled_pip = list((ensurepip / "_bundled").glob("pip-*.whl"))
    if not (ensurepip / "__init__.py").is_file() or not bundled_pip:
        raise ValueError("Official CPython archive is missing ensurepip or its bundled pip wheel")

    assets = work / "assets"
    assets.mkdir(parents=True, exist_ok=True)
    stdlib_zip = assets / "python314.zip"
    file_count = _make_stdlib_zip(prefix / "lib" / "python3.14", stdlib_zip)
    return {
        "native_dir": native,
        "stdlib_zip": stdlib_zip,
        "extension_count": extension_count,
        "stdlib_file_count": file_count,
        "archive_sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
        "launcher": launcher,
    }
