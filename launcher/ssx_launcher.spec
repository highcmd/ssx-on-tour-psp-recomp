# PyInstaller spec: one-file SSXLauncher(.exe) carrying the launcher, Python, rabbitizer
# and the repository's source tree (git ls-files). It contains no game data.
#   pip install -r launcher/requirements.txt
#   pyinstaller launcher/ssx_launcher.spec
import os, subprocess
from PyInstaller.utils.hooks import collect_data_files

root = os.path.abspath(os.path.join(SPECPATH, ".."))
try:
    files = subprocess.run(["git", "ls-files", "-z"], cwd=root, capture_output=True,
                           check=True).stdout.decode().split("\0")
except (OSError, subprocess.CalledProcessError):   # no git (e.g. building under Wine): walk the tree
    files = ["CMakeLists.txt", "LICENSE", "README.md", "NOTES.md", "ssx.ini.example", "requirements.txt",
             "data/nids.json", "data/hooks.json"]
    for d in ("tools", "runtime", "cmake", "docs"):
        for dirpath, dirnames, names in os.walk(os.path.join(root, d)):
            dirnames[:] = [n for n in dirnames if n != "__pycache__"]
            files += [os.path.relpath(os.path.join(dirpath, n), root).replace(os.sep, "/") for n in names]
datas = []
for f in files:
    if f and not f.startswith(("launcher/", ".github/")):
        datas.append((os.path.join(root, f), os.path.join("src", os.path.dirname(f)) or "src"))
datas += collect_data_files("sv_ttk")

# The pipeline (tools/*.py) is imported from the unpacked source tree at run time,
# so PyInstaller can't see what it needs from the standard library: list it.
hidden = ["rabbitizer", "argparse", "glob", "hashlib", "struct", "zipfile", "tarfile", "lzma", "bz2",
          "gzip", "urllib.request", "ssl", "http.client", "runpy", "contextlib", "json", "bisect",
          "dataclasses", "re", "platform", "shutil", "subprocess", "collections", "itertools",
          "functools", "typing", "enum", "pathlib", "textwrap", "io", "copy", "math"]

a = Analysis([os.path.join(SPECPATH, "ssx_launcher.py")], datas=datas, hiddenimports=hidden)
pyz = PYZ(a.pure)
exe = EXE(pyz, a.scripts, a.binaries, a.datas, name="SSXLauncher", console=False,
          upx=False, icon=None)
