#!/usr/bin/env python3
"""Download the toolchain and libraries for the Windows x64 build.

Everything goes to third_party/win64/ (gitignored). Works on Linux/WSL (cross
build) and on Windows (native build): only the compiler download differs.

  llvm-mingw  clang/lld for x86_64-w64-mingw32, UCRT     (github.com/mstorsjo/llvm-mingw)
  SDL2        mingw development package                   (github.com/libsdl-org/SDL)
  FFmpeg      win64 shared build, GPL                     (github.com/BtbN/FFmpeg-Builds)

The first two are pinned releases checked by SHA-256. FFmpeg comes from the
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
    "ffmpeg": (
        f"{GH}/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip",
        None),
}


def fetch(name):
    url, sha = PKGS[name]
    DEST.mkdir(parents=True, exist_ok=True)
    out = DEST / url.rsplit("/", 1)[1]
    if out.exists() and (sha is None or hashlib.sha256(out.read_bytes()).hexdigest() == sha):
        return out
    print(f"==> downloading {url}", flush=True)
    tmp = out.with_suffix(out.suffix + ".part")
    with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
        total = int(r.headers.get("Content-Length") or 0)
        done = 0
        while chunk := r.read(1 << 20):
            f.write(chunk)
            done += len(chunk)
            if total and sys.stdout.isatty():
                print(f"\r    {done >> 20}/{total >> 20} MiB", end="", flush=True)
        if total and sys.stdout.isatty():
            print()
    if sha:
        got = hashlib.sha256(tmp.read_bytes()).hexdigest()
        if got != sha:
            tmp.unlink()
            sys.exit(f"error: {out.name}: SHA-256 {got}, expected {sha}")
    tmp.rename(out)
    return out


def unpack(archive, marker):
    """Extract archive into DEST unless DEST/marker exists; return DEST/marker."""
    target = DEST / marker
    if target.exists():
        return target
    print(f"==> unpacking {archive.name}", flush=True)
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
        sys.exit(f"error: {archive.name} did not contain {marker}")
    return target


def ensure():
    """Return dict with the toolchain root, SDL2 prefix and FFmpeg root."""
    host = "windows" if platform.system() == "Windows" else "linux"
    if host == "linux" and platform.machine() not in ("x86_64", "AMD64"):
        sys.exit("error: the Windows cross build needs an x86-64 Linux host")
    llvm_dir = (f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-ubuntu-22.04-x86_64" if host == "linux"
                else f"llvm-mingw-{LLVM_MINGW_VER}-ucrt-x86_64")
    llvm = unpack(fetch(f"llvm-mingw-{host}"), llvm_dir)
    sdl = unpack(fetch("sdl2"), "SDL2-2.32.10") / "x86_64-w64-mingw32"
    ffm = unpack(fetch("ffmpeg"), "ffmpeg-n8.1-latest-win64-gpl-shared-8.1")
    for need in (ffm / "include/libavcodec/avcodec.h", ffm / "bin", sdl / "bin/SDL2.dll"):
        if not need.exists():
            sys.exit(f"error: unexpected package layout, {need} is missing")
    return {"llvm": llvm, "sdl2": sdl, "ffmpeg": ffm}


if __name__ == "__main__":
    for k, v in ensure().items():
        print(f"{k}: {v}")
