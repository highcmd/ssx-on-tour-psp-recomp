#!/usr/bin/env python3
"""Download the toolchain and libraries for the Windows x64 build.

Everything goes to third_party/win64/ (gitignored). Works on Linux/WSL (cross
build) and on Windows (native build): only the compiler download differs.

  llvm-mingw  clang/lld for x86_64-w64-mingw32, UCRT     (github.com/mstorsjo/llvm-mingw)
  SDL2        mingw development package                   (github.com/libsdl-org/SDL)
  FFmpeg      win64 shared build, GPL                     (github.com/BtbN/FFmpeg-Builds)

On a Windows host without CMake/Ninja on PATH (the packaged launcher), those
are fetched too. All but FFmpeg are pinned releases checked by SHA-256. FFmpeg comes from the
"latest" build of the 8.1 release branch, which is rebuilt over time, so it is
checked by layout only.
"""
import hashlib, pathlib, platform, shutil, sys, tarfile, urllib.request, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
DEST = ROOT / "third_party" / "win64"

LLVM_MINGW_VER = "20260922"
GH = "https://github.com"
PKGS = {
    "llvm-mingw-linux": (
        f"{GH}/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VER}/llvm-mingw-{LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64.tar.xz",
        "bb7bb7654b33d5aa8712acb837c963b2e0c56352560c76105270a3268c665c21"),
    "llvm-mingw-windows": (
        f"{GH}/mstorsjo/llvm-mingw/releases/download/{LLVM_MINGW_VER}/llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64.zip",
        "e3ad77d117a4bea19a7a3b333341824d79a5a371004a10e25b8504e7b3047666"),
    "sdl2": (
        f"{GH}/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-devel-2.32.10-mingw.zip",
        "f15cff5fca62ec9381a016ef1d42a95c638cd72d2f226ba5781c76fe43dbd1ac"),
    "cmake-windows": (
        f"{GH}/Kitware/CMake/releases/download/v4.3.5/cmake-4.3.5-windows-x86_64.zip",
        "c05a60d22d5df22d75a72ae9b6628ef5f20daee4f4ba380920e1fbbeaf244fc1"),
    "ninja-windows": (
        f"{GH}/ninja-build/ninja/releases/download/v1.13.2/ninja-win.zip",
        "07fc8261b42b20e71d1720b39068c2e14ffcee6396b76fb7a795fb460b78dc65"),
    "ffmpeg": (
        f"{GH}/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip",
        None),
}


def log(msg):
    print(msg, flush=True)


class DepsError(Exception):
    pass


def die(msg):
    raise DepsError(msg)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def fetch(name):
    url, sha = PKGS[name]
    DEST.mkdir(parents=True, exist_ok=True)
    out = DEST / url.rsplit("/", 1)[1]
    if out.exists() and (sha is None or sha256(out) == sha):
        return out
    log(f"==> downloading {url}")
    tmp = out.with_suffix(out.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = step = 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            done += len(chunk)
            if total and done * 10 // total > step:
                step = done * 10 // total
                log(f"    {done >> 20}/{total >> 20} MiB")
    if sha:
        got = sha256(tmp)
        if got != sha:
            tmp.unlink()
            die(f"{out.name}: SHA-256 {got}, expected {sha}")
    tmp.rename(out)
    return out


def unpack(archive, marker):
    """Extract archive into DEST unless DEST/marker exists; return DEST/marker."""
    target = DEST / marker
    if target.exists():
        return target
    log(f"==> unpacking {archive.name}")
    if archive.name.endswith(".zip"):
        with zipfile.ZipFile(archive) as z:
            z.extractall(DEST)
            if platform.system() != "Windows":   # keep the executable bits of the toolchain
                for info in z.infolist():
                    mode = info.external_attr >> 16
                    if mode:
                        (DEST / info.filename).chmod(mode & 0o777)
    else:
        with tarfile.open(archive) as t:
            t.extractall(DEST, filter="tar") if hasattr(tarfile, "data_filter") else t.extractall(DEST)
    if not target.exists():
        die(f"{archive.name} did not contain {marker}")
    return target


def ensure():
    """Return dict with the toolchain root, SDL2 prefix, FFmpeg root and tool paths."""
    host = "windows" if platform.system() == "Windows" else "linux"
    if host == "linux" and platform.machine() not in ("x86_64", "AMD64"):
        die("the Windows cross build needs an x86-64 Linux host")
    llvm_dir = (f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64" if host == "linux"
                else f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64")
    llvm = unpack(fetch(f"llvm-mingw-{host}"), llvm_dir)
    sdl = unpack(fetch("sdl2"), "SDL2-2.32.10") / "x86_64-w64-mingw32"
    ffm = unpack(fetch("ffmpeg"), "ffmpeg-n8.1-latest-win64-gpl-shared-8.1")
    for need in (ffm / "include/libavcodec/avcodec.h", ffm / "bin", sdl / "bin/SDL2.dll"):
        if not need.exists():
            die(f"unexpected package layout, {need} is missing")
    tools = {}
    if host == "windows":
        if not shutil.which("cmake"):
            tools["cmake"] = str(unpack(fetch("cmake-windows"), "cmake-4.3.5-windows-x86_64") / "bin/cmake.exe")
        if not shutil.which("ninja"):
            nz = fetch("ninja-windows")
            ndir = DEST / "ninja-1.13.2"
            if not (ndir / "ninja.exe").exists():
                with zipfile.ZipFile(nz) as z:
                    z.extractall(ndir)
            tools["ninja"] = str(ndir / "ninja.exe")
    return {"llvm": llvm, "sdl2": sdl, "ffmpeg": ffm, "tools": tools}


if __name__ == "__main__":
    try:
        for k, v in ensure().items():
            print(f"{k}: {v}")
    except DepsError as e:
        sys.exit(f"error: {e}")
