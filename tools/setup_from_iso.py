#!/usr/bin/env python3
"""Build the PC port from your own copy of SSX On Tour (USA, ULUS-10042).

The repository holds no game data and no code derived from it. This script takes
the UMD image you dumped yourself (.iso, or a .zip holding one) and produces
everything locally:

  iso/<name>.iso         the image (unpacked from the zip, or a symlink to it)
  extracted/             the UMD file system (read by the runtime as disc0:)
  data/functions.json    function analysis of BOOT.BIN
  build/gen/             recompiled C++ and the memory image
  build/cmake/ssx        the executable

All of these are in .gitignore. Usage:

  tools/setup_from_iso.py <image.iso|image.zip> [--no-build] [--force] [-j N]
"""
import argparse, hashlib, os, pathlib, shutil, struct, subprocess, sys, zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SECTOR = 2048

# Hashes identify the supported release; they are not game content.
GAME_ID = b"ULUS-10042"
BOOT_SHA1 = "74d1360e3111283fe937185ef23fa8d901bb0085"   # PSP_GAME/SYSDIR/BOOT.BIN


def die(msg):
    sys.exit(f"error: {msg}")


def step(msg):
    print(f"==> {msg}", flush=True)


# --- ISO 9660 ------------------------------------------------------------

class Iso:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        pvd = self.read(16 * SECTOR, SECTOR)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            die(f"{path} is not an ISO 9660 image")
        self.root = self._record(pvd, 156)

    def read(self, off, size):
        self.f.seek(off)
        data = self.f.read(size)
        if len(data) != size:
            die(f"{self.path} is truncated (read past the end at {off:#x})")
        return data

    @staticmethod
    def _record(buf, off):
        n = buf[off + 32]
        name = buf[off + 33:off + 33 + n]
        return {
            "lba": struct.unpack_from("<I", buf, off + 2)[0],
            "size": struct.unpack_from("<I", buf, off + 10)[0],
            "dir": bool(buf[off + 25] & 2),
            "name": name.decode("ascii", "replace").split(";")[0],
            "special": name in (b"\0", b"\1"),
        }

    def listdir(self, rec):
        data = self.read(rec["lba"] * SECTOR, rec["size"])
        off = 0
        while off < len(data):
            n = data[off]
            if n == 0:                        # records never cross a sector
                off = (off // SECTOR + 1) * SECTOR
                continue
            r = self._record(data, off)
            if not r["special"]:
                yield r
            off += n

    def walk(self, rec=None, prefix=""):
        for r in self.listdir(rec or self.root):
            path = prefix + r["name"]
            if r["dir"]:
                yield path, r
                yield from self.walk(r, path + "/")
            else:
                yield path, r

    def find(self, path):
        rec = self.root
        for part in path.split("/"):
            rec = next((r for r in self.listdir(rec) if r["name"].upper() == part.upper()), None)
            if rec is None:
                return None
        return rec

    def copy_to(self, rec, dst, h=None):
        self.f.seek(rec["lba"] * SECTOR)
        left = rec["size"]
        with open(dst, "wb") as out:
            while left:
                chunk = self.f.read(min(left, 1 << 20))
                if not chunk:
                    die(f"{self.path} is truncated")
                out.write(chunk)
                if h:
                    h.update(chunk)
                left -= len(chunk)

    def read_file(self, rec):
        return self.read(rec["lba"] * SECTOR, rec["size"])


# --- steps ---------------------------------------------------------------

def obtain_iso(src):
    """Return the path of a raw image, unpacking a zip into iso/ if needed."""
    src = src.resolve()
    if not src.exists():
        die(f"{src} does not exist")
    isodir = ROOT / "iso"
    isodir.mkdir(exist_ok=True)
    if zipfile.is_zipfile(src):
        with zipfile.ZipFile(src) as z:
            members = [m for m in z.infolist() if m.filename.lower().endswith(".iso")]
            if len(members) != 1:
                die(f"{src} should contain exactly one .iso (found {len(members)})")
            m = members[0]
            dst = isodir / pathlib.Path(m.filename).name
            if dst.exists() and dst.stat().st_size == m.file_size:
                step(f"using already unpacked {dst.relative_to(ROOT)}")
                return dst
            step(f"unpacking {m.filename} ({m.file_size >> 20} MiB)")
            tmp = dst.with_suffix(".iso.part")
            with z.open(m) as i, open(tmp, "wb") as o:
                shutil.copyfileobj(i, o, 1 << 20)
            tmp.rename(dst)
            return dst
    if src.parent == isodir:
        return src
    link = isodir / src.name
    if link.is_symlink() or link.exists():
        if link.resolve() != src:
            die(f"{link} already exists and points elsewhere; remove it first")
    else:
        link.symlink_to(src)
    return link


def verify(iso):
    step("checking the image")
    umd = iso.find("UMD_DATA.BIN")
    if umd is None or not iso.read_file(umd).startswith(GAME_ID):
        got = iso.read_file(umd)[:10].decode("ascii", "replace") if umd else "no UMD_DATA.BIN"
        die(f"this is not SSX On Tour USA ({GAME_ID.decode()}); found: {got}")
    boot = iso.find("PSP_GAME/SYSDIR/BOOT.BIN")
    if boot is None:
        die("PSP_GAME/SYSDIR/BOOT.BIN is missing from the image")
    sha = hashlib.sha1(iso.read_file(boot)).hexdigest()
    if sha != BOOT_SHA1:
        die(f"BOOT.BIN has SHA-1 {sha}, expected {BOOT_SHA1}.\n"
            "       The image is a different revision or modified; only the retail "
            "ULUS-10042 release is supported.")
    print(f"    {GAME_ID.decode()}, BOOT.BIN {sha[:12]}… OK")


def extract(iso, force):
    out = ROOT / "extracted"
    stamp = out / ".complete"
    if stamp.exists() and not force:
        step("extracted/ is up to date")
        return
    step("extracting the UMD file system to extracted/")
    if out.exists():
        shutil.rmtree(out)
    tmp = ROOT / "extracted.part"
    if tmp.exists():
        shutil.rmtree(tmp)
    tmp.mkdir()
    n = total = 0
    for path, rec in iso.walk():
        dst = tmp / path
        if rec["dir"]:
            dst.mkdir(parents=True, exist_ok=True)
            continue
        dst.parent.mkdir(parents=True, exist_ok=True)
        iso.copy_to(rec, dst)
        n += 1
        total += rec["size"]
    (tmp / ".complete").write_text(BOOT_SHA1 + "\n")
    tmp.rename(out)
    print(f"    {n} files, {total >> 20} MiB")


def run(cmd, **kw):
    print("    $ " + " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], **kw)
    if r.returncode:
        die(f"command failed with exit code {r.returncode}")


def recompile(force):
    try:
        import rabbitizer  # noqa: F401
    except ImportError:
        die("the Python package 'rabbitizer' is missing; run ./setup.sh, "
            "or: pip install -r requirements.txt")
    py = sys.executable
    funcs = ROOT / "data/functions.json"
    if force or not funcs.exists():
        step("analysing BOOT.BIN (function discovery)")
        run([py, ROOT / "tools/analyze.py"])
    else:
        step("data/functions.json is up to date")
    step("recompiling MIPS to C++ (build/gen)")
    run([py, ROOT / "tools/recomp.py"])


def build(iso_path, jobs):
    for tool in ("cmake", "ninja", "clang++"):
        if not shutil.which(tool):
            die(f"'{tool}' not found; see README.md for the build dependencies")
    step("configuring and building")
    bdir = ROOT / "build/cmake"
    run(["cmake", "-S", ROOT, "-B", bdir, "-G", "Ninja",
         "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_CXX_COMPILER=clang++",
         f"-DSSX_ISO={iso_path.resolve()}"])
    run(["ninja", "-C", bdir, "ssx"] + (["-j", str(jobs)] if jobs else []))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("image", type=pathlib.Path, help="your SSX On Tour USA dump (.iso or .zip)")
    ap.add_argument("--no-build", action="store_true", help="stop after generating build/gen")
    ap.add_argument("--force", action="store_true", help="redo extraction and analysis")
    ap.add_argument("-j", "--jobs", type=int, default=0, help="parallel compile jobs")
    a = ap.parse_args()

    iso_path = obtain_iso(a.image)
    iso = Iso(iso_path)
    verify(iso)
    extract(iso, a.force)
    recompile(a.force)
    if not a.no_build:
        build(iso_path, a.jobs)
    step("done. Start the game with ./run.sh (options: ./run.sh --help)")


if __name__ == "__main__":
    main()
