#!/usr/bin/env python3
"""Launcher for the Mat Hoffman's Pro BMX 2 (PS2) recompilation, on Windows.

Sets up, builds, updates and starts the game. You need your own copy of the
game (SLUS-20335, USA): put the .bin/.cue or .iso in the "Mat Hoffman 2" folder
next to this launcher. No other installs are needed: the build tools (a C
compiler and Python for the code generators) are downloaded into tools/, and
nothing is written outside the launcher's folder.

    python mh2launcher.py                     open the window
    python mh2launcher.py status              what is ready (no window)
    python mh2launcher.py download|update|build|play|update-launcher
    python mh2launcher.py --root DIR ...      keep everything in DIR instead
    python mh2launcher.py --disc FILE ...     use this disc image

Folders under the root: "Mat Hoffman 2" (your disc image), tools/ (compiler and
Python, downloaded), mhrecomp/ (the project: recompilers and runtime), work/
(files read from the disc, generated code, objects) and game/ (the built game,
mh2.ini and its log). Updates never touch game/, so your settings are kept.
"""
import errno
import hashlib
import http.client
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.request
import zipfile
import zlib
from concurrent.futures import ThreadPoolExecutor

# The project's online home (GitHub "owner/name"). Empty while it is not published:
# the launcher then uses the mhrecomp folder next to it and Update says so.
REPO = "ADHD-TISM/Mat-Hoffman-2-Recomp"
LAUNCHER_VERSION = "1"

GAME_NAME = "Mat Hoffman's Pro BMX 2"
GAME_SERIAL = "SLUS-20335"
GAME_EXE = "SLUS_203.35"
IOP_MODULES = [("PS2SOUND.IRX", "ps2sound", "80000")]   # (file on the disc, name, load address) for recomp_iop.py; runtime/iop.c links iop_mod_ps2sound
EE_CODE = "42bf00-432980"     # C++ static initialisers outside the text range (recomp_ee.py --code)

# Build tools: llvm-mingw (clang + a MinGW-w64 UCRT sysroot), pinned and checked. Same as the PS1 launcher.
LLVM_NAME = "llvm-mingw-20261104-ucrt-x86_64"
LLVM_URL = "https://github.com/mstorsjo/llvm-mingw/releases/download/20261104/%s.zip" % LLVM_NAME
LLVM_SHA256 = "552191b5754c49a5a20723276a6fc56541ea1922c4c50b7b669022333fc1b40f"
LLVM_SKIP = ("aarch64-w64-mingw32/", "armv7-w64-mingw32/", "i686-w64-mingw32/", "arm64ec-w64-mingw32/")
# Python for the code generators, used only when the launcher itself is a packaged exe.
PY_URL = "https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip"
PY_SHA256 = "8d3f33be9eb810f23c102f08475af2854e50484b8e4e06275e937be61ce3d2fb"

FROZEN = bool(getattr(sys, "frozen", False))
NO_WINDOW = 0x08000000
NEW_CONSOLE = 0x00000010
EXE_FILE = "mh2.exe"
INI_FILE = "mh2.ini"
UA = {"User-Agent": "mhrecomp-launcher"}
MARKER = ".downloaded-by-launcher"     # in a project folder the launcher installed (and so may replace)


class Fail(Exception):
    """A step failed; the message says what to do about it."""


# ---------------------------------------------------------------- paths/config

def default_root():
    """The folder the launcher lives in, or the one above it when it sits in launcher/ or launcher/dist/."""
    here = os.path.dirname(os.path.abspath(sys.executable if FROZEN else __file__))
    d = here
    for _ in range(3):
        if os.path.isdir(os.path.join(d, "Mat Hoffman 2")) or os.path.isdir(os.path.join(d, "mhrecomp")):
            return d
        d = os.path.dirname(d)
    return here


class Paths:
    def __init__(self, root):
        self.root = os.path.abspath(root)
        self.tools = os.path.join(self.root, "tools")
        self.project = os.path.join(self.root, "mhrecomp")
        self.work = os.path.join(self.root, "work")
        self.dl = os.path.join(self.work, "dl")
        self.discfiles = os.path.join(self.work, "disc")
        self.gen = os.path.join(self.work, "gen")
        self.gen_iop = os.path.join(self.work, "gen_iop")
        self.gen_vu = os.path.join(self.work, "gen_vu")
        self.obj = os.path.join(self.work, "obj")
        self.llvm = os.path.join(self.tools, "llvm-mingw")
        self.pydir = os.path.join(self.tools, "python")
        self.discdir = os.path.join(self.root, "Mat Hoffman 2")     # where people put their game
        self.game = os.path.join(self.root, "game")
        self.exe = os.path.join(self.game, EXE_FILE)
        self.ini = os.path.join(self.game, INI_FILE)
        self.cfg = os.path.join(self.root, "launcher.json")

    @property
    def clang(self):
        # MH2_CLANG: a developer's own llvm-mingw clang instead of the downloaded one
        return os.environ.get("MH2_CLANG") or os.path.join(self.llvm, "bin", "x86_64-w64-mingw32-clang.exe")

    @property
    def tool_python(self):
        return os.path.join(self.pydir, "python.exe") if FROZEN else sys.executable


class Config:
    def __init__(self, path):
        self.path = path
        self.lock = threading.RLock()   # the worker thread and the window both save
        try:
            with open(path, encoding="utf-8") as f:
                self.d = json.load(f)
        except (OSError, ValueError):
            self.d = {}

    def get(self, k, default=None):
        return self.d.get(k, default)

    def set(self, k, v):
        with self.lock:
            self.d[k] = v
            self.save()

    def save(self):
        with self.lock:
            tmp = self.path + ".tmp"
            os.makedirs(os.path.dirname(self.path), exist_ok=True)
            with open(tmp, "w", encoding="utf-8") as f:
                json.dump(self.d, f, indent=1, sort_keys=True)
            os.replace(tmp, self.path)


# ---------------------------------------------------------------- helpers

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch(url, dest, log, sha256=None, label=None):
    """Download url to dest (through a .part file), checking the SHA-256 if given."""
    label = label or os.path.basename(dest)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    part = dest + ".part"
    last = None
    for attempt in range(3):
        try:
            h = hashlib.sha256()
            req = urllib.request.Request(url, headers=UA)
            with urllib.request.urlopen(req, timeout=30) as r, open(part, "wb") as f:
                total = int(r.headers.get("Content-Length") or 0)
                got, shown = 0, -1
                while True:
                    b = r.read(1 << 18)
                    if not b:
                        break
                    f.write(b)
                    h.update(b)
                    got += len(b)
                    if total and got * 10 // total != shown:
                        shown = got * 10 // total
                        log("  %s: %d%% of %.1f MB" % (label, shown * 10, total / 1e6))
            if sha256 and h.hexdigest() != sha256:
                raise Fail("%s failed its checksum (corrupt or tampered download)" % label)
            os.replace(part, dest)
            return
        except (urllib.error.URLError, OSError, TimeoutError, http.client.HTTPException) as e:
            last = e   # HTTPException: e.g. IncompleteRead when a chunked archive is cut off
            log("  %s: %s%s" % (label, e, ", retrying" if attempt < 2 else ""))
            time.sleep(2)
    raise Fail("could not download %s: %s. Check your internet connection." % (label, last))


def safe_extract(zpath, dest, strip=0, skip=(), only=None):
    """Unpack a zip into dest, dropping `strip` leading path parts and refusing
    any member that would land outside dest. only: keep just the members whose
    path (after dropping strip - 1 parts) starts with this folder, e.g. "mhrecomp/"."""
    base = os.path.realpath(dest)
    os.makedirs(base, exist_ok=True)
    if only is not None:
        if strip < 1 or not only.endswith("/"):      # a real check, not an assert: kept under python -O
            raise ValueError("only= needs strip >= 1 and a folder name ending in /")
    try:
        with zipfile.ZipFile(zpath) as z:
            _extract_members(z, base, strip, skip, only)
    except (zipfile.BadZipFile, zlib.error, EOFError):
        raise Fail("the download is damaged; try again")
    except OSError as e:
        # a central directory whose offsets do not match the data makes zipfile seek
        # before the start (EINVAL); other OSErrors (disk full, access) keep their message
        if e.errno == errno.EINVAL and e.filename is None:
            raise Fail("the download is damaged; try again")
        raise


def _extract_members(z, base, strip, skip, only):
    """safe_extract's loop over the archive members."""
    for info in z.infolist():
        all_parts = [p for p in info.filename.split("/") if p]
        if only is not None and not ("/".join(all_parts[strip - 1:]) + "/").startswith(only):
            continue
        parts = all_parts[strip:]
        if not parts:
            continue
        rel = "/".join(parts)
        if any((rel + "/").startswith(s) for s in skip):
            continue
        target = os.path.realpath(os.path.join(base, *parts))
        if target != base and not target.startswith(base + os.sep):
            raise Fail("unsafe path in archive: %s" % info.filename)
        if info.is_dir() or info.filename.endswith("/"):
            os.makedirs(target, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(target), exist_ok=True)
        with z.open(info) as src, open(target, "wb") as out:
            shutil.copyfileobj(src, out, 1 << 20)


def rename_dir(src, dst):
    """os.replace for a folder, retried: on Windows a virus scanner holding a
    just-written file open makes the rename fail for a moment (WinError 5)."""
    for attempt in range(10):
        try:
            os.replace(src, dst)
            return
        except PermissionError:
            if attempt == 9:
                raise
            time.sleep(0.5)


def replace_dir(tmp, final):
    """Swap tmp in for final. final is first moved aside whole, so a failure
    never leaves it half deleted (with its launcher marker gone)."""
    old = final + ".old"
    shutil.rmtree(old, ignore_errors=True)
    if os.path.exists(old):
        raise Fail("%s is left from an earlier update and cannot be deleted (a file in it is open?). "
                   "Close other programs or delete it, then try again." % old)
    moved = False
    if os.path.isdir(final):
        rename_dir(final, old)
        moved = True
    try:
        rename_dir(tmp, final)
    except OSError:
        if moved and not os.path.exists(final):
            rename_dir(old, final)     # put the old one back
        raise
    shutil.rmtree(old, ignore_errors=True)


def run(cmd, log, cwd=None, env=None, tag=""):
    """Run a command, streaming its output to log. Returns the output text."""
    p = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         creationflags=NO_WINDOW, text=True, errors="replace")
    out = []
    for line in p.stdout:
        out.append(line)
        if tag:
            log(tag + line.rstrip())
    p.wait()
    text = "".join(out)
    if p.returncode != 0:
        raise Fail("%s failed (exit %d)\n%s" % (os.path.basename(cmd[0]), p.returncode, text[-2000:]))
    return text


def key_of(*parts):
    h = hashlib.sha256()
    for p in parts:
        h.update(p if isinstance(p, bytes) else str(p).encode("utf-8"))
        h.update(b"\0")
    return h.hexdigest()


def file_bytes(path):
    with open(path, "rb") as f:
        return f.read()


# ---------------------------------------------------------------- download/update

def version_tuple(tag):
    m = re.fullmatch(r"v?(\d+(?:\.\d+)*)", tag or "")
    return tuple(int(x) for x in m.group(1).split(".")) if m else None


def latest_tag(log, strict=False):
    """Newest vX.Y.Z tag of the project, or None (no tags, not published, or GitHub not reachable)."""
    if not REPO:
        return None
    try:
        req = urllib.request.Request("https://api.github.com/repos/%s/tags?per_page=100" % REPO, headers=UA)
        with urllib.request.urlopen(req, timeout=20) as r:
            tags = [t["name"] for t in json.load(r)]
    except (urllib.error.URLError, OSError, ValueError, TimeoutError, http.client.HTTPException) as e:
        if strict:
            raise Fail("could not look up the newest version (%s). Try again in a few minutes." % e)
        log("could not look up the newest version: %s" % e)
        return None
    tags = [t for t in tags if version_tuple(t)]
    return max(tags, key=version_tuple) if tags else None


def toolchain_ok(P):
    ok = os.path.isfile(P.clang)
    if FROZEN:
        ok = ok and os.path.isfile(P.tool_python)
    return ok


def install_toolchain(P, log):
    if not os.path.isfile(P.clang):
        z = os.path.join(P.dl, LLVM_NAME + ".zip")
        if not (os.path.isfile(z) and sha256_file(z) == LLVM_SHA256):
            log("downloading the C compiler (about 190 MB)")
            fetch(LLVM_URL, z, log, LLVM_SHA256, "compiler")
        log("unpacking the compiler")
        tmp = P.llvm + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        safe_extract(z, tmp, strip=1, skip=LLVM_SKIP)
        replace_dir(tmp, P.llvm)
        os.remove(z)
    else:
        log("build tools: compiler is installed")
    if FROZEN and not os.path.isfile(P.tool_python):
        z = os.path.join(P.dl, "python-embed.zip")
        log("downloading Python for the code generators (about 11 MB)")
        fetch(PY_URL, z, log, PY_SHA256, "python")
        tmp = P.pydir + ".tmp"
        shutil.rmtree(tmp, ignore_errors=True)
        safe_extract(z, tmp)
        replace_dir(tmp, P.pydir)
        os.remove(z)
    if not toolchain_ok(P):
        raise Fail("the build tools did not unpack correctly; delete the tools folder and try again")


PROJECT_FILES = [os.path.join("tools", n) for n in ("recomp_ee.py", "recomp_iop.py", "recomp_vu.py")] + \
                ["extra_ee.txt", "vu_recipe.json", os.path.join("runtime", "main.c")]


def project_ok(P):
    return all(os.path.isfile(os.path.join(P.project, n)) for n in PROJECT_FILES)


def is_working_copy(P):
    """A project folder the launcher did not download (a developer's own): never replaced."""
    return project_ok(P) and not os.path.isfile(os.path.join(P.project, MARKER))


def fetch_project_zip(P, tag, log):
    ref = "refs/tags/" + tag if tag else "refs/heads/main"
    url = "https://github.com/%s/archive/%s.zip" % (REPO, ref)
    z = os.path.join(P.dl, "project-%s.zip" % (tag or "main"))
    log("downloading the project (%s)" % (tag or "main"))
    fetch(url, z, log, None, "project")
    return z


def install_project(P, C, log, force=False):
    """Download the newest project version unless that one is already installed."""
    if is_working_copy(P):
        log("project: using your own copy in %s (the launcher never replaces it)" % P.project)
        return "local"
    if os.path.isdir(P.project) and not os.path.isfile(os.path.join(P.project, MARKER)) and os.listdir(P.project):
        raise Fail("%s exists but is incomplete and was not made by the launcher, so it is not replaced. "
                   "Move it away, then try again." % P.project)
    if not REPO:
        if project_ok(P):
            log("project is installed; there is no online copy to update from yet")
            return C.get("tag", "local")
        raise Fail("The project is not published online yet, so it cannot be downloaded. "
                   "Put the mhrecomp folder next to the launcher.")
    tag = latest_tag(log, strict=not (project_ok(P) and C.get("tag")))
    have = C.get("tag")
    if project_ok(P) and not force and have and (tag is None or tag == have):
        log("project is up to date (%s)" % have)
        return have
    z = fetch_project_zip(P, tag, log)
    tmp = P.project + ".tmp"
    shutil.rmtree(tmp, ignore_errors=True)
    try:
        # the repository archive is <repo>-<ref>/mhrecomp/... next to launcher/: unpack just that folder
        safe_extract(z, tmp, strip=2, only="mhrecomp/")
        if not all(os.path.isfile(os.path.join(tmp, n)) for n in PROJECT_FILES):
            raise Fail("the downloaded project looks wrong (files missing)")
        open(os.path.join(tmp, MARKER), "w").close()
        replace_dir(tmp, P.project)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
        try:
            os.remove(z)
        except OSError:
            pass
    C.set("tag", tag or "main")
    log("project installed: %s" % (tag or "main"))
    return tag or "main"


def update_launcher(P, C, log):
    """Replace this file with the launcher from the project's newest version."""
    if not REPO:
        raise Fail("The project is not published online yet, so there is no newer launcher to get.")
    if FROZEN:
        raise Fail("This is the packaged launcher: download the new launcher from the project's releases page.")
    tag = latest_tag(log, strict=True)
    z = fetch_project_zip(P, tag, log)
    try:
        try:
            with zipfile.ZipFile(z) as zf:
                names = [n for n in zf.namelist() if n.endswith("/launcher/mh2launcher.py")]
                if not names:
                    raise Fail("this version of the project has no launcher")
                new = zf.read(names[0])
        except (zipfile.BadZipFile, zlib.error, EOFError):
            raise Fail("the download is damaged; try again")
    finally:
        os.remove(z)
    me = os.path.abspath(__file__)
    m = re.search(rb'^LAUNCHER_VERSION = "(\d+)"', new, re.M)
    if not m:
        raise Fail("the downloaded launcher looks wrong; not replacing this one")
    if new == file_bytes(me) or int(m.group(1)) <= int(LAUNCHER_VERSION):
        log("the launcher is already the newest (version %s)" % LAUNCHER_VERSION)
        return False
    tmp = me + ".new"
    with open(tmp, "wb") as f:
        f.write(new)
    os.replace(tmp, me)
    log("launcher updated; restart it to use the new version")
    return True


# ---------------------------------------------------------------- disc image

def resolve_image(path):
    """The data file behind a .cue (or the .bin/.iso itself)."""
    if not path or not os.path.isfile(path):
        raise Fail("game image not found: %s" % (path or "(none chosen)"))
    if path.lower().endswith(".cue"):
        with open(path, encoding="utf-8", errors="replace") as f:
            m = re.search(r'^\s*FILE\s+"(.+?)"', f.read(), re.M | re.I)
        if not m:
            raise Fail("%s has no FILE line" % os.path.basename(path))
        b = os.path.join(os.path.dirname(os.path.abspath(path)), m.group(1))
        if not os.path.isfile(b):
            raise Fail("%s names %s, which is not next to it" % (os.path.basename(path), m.group(1)))
        return b
    return path


class Image:
    """Reads files from the root directory of a raw 2352-byte-sector .bin or a 2048-byte-sector .iso."""
    def __init__(self, path):
        self.path = resolve_image(path)
        n = os.path.getsize(self.path)
        if n % 2352 == 0:
            self.ss = 2352
        elif n % 2048 == 0:
            self.ss = 2048
        else:
            raise Fail("%s is not a disc image (its size fits neither .bin nor .iso sectors)" % os.path.basename(self.path))
        self.f = open(self.path, "rb")

    def close(self):
        self.f.close()

    def sector(self, lba):
        self.f.seek(lba * self.ss)
        raw = self.f.read(self.ss)
        if len(raw) < self.ss:
            return b"\0" * 2048
        if self.ss == 2048:
            return raw
        return raw[24:24 + 2048] if raw[15] == 2 else raw[16:16 + 2048]

    def root(self):
        """{NAME: (lba, size)} for the files in the root directory."""
        pvd = self.sector(16)
        if pvd[1:6] != b"CD001":
            raise Fail("%s is not a game disc image (no ISO 9660 file system)" % os.path.basename(self.path))
        lba, size = struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0]
        if not 0 < size <= 1 << 20:
            raise Fail("%s: bad root directory" % os.path.basename(self.path))
        data = b"".join(self.sector(lba + i) for i in range((size + 2047) // 2048))[:size]
        out, p = {}, 0
        while p + 34 < len(data):
            n = data[p]
            if n == 0:
                p = (p // 2048 + 1) * 2048
                continue
            k = data[p + 32]
            name = data[p + 33:p + 33 + k].decode("ascii", "replace").split(";")[0]
            if not data[p + 25] & 2:
                out[name.upper()] = (struct.unpack_from("<I", data, p + 2)[0], struct.unpack_from("<I", data, p + 10)[0])
            p += n
        return out

    def read(self, name):
        ent = self.root().get(name.upper())
        if not ent:
            raise Fail("%s is not on this disc: is it %s (%s)?" % (name, GAME_NAME, GAME_SERIAL))
        lba, size = ent
        return b"".join(self.sector(lba + i) for i in range((size + 2047) // 2048))[:size]


def image_fingerprint(path):
    b = resolve_image(path)
    st = os.stat(b)
    return "%s|%d|%d" % (os.path.abspath(b), st.st_size, st.st_mtime_ns)


def identify(C, path):
    """The boot file the disc's SYSTEM.CNF names (e.g. SLUS_203.35), or "" if none. Cached per image."""
    fp = image_fingerprint(path)
    cached = C.get("ident", {}).get(fp)
    if cached is None:
        im = Image(path)
        try:
            cnf = im.read("SYSTEM.CNF").decode("ascii", "replace")
            m = re.search(r"BOOT2\s*=\s*cdrom0:\\?([^;\s]+)", cnf, re.I)
            cached = m.group(1).upper() if m else ""
        except Fail:
            cached = ""
        finally:
            im.close()
        with C.lock:
            C.d.setdefault("ident", {})[fp] = cached
            C.save()
    return cached


def check_disc(C, path):
    """Raise Fail unless path is this game."""
    have = identify(C, path)
    if have != GAME_EXE:
        raise Fail("this image is %s, not %s (%s)" % (("a disc that boots " + have) if have else "not a PS2 game disc",
                                                     GAME_NAME, GAME_SERIAL))


def find_disc(P, C):
    """The first image of this game in the "Mat Hoffman 2" folder (or one folder inside it), or None."""
    try:
        os.makedirs(P.discdir, exist_ok=True)
        cands = {".cue": [], ".bin": [], ".iso": []}
        for dirpath, dirs, files in os.walk(P.discdir):
            if os.path.relpath(dirpath, P.discdir).count(os.sep) >= 1:
                dirs[:] = []
            for n in sorted(files):
                ext = os.path.splitext(n)[1].lower()
                if ext in cands:
                    cands[ext].append(os.path.join(dirpath, n))
        for path in cands[".cue"] + cands[".bin"] + cands[".iso"]:
            try:
                check_disc(C, path)
                return path
            except (Fail, OSError):
                continue
    except OSError:
        pass
    return None


def adopt_disc(P, C):
    """Use the image in the game folder when none that works is chosen. True if the choice changed."""
    cur = C.get("disc")
    try:
        if cur:
            check_disc(C, cur)
            return False
    except (Fail, OSError):
        pass
    found = find_disc(P, C)
    if found and os.path.abspath(found) != cur:
        C.set("disc", os.path.abspath(found))
        return True
    return False


# ---------------------------------------------------------------- generate

def gen_inputs(P):
    pr = P.project
    return [os.path.join(pr, "tools", n) for n in ("recomp_ee.py", "recomp_iop.py", "recomp_vu.py")] + \
           [os.path.join(pr, "extra_ee.txt"), os.path.join(pr, "vu_recipe.json")]


def regen_key(P, disc):
    return key_of("gen1", image_fingerprint(disc), *[file_bytes(p) for p in gen_inputs(P)])


def generated_ok(P, C, disc):
    try:
        return (bool(disc) and project_ok(P) and C.get("regen_key") == regen_key(P, disc)
                and os.path.isfile(os.path.join(P.gen, "table.c"))
                and os.path.isfile(os.path.join(P.gen_vu, "vu1_rec.h"))
                and all(os.path.isfile(os.path.join(P.gen_iop, "iop_%s.c" % n)) for _f, n, _a in IOP_MODULES))
    except (Fail, OSError):
        return False


def build_vu_snapshots(P, elf_dir, log):
    """Rebuild the VU1 code snapshots from the game's own files (vu_recipe.json says where each piece is)."""
    with open(os.path.join(P.project, "vu_recipe.json"), encoding="utf-8") as f:
        recipe = json.load(f)
    outs, srcs = [], {}
    for snap in recipe["snapshots"]:
        code = bytearray(snap["size"])
        for pc in snap["pieces"]:
            name = pc["from_file"]
            if name not in srcs:
                srcs[name] = file_bytes(os.path.join(elf_dir, name))
            piece = srcs[name][pc["file"]:pc["file"] + pc["len"]]
            if len(piece) != pc["len"]:
                raise Fail("%s is shorter than expected: is the disc %s?" % (name, GAME_SERIAL))
            code[pc["vu"]:pc["vu"] + pc["len"]] = piece
        if hashlib.sha256(code).hexdigest() != snap["sha256"]:
            raise Fail("VU1 program %s read from the disc does not match: is the disc %s, version 1.00?"
                       % (snap["hash"], GAME_SERIAL))
        base = os.path.join(P.gen_vu, "vu1_%s" % snap["hash"])
        with open(base + ".bin", "wb") as f:
            f.write(code)
        with open(base + ".pcs", "w") as f:
            f.write("\n".join(snap["pcs"]) + "\n")
        outs.append(base + ".bin")
    log("  %d VU1 program(s) rebuilt from the disc" % len(outs))
    return outs


def generate(P, C, disc, log):
    if generated_ok(P, C, disc):
        log("generated code is up to date")
        return
    if not project_ok(P):
        raise Fail("the project is missing (Update tab)")
    check_disc(C, disc)
    key = regen_key(P, disc)
    py = P.tool_python
    tools = os.path.join(P.project, "tools")
    C.set("regen_key", None)
    for d in (P.discfiles, P.gen, P.gen_iop, P.gen_vu):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    log("reading the game files from the disc image")
    im = Image(disc)
    try:
        for name in [GAME_EXE] + [f for f, _n, _a in IOP_MODULES]:
            with open(os.path.join(P.discfiles, name), "wb") as f:
                f.write(im.read(name))
    finally:
        im.close()
    elf = os.path.join(P.discfiles, GAME_EXE)
    log("translating the game code (EE, a few seconds)")
    run([py, "-I", os.path.join(tools, "recomp_ee.py"), elf, P.gen, "--extra", os.path.join(P.project, "extra_ee.txt"),
         "--code", EE_CODE], log, cwd=P.project)
    for fname, name, addr in IOP_MODULES:
        log("translating %s (IOP)" % fname)
        run([py, "-I", os.path.join(tools, "recomp_iop.py"), os.path.join(P.discfiles, fname), name, addr, P.gen_iop],
            log, cwd=P.project)
    log("translating the VU1 microcode")
    snaps = build_vu_snapshots(P, P.discfiles, log)
    run([py, "-I", os.path.join(tools, "recomp_vu.py"), os.path.join(P.gen_vu, "vu1_rec.h")] + snaps, log, cwd=P.project)
    C.set("regen_key", key)
    log("code translated")


# ---------------------------------------------------------------- compile

def _sources(P):
    rt = os.path.join(P.project, "runtime")
    src = [os.path.join(rt, n) for n in sorted(os.listdir(rt)) if n.endswith(".c")]
    src += [os.path.join(P.gen, n) for n in sorted(os.listdir(P.gen)) if n.endswith(".c")]
    src += [os.path.join(P.gen_iop, n) for n in sorted(os.listdir(P.gen_iop)) if n.endswith(".c")]
    hdr = []
    for d in (rt, P.gen, P.gen_vu):
        hdr += [os.path.join(d, n) for n in sorted(os.listdir(d)) if n.endswith(".h")]
    return src, hdr


def _flags(P, src):
    # the options build.sh uses, without debug info
    f = ["-O2", "-fno-strict-aliasing", "-fwrapv", "-I", os.path.join(P.project, "runtime"), "-I", P.gen, "-I", P.gen_vu]
    if os.path.dirname(src) == os.path.join(P.project, "runtime"):
        f += ["-Wall", "-Wno-unused-but-set-variable", "-Wno-unused-function"]
    else:
        f += ["-w"]
    return f


LINK = ["-static", "-Wl,--stack,67108864", "-lwinmm", "-lgdi32", "-luser32", "-lopengl32"]


def build_key(P):
    src, hdr = _sources(P)
    h = hashlib.sha256(b"build1")
    for p in hdr + src:
        h.update(os.path.basename(p).encode())
        h.update(file_bytes(p))
    return h.hexdigest()


def built_ok(P, C):
    try:
        return (os.path.isfile(P.exe) and os.path.isfile(os.path.join(P.game, GAME_EXE))
                and os.path.isdir(P.gen) and C.get("build_key") == build_key(P))
    except OSError:
        return False


def compile_game(P, C, log, progress=None):
    if not toolchain_ok(P):
        raise Fail("the build tools are not downloaded yet (Update tab)")
    if built_ok(P, C):
        log("game is up to date")
        return
    src, hdr = _sources(P)
    hh = hashlib.sha256(b"".join(file_bytes(p) for p in hdr)).hexdigest()
    os.makedirs(P.obj, exist_ok=True)
    keyfile = os.path.join(P.obj, "keys.json")
    try:
        with open(keyfile) as f:
            keys = json.load(f)
    except (OSError, ValueError):
        keys = {}
    env = dict(os.environ)
    env["PATH"] = os.path.dirname(P.clang) + os.pathsep + env.get("PATH", "")
    todo, objs, newkeys = [], [], {}
    for p in src:
        tag = os.path.basename(os.path.dirname(p)) + "_" + os.path.basename(p)     # runtime_main.c, gen_code_000.c
        o = os.path.join(P.obj, tag[:-2] + ".o")
        objs.append(o)
        k = key_of(hh, " ".join(_flags(P, p)), P.clang, file_bytes(p))
        newkeys[tag] = k
        if not (os.path.isfile(o) and keys.get(tag) == k):
            todo.append((tag, p, o))
    # vu.c (with the recompiled VU1 programs) is still the largest file: start it first
    todo.sort(key=lambda t: t[1].endswith(os.sep + "vu.c"), reverse=True)
    log("compiling %d of %d files%s" % (len(todo), len(src), " (the first build takes about a minute)" if len(todo) > 20 else ""))
    done = [0]
    errors = []
    ok_done = {}
    lock = threading.Lock()

    def one(t):
        tag, p, o = t
        if errors:
            return
        tmp = o + ".tmp"
        try:
            run([P.clang, "-c", p, "-o", tmp] + _flags(P, p), lambda s: None, cwd=P.root, env=env)
            os.replace(tmp, o)
            ok_done[tag] = True
        except Fail as e:
            errors.append(str(e))
        with lock:
            done[0] += 1
            if progress:
                progress(done[0], len(todo))
            if done[0] % 10 == 0 or done[0] == len(todo):
                log("  %d / %d" % (done[0], len(todo)))

    if todo:
        for tag, _p, _o in todo:
            keys.pop(tag, None)
        with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 2)) as ex:
            list(ex.map(one, todo))
        if errors:     # keep the keys of what did compile, so the next build does not redo it
            pending = set(t[0] for t in todo)
            with open(keyfile, "w") as f:
                json.dump({k: v for k, v in newkeys.items() if k not in pending or ok_done.get(k)}, f)
            raise Fail("compile error:\n" + errors[0])
    with open(keyfile, "w") as f:
        json.dump(newkeys, f)
    log("linking")
    os.makedirs(P.game, exist_ok=True)
    tmp = P.exe + ".new"
    run([P.clang] + objs + ["-o", tmp] + LINK, log, cwd=P.root, env=env)
    try:
        os.replace(tmp, P.exe)
    except OSError:
        os.remove(tmp)
        raise Fail("%s is in use: close the game and build again" % EXE_FILE)
    shutil.copyfile(os.path.join(P.discfiles, GAME_EXE), os.path.join(P.game, GAME_EXE))   # the runtime loads it at start
    if not os.path.isfile(P.ini):
        with open(P.ini, "w", encoding="utf-8", newline="\n") as f:
            f.write(default_ini_text())
    C.set("build_key", build_key(P))
    log("game built: %s" % P.exe)


def build_all(P, C, disc, log, progress=None):
    if not disc:
        raise Fail("put your game in the \"Mat Hoffman 2\" folder or choose it first (Build tab)")
    if not toolchain_ok(P) or not project_ok(P):
        raise Fail("download the build tools first (Update tab)")
    generate(P, C, disc, log)
    compile_game(P, C, log, progress)


# ---------------------------------------------------------------- settings (mh2.ini)

KEYNAMES = ("up down left right enter backspace tab space lshift rshift lctrl rctrl lalt ralt "
            "comma period slash semicolon num0 num1 num2 num3 num4 num5 num6 num7 num8 num9 none").split()
B01 = ["0", "1"]

# (key, tab, label, kind, choices, default, comment)
SETTINGS = [
    ("frame_limit", "Gameplay", "Run at console speed (60 fps)", "bool", B01, "1",
     "1 = the console's speed, 0 = as fast as the PC can go (for testing)"),
    ("gamepad", "Gameplay", "Use an Xbox-style gamepad", "bool", B01, "1",
     "1 = use XInput gamepads (A = cross, B = circle, X = square, Y = triangle) for player 1 and the Player 2 pad setting; 0 = keyboard only"),
    ("rumble", "Gameplay", "Gamepad vibration", "bool", B01, "1",
     "1 = the gamepad vibrates when the game asks for it (also needs Vibration: On in the game's Options > Controller)"),
    ("pad2", "Gameplay", "Player 2 pad", "choice", ["gamepad2", "gamepad1", "off"], "gamepad2",
     "Player 2 (Multiplayer): gamepad2 = second gamepad, gamepad1 = first gamepad (player 1 uses the keyboard), off. Needs gamepads on"),
    ("skip_intro", "Gameplay", "Skip intro movies", "bool", B01, "0",
     "1 = skip the start-up movies (Acclaim and Z-Axis logos still show 2 s each); movies in Cool Stuff still play"),
    ("scale", "Video", "Internal resolution", "choice", ["1", "2", "3", "4"], "2",
     "Internal 3D resolution: 1 = original PS2, 2-4 = sharper, with smoother edges (supersampled with the smooth filter; more GPU work)"),
    ("window_size", "Video", "Window size", "combo", ["auto", "640x480", "960x720", "1280x960", "1600x1200",
                                                      "1280x720", "1920x1080"], "auto",
     "Window size in pixels (WxH). auto = 1280x960 (1280x720 with widescreen), made smaller if the screen is"),
    ("vsync", "Video", "Vertical sync", "bool", B01, "1",
     "1 = show frames in step with a 60 or 120 Hz screen (smoother); 0 = timer only"),
    ("fullscreen", "Video", "Start in fullscreen", "bool", B01, "0",
     "1 = start in borderless fullscreen (F11 or Alt+Enter toggles while playing)"),
    ("widescreen", "Video", "Widescreen (16:9)", "bool", B01, "0",
     "1 = a wider 3D view shown at 16:9 (the menus and on-screen text are stretched)"),
    ("aspect", "Video", "Aspect ratio", "choice", ["4:3", "stretch"], "4:3",
     "4:3 = console shape with black bars, stretch = fill the window"),
    ("filter", "Video", "Picture filter", "choice", ["smooth", "nearest"], "smooth",
     "How the picture is enlarged to the window: smooth = blended, nearest = sharp pixels"),
    ("volume", "Audio", "Volume (%)", "choice", ["100", "75", "50", "25", "0"], "100",
     "Game sound volume in percent (music, effects and movie sound)"),
    ("gs_thread", "Debug", "Graphics on its own thread", "bool", B01, "1",
     "1 = VIF1, VU1 and drawing run on a second CPU thread (smoother); 0 = all on one thread"),
    ("async_tex_sync", "Debug", "Approximate texture write-back", "bool", B01, "0",
     "1 = textures read from GPU drawing use a copy 2-4 frames old (faster, inexact); 0 = exact"),
    ("vu_mode", "Debug", "VU1 code", "choice", ["recompiled", "interpreter", "check"], "recompiled",
     "recompiled = fast; interpreter = run VU1 microcode in the interpreter; check = run both and compare (slow)"),
]
KEYS = [("up", "up"), ("down", "down"), ("left", "left"), ("right", "right"), ("start", "enter"),
        ("select", "backspace, rshift"), ("cross", "K, space"), ("circle", "L"), ("square", "J"), ("triangle", "I"),
        ("l1", "Q"), ("r1", "E"), ("l2", "1"), ("r2", "3"), ("l3", "Z"), ("r3", "C"),
        ("lstick_up", "W"), ("lstick_down", "S"), ("lstick_left", "A"), ("lstick_right", "D"),
        ("rstick_up", "num8"), ("rstick_down", "num2"), ("rstick_left", "num4"), ("rstick_right", "num6")]
for _n, _d in KEYS:
    SETTINGS.append(("key_" + _n, "Input", "Key: " + _n.replace("_", " ").upper(), "key", None, _d, None))
SCHEMA = {s[0]: s for s in SETTINGS}
SETTING_TABS = ["Gameplay", "Video", "Input", "Audio", "Debug", "All settings"]


def default_ini_text():
    out = ["# Mat Hoffman's Pro BMX 2 recomp settings (mh2.ini). Delete this file to get the defaults back.",
           "# The launcher's Settings tab edits it; updates never replace it.", ""]
    for key, _tab, _lab, kind, _ch, dflt, comment in SETTINGS:
        if comment:
            out.append("# " + comment)
        if key == "key_up":
            out.append("# Keyboard: a letter or digit, or " + " ".join(KEYNAMES) + "; two keys separated by a comma")
        out.append("%s = %s" % (key, dflt))
    return "\n".join(out) + "\n"


_INI_LINE = re.compile(r"^\s*([A-Za-z0-9_]+)\s*=\s*(.*?)\s*$")


def read_ini(path):
    """Settings in the file (name -> value); the defaults for anything missing."""
    vals = {s[0]: s[5] for s in SETTINGS}
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                if not s or s[0] in "#;":
                    continue
                m = _INI_LINE.match(line)
                if m:
                    vals[m.group(1).lower()] = m.group(2)
    except OSError:
        pass
    return vals


def valid_setting(key, val):
    s = SCHEMA.get(key)
    if not s:
        return True
    if s[3] in ("choice", "bool"):
        return val.lower() in [c.lower() for c in s[4]]
    if s[3] == "combo":
        m = re.fullmatch(r"(\d{3,5})x(\d{3,5})", val)
        return val.lower() == s[5] or bool(m and 320 <= int(m.group(1)) <= 16384 and 240 <= int(m.group(2)) <= 16384)
    parts = [p.strip() for p in val.split(",")]
    if not 1 <= len(parts) <= 2:
        return False
    return all((len(p) == 1 and p.isalnum() and p.isascii()) or p.lower() in KEYNAMES for p in parts)


def write_ini(path, values):
    """Write values into the file, keeping its comments, order and unknown lines."""
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().splitlines()
    except OSError:
        lines = default_ini_text().splitlines()
    todo = dict(values)
    out = []
    for line in lines:
        m = _INI_LINE.match(line)
        if m and not line.lstrip().startswith(("#", ";")) and m.group(1).lower() in todo:
            k = m.group(1).lower()
            out.append("%s = %s" % (m.group(1), todo.pop(k)))
        else:
            out.append(line)
    for k, v in todo.items():
        out.append("%s = %s" % (k, v))
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(out) + "\n")
    os.replace(tmp, path)


# ---------------------------------------------------------------- play

DEBUG_FLAGS = [("--log-sys 1", "Log kernel calls (system calls, threads)"),
               ("--log-pad", "Log controller traffic"),
               ("--log-iop", "Log the I/O processor"),
               ("--log-ipu", "Log the movie decoder")]


def play(P, C, log):
    if not os.path.isfile(P.exe):
        raise Fail("the game is not built yet (Build tab)")
    disc = C.get("disc")
    if not disc:
        raise Fail("choose your game image first (Build tab)")
    image = resolve_image(disc)
    args = [P.exe, "--image", os.path.abspath(image)]
    for f, _ in DEBUG_FLAGS:
        if f in C.get("debug_flags", []):
            args += f.split()
    args.append(P.game)
    if C.get("debug_console", False):
        subprocess.Popen(args, cwd=P.game, creationflags=NEW_CONSOLE)
    else:
        logf = open(os.path.join(P.game, "mh2.log"), "w")    # what the game prints, for when something goes wrong
        subprocess.Popen(args, cwd=P.game, creationflags=NO_WINDOW, stdout=logf, stderr=subprocess.STDOUT)
        logf.close()
    log("started the game (its messages go to %s)" % os.path.join(P.game, "mh2.log"))


# ---------------------------------------------------------------- status

def status(P, C):
    """(key, label, ok, tab_to_fix_it) for each requirement."""
    disc = C.get("disc")
    disc_ok = False
    try:
        if disc:
            check_disc(C, disc)
            disc_ok = True
    except (Fail, OSError):
        pass
    return [
        ("tools", "Build tools", toolchain_ok(P), "Update"),
        ("project", "Project files", project_ok(P), "Update"),
        ("disc", "Your game (%s)" % GAME_SERIAL, disc_ok, "Build"),
        ("gen", "Game code generated", generated_ok(P, C, disc) if disc_ok else False, "Build"),
        ("built", "Game built", built_ok(P, C) if disc_ok else os.path.isfile(P.exe), "Build"),
    ]


def ready_to_play(P, C):
    st = {k: ok for k, _l, ok, _t in status(P, C)}
    return os.path.isfile(P.exe) and st["disc"]


# ---------------------------------------------------------------- command line

def open_session(root, disc=None):
    P = Paths(root or default_root())
    C = Config(P.cfg)
    if disc:
        check_disc(C, disc)
        C.set("disc", os.path.abspath(disc))
    else:
        adopt_disc(P, C)
    return P, C


def cli(argv):
    root, disc, cmd = None, None, []
    it = iter(argv)
    for a in it:
        if a == "--root":
            root = next(it, None)
        elif a == "--disc":
            disc = next(it, None)
        else:
            cmd.append(a)
    try:
        P, C = open_session(root, disc)
    except (Fail, OSError) as e:
        print("error: %s" % e)
        return 1
    log = lambda s: print(s, flush=True)
    what = cmd[0] if cmd else "status"
    try:
        if what == "status":
            print("root: %s" % P.root)
            for _k, label, ok, tab in status(P, C):
                print("%s %s%s" % ("OK " if ok else "-- ", label, "" if ok else "   (%s tab)" % tab))
            print("game image: %s" % C.get("disc", "none"))
        elif what == "download":
            install_toolchain(P, log)
            install_project(P, C, log)
        elif what == "update":
            install_project(P, C, log)
        elif what == "update-launcher":
            update_launcher(P, C, log)
        elif what == "build":
            build_all(P, C, C.get("disc"), log)
        elif what == "play":
            play(P, C, log)
        else:
            print(__doc__)
            return 2
    except (Fail, OSError) as e:
        print("error: %s" % e)
        return 1
    return 0


# ---------------------------------------------------------------- window

LIGHT = dict(bg="#f0f0f0", fg="#000000", field="#ffffff", btn="#e1e1e1", btn_hot="#d0d0d0", line="#adadad",
             muted="#6b6b6b", warn="#b36b00", ok="#1a7f37", bad="#c62828", accent="#0078d7")
DARK = dict(bg="#1e1f22", fg="#e6e6e6", field="#2b2d31", btn="#3a3c42", btn_hot="#4a4d55", line="#4a4d55",
            muted="#9a9da5", warn="#e0a030", ok="#4cc366", bad="#ff6b6b", accent="#3b82f6")


def system_prefers_dark():
    try:
        import winreg
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER,
                            r"Software\Microsoft\Windows\CurrentVersion\Themes\Personalize") as k:
            return winreg.QueryValueEx(k, "AppsUseLightTheme")[0] == 0
    except OSError:
        return False


def gui(root_dir, notice=None):
    import queue
    import tkinter as tk
    from tkinter import filedialog, messagebox, ttk

    P = Paths(root_dir)
    C = Config(P.cfg)
    win = tk.Tk()
    win.title("Mat Hoffman 2 Recomp launcher")
    win.geometry("780x660")
    win.minsize(660, 540)
    q = queue.Queue()
    busy = threading.Event()

    def log(s):
        q.put(("log", s))

    style = ttk.Style(win)
    native_theme = style.theme_use()
    plain = []          # classic tk widgets that ttk's theme does not colour
    pal = dict(LIGHT)

    def apply_theme(dark):
        pal.update(DARK if dark else LIGHT)
        if dark:
            style.theme_use("clam")
            style.configure(".", background=pal["bg"], foreground=pal["fg"], fieldbackground=pal["field"],
                            bordercolor=pal["line"], lightcolor=pal["bg"], darkcolor=pal["bg"],
                            troughcolor=pal["field"], insertcolor=pal["fg"], focuscolor=pal["bg"])
            style.map(".", foreground=[("disabled", pal["muted"])])
            style.configure("TButton", background=pal["btn"], padding=(8, 3))
            style.map("TButton", background=[("active", pal["btn_hot"]), ("disabled", pal["bg"])])
            style.configure("TNotebook", background=pal["bg"])
            style.configure("TNotebook.Tab", background=pal["btn"], foreground=pal["fg"], padding=(10, 3))
            style.map("TNotebook.Tab", background=[("selected", pal["bg"])])
            style.configure("TCombobox", arrowcolor=pal["fg"], selectbackground=pal["field"], selectforeground=pal["fg"])
            style.map("TCombobox", fieldbackground=[("readonly", pal["field"])], foreground=[("readonly", pal["fg"])])
            style.configure("TCheckbutton", indicatorbackground=pal["field"], indicatorforeground=pal["fg"])
            style.configure("TProgressbar", background=pal["accent"])
            win.option_add("*TCombobox*Listbox.background", pal["field"])
            win.option_add("*TCombobox*Listbox.foreground", pal["fg"])
            win.option_add("*TCombobox*Listbox.selectBackground", pal["accent"])
        else:
            style.theme_use(native_theme)
            win.option_add("*TCombobox*Listbox.background", "white")
            win.option_add("*TCombobox*Listbox.foreground", "black")
            win.option_add("*TCombobox*Listbox.selectBackground", "#0078d7")
        style.configure("Muted.TLabel", foreground=pal["muted"])
        style.configure("Warn.TLabel", foreground=pal["warn"])
        win.configure(background=pal["bg"] if dark else style.lookup("TFrame", "background") or pal["bg"])
        for w in plain:
            w.configure(background=pal["field"] if isinstance(w, tk.Text) else pal["bg"])
            if isinstance(w, tk.Text):
                w.configure(foreground=pal["fg"], insertbackground=pal["fg"])
        try:                       # dark title bar on Windows 10/11
            import ctypes
            hwnd = ctypes.windll.user32.GetParent(win.winfo_id())
            v = ctypes.c_int(1 if dark else 0)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(hwnd, 20, ctypes.byref(v), 4)
        except Exception:
            pass

    dark_var = tk.BooleanVar(value=C.get("dark", system_prefers_dark()))

    top = ttk.Frame(win, padding=(10, 8))
    top.pack(fill="x")
    ttk.Label(top, text="Mat Hoffman's Pro BMX 2", font=("Segoe UI", 14, "bold")).pack(side="left")
    ttk.Label(top, text="  PS2 recomp", style="Muted.TLabel").pack(side="left")
    ver = ttk.Label(top, text="")
    ver.pack(side="right")

    def toggle_dark():
        C.set("dark", bool(dark_var.get()))
        apply_theme(dark_var.get())
        refresh()

    ttk.Checkbutton(top, text="Dark mode", variable=dark_var, command=toggle_dark).pack(side="right", padx=16)

    nb = ttk.Notebook(win)
    nb.pack(fill="both", expand=True, padx=10)
    tabs = {}
    for name in ("Play", "Build", "Update", "Settings"):
        fr = ttk.Frame(nb, padding=12)
        nb.add(fr, text=name)
        tabs[name] = fr

    bar = ttk.Progressbar(win, mode="determinate")
    bar.pack(fill="x", padx=10, pady=(6, 0))
    logbox = tk.Text(win, height=9, wrap="word", state="disabled", font=("Consolas", 9))
    logbox.pack(fill="both", padx=10, pady=8)
    plain.append(logbox)

    buttons = []

    def lock(b):
        buttons.append(b)
        return b

    # ---- Play tab
    p = tabs["Play"]
    ttk.Label(p, text="What is ready", font=("Segoe UI", 10, "bold")).pack(anchor="w")
    rows = {}
    for key, label, _ok, _tab in status(P, C):
        l = ttk.Label(p, text=label, cursor="hand2", font=("Segoe UI", 11))
        l.pack(anchor="w", pady=2)
        rows[key] = l
    ttk.Label(p, text="Click a ✗ to go where it is fixed.", style="Muted.TLabel").pack(anchor="w", pady=(0, 10))
    play_btn = lock(ttk.Button(p, text="Play"))
    play_btn.pack(anchor="w", ipadx=24, ipady=6)
    close_var = tk.BooleanVar(value=C.get("close_on_play", False))

    def set_close():
        C.set("close_on_play", bool(close_var.get()))

    ttk.Checkbutton(p, text="Close the launcher when the game starts", variable=close_var,
                    command=set_close).pack(anchor="w", pady=(8, 0))
    ttk.Label(p, style="Muted.TLabel", wraplength=680,
              text="Keyboard: arrows = d-pad, W A S D = left stick, K / space = cross, L = circle, J = square, "
                   "I = triangle, Q / E = L1 / R1, 1 / 3 = L2 / R2, Enter = start. An Xbox-style gamepad works too. "
                   "F11 or Alt+Enter = fullscreen.").pack(anchor="w", pady=(10, 0))
    upd_note = ttk.Label(p, text="", style="Warn.TLabel")
    upd_note.pack(anchor="w", pady=8)

    # ---- Build tab
    b = tabs["Build"]
    ttk.Label(b, text="Your game image (%s, %s): .cue + .bin, or .iso" % (GAME_NAME, GAME_SERIAL)).pack(anchor="w")
    ttk.Label(b, style="Muted.TLabel", wraplength=680,
              text="Put it in the \"Mat Hoffman 2\" folder next to the launcher and it is found automatically, "
                   "or browse to it anywhere. You need your own copy of the game.").pack(anchor="w")
    disc_var = tk.StringVar(value=C.get("disc", ""))
    row = ttk.Frame(b)
    row.pack(fill="x", pady=4)
    ttk.Entry(row, textvariable=disc_var).pack(side="left", fill="x", expand=True)

    def browse():
        if busy.is_set():
            return
        f = filedialog.askopenfilename(title="Choose the game image",
                                       filetypes=[("Disc image", "*.cue *.bin *.iso"), ("All files", "*.*")])
        if f:
            disc_var.set(os.path.normpath(f))
            C.set("disc", disc_var.get())
            refresh()

    def open_discdir():
        os.makedirs(P.discdir, exist_ok=True)
        os.startfile(P.discdir)

    ttk.Button(row, text="Browse...", command=browse).pack(side="left", padx=(6, 0))
    ttk.Button(row, text="Open folder", command=open_discdir).pack(side="left", padx=(6, 0))
    disc_msg = ttk.Label(b, text="", wraplength=660)
    disc_msg.pack(anchor="w", pady=4)
    build_btn = lock(ttk.Button(b, text="Build the game"))
    build_btn.pack(anchor="w", pady=6, ipadx=16, ipady=4)
    ttk.Label(b, wraplength=680, style="Muted.TLabel",
              text="Checks your game, reads its code from the disc image, translates it to C and builds the game "
                   "into the game folder. Steps that are already up to date are skipped. The first build takes "
                   "about a minute.").pack(anchor="w")

    # ---- Update tab
    u = tabs["Update"]
    info = ttk.Label(u, text="", justify="left")
    info.pack(anchor="w", pady=(0, 8))
    dl_btn = lock(ttk.Button(u, text="Download (build tools and project, about 200 MB)"))
    dl_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    up_btn = lock(ttk.Button(u, text="Update the project"))
    up_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    ul_btn = lock(ttk.Button(u, text="Update launcher"))
    ul_btn.pack(anchor="w", pady=4, ipadx=8, ipady=3)
    ttk.Label(u, wraplength=680, style="Muted.TLabel",
              text="Updates never touch the game folder, so your settings are kept. After updating the project, "
                   "build again.").pack(anchor="w", pady=8)

    # ---- Settings tab
    s = tabs["Settings"]
    svars = {}
    for k, v in read_ini(P.ini).items():
        svars[k] = tk.StringVar(value=v)
    snb = ttk.Notebook(s)
    snb.pack(fill="both", expand=True)
    stabs = {}
    for name in SETTING_TABS:
        fr = ttk.Frame(snb, padding=8)
        snb.add(fr, text=name)
        stabs[name] = fr

    def scrolled(parent):
        cv = tk.Canvas(parent, highlightthickness=0, background=pal["bg"])
        plain.append(cv)
        sb = ttk.Scrollbar(parent, orient="vertical", command=cv.yview)
        inner = ttk.Frame(cv)
        inner.bind("<Configure>", lambda e: cv.configure(scrollregion=cv.bbox("all")))
        cv.create_window((0, 0), window=inner, anchor="nw")
        cv.configure(yscrollcommand=sb.set)
        cv.pack(side="left", fill="both", expand=True)
        sb.pack(side="right", fill="y")
        return inner

    def widget_for(parent, key, label, r):
        sc = SCHEMA.get(key)
        ttk.Label(parent, text=label).grid(row=r, column=0, sticky="w", padx=(0, 12), pady=2)
        var = svars.setdefault(key, tk.StringVar(value=""))
        if sc and sc[3] == "bool":
            w = ttk.Checkbutton(parent, variable=var, onvalue="1", offvalue="0")
        elif sc and sc[3] == "choice":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], state="readonly", width=14)
        elif sc and sc[3] == "combo":
            w = ttk.Combobox(parent, textvariable=var, values=sc[4], width=14)
        else:
            w = ttk.Entry(parent, textvariable=var, width=24)
        w.grid(row=r, column=1, sticky="w", pady=2)

    for name in ("Gameplay", "Video", "Input", "Debug"):
        inner = scrolled(stabs[name]) if name == "Input" else stabs[name]
        r = 0
        for key, tab, label, *_ in SETTINGS:
            if tab == name:
                widget_for(inner, key, label, r)
                r += 1
        if name == "Input":
            ttk.Label(inner, style="Muted.TLabel", wraplength=520,
                      text="Keys: a letter or digit, or up down left right enter backspace tab space lshift rshift "
                           "lctrl rctrl lalt ralt comma period slash semicolon num0..num9 none. Two keys separated "
                           "by a comma.").grid(row=r, column=0, columnspan=2, sticky="w", pady=6)
        if name == "Debug":
            dbg = ttk.Frame(stabs["Debug"])
            dbg.grid(row=r, column=0, columnspan=2, sticky="w", pady=(12, 0))

    dc = tk.BooleanVar(value=C.get("debug_console", False))
    ttk.Checkbutton(dbg, text="Show the console window while playing", variable=dc).pack(anchor="w", pady=2)
    dflags = {}
    for flag, text in DEBUG_FLAGS:
        v = tk.BooleanVar(value=flag in C.get("debug_flags", []))
        dflags[flag] = v
        ttk.Checkbutton(dbg, text="%s   %s" % (flag, text), variable=v).pack(anchor="w", pady=2)
    ttk.Label(dbg, style="Muted.TLabel", wraplength=560,
              text="The console window and log flags are launcher options, not part of mh2.ini. Without the "
                   "console, the game's messages go to mh2.log in the game folder.").pack(anchor="w", pady=8)

    allinner = scrolled(stabs["All settings"])

    def fill_all():
        for w in allinner.winfo_children():
            w.destroy()
        for r, key in enumerate(sorted(svars)):
            widget_for(allinner, key, key, r)

    fill_all()

    def save_settings():
        vals = {k: v.get().strip() for k, v in svars.items() if v.get().strip() != ""}
        for k, v in vals.items():
            if not valid_setting(k, v):
                messagebox.showerror("Settings", "%s: \"%s\" is not a valid value." % (k, v))
                return
        write_ini(P.ini, vals)
        with C.lock:
            C.d["debug_console"] = bool(dc.get())
            C.d["debug_flags"] = [f for f, v in dflags.items() if v.get()]
            C.save()
        log("settings saved to %s (used the next time the game starts)" % P.ini)

    def reset_settings():
        if not messagebox.askyesno("Settings", "Put every setting back to its default?"):
            return
        for k, sc in SCHEMA.items():
            svars.setdefault(k, tk.StringVar()).set(sc[5])

    def reload_settings():
        for k, v in read_ini(P.ini).items():
            svars.setdefault(k, tk.StringVar()).set(v)
        fill_all()

    sbtn = ttk.Frame(s)
    sbtn.pack(fill="x", pady=(8, 0))
    ttk.Button(sbtn, text="Save", command=save_settings).pack(side="left")
    ttk.Button(sbtn, text="Reset to defaults", command=reset_settings).pack(side="left", padx=6)
    ttk.Button(sbtn, text="Reload", command=reload_settings).pack(side="left")
    ttk.Label(sbtn, text="(edits %s)" % P.ini, style="Muted.TLabel").pack(side="left", padx=8)

    # ---- plumbing
    def refresh():
        if not busy.is_set() and adopt_disc(P, C):
            disc_var.set(C.get("disc", ""))
            log("found your game: %s" % C.get("disc"))
        first_bad = None
        for key, label, ok, tab in status(P, C):
            rows[key].configure(text=("✓  " if ok else "✗  ") + label, foreground=pal["ok"] if ok else pal["bad"])
            rows[key].bind("<Button-1>", lambda e, t=tab: nb.select(tabs[t]))
            if not ok:
                first_bad = first_bad or tab
        play_btn.configure(state="normal" if (ready_to_play(P, C) and not busy.is_set()) else "disabled")
        proj = "your own copy (mhrecomp)" if is_working_copy(P) else C.get("tag", "installed" if project_ok(P) else "not downloaded")
        ver.configure(text="launcher %s" % LAUNCHER_VERSION)
        info.configure(text="Project: %s\nBuild tools: %s\nGame folder: %s" % (
            proj, "installed" if toolchain_ok(P) else "not downloaded", P.game))
        d = disc_var.get().strip()
        if d:
            try:
                check_disc(C, d)
                disc_msg.configure(text="Game found: %s" % os.path.basename(resolve_image(d)), foreground=pal["ok"])
            except (Fail, OSError) as e:
                disc_msg.configure(text=str(e), foreground=pal["bad"])
        else:
            disc_msg.configure(text="No game image yet: put yours in the \"Mat Hoffman 2\" folder.", foreground=pal["muted"])
        return first_bad

    def work(fn, done_msg):
        if busy.is_set():
            return
        busy.set()
        for bt in buttons:
            bt.configure(state="disabled")
        bar.configure(mode="indeterminate")
        bar.start(12)

        def target():
            try:
                fn()
                q.put(("done", done_msg))
            except (Fail, OSError) as e:
                q.put(("error", str(e)))
            except Exception as e:      # keep the window alive on a bug
                q.put(("error", "unexpected error: %r" % e))
        threading.Thread(target=target, daemon=True).start()

    def progress(done, total):
        q.put(("progress", (done, total)))

    def do_download():
        def f():
            install_toolchain(P, log)
            install_project(P, C, log)
        work(f, "Download finished.")

    def do_build():
        C.set("disc", disc_var.get().strip())
        work(lambda: build_all(P, C, C.get("disc"), log, progress), "Build finished.")

    def do_play():
        try:
            play(P, C, log)
        except (Fail, OSError) as e:
            messagebox.showerror("Play", str(e))
            return
        if close_var.get():
            win.after(500, win.destroy)

    play_btn.configure(command=do_play)
    build_btn.configure(command=do_build)
    dl_btn.configure(command=do_download)
    up_btn.configure(command=lambda: work(lambda: install_project(P, C, log), "Project check finished."))
    ul_btn.configure(command=lambda: work(lambda: update_launcher(P, C, log), "Launcher check finished."))

    def check_updates():
        q.put(("latest", latest_tag(lambda s: None)))
    threading.Thread(target=check_updates, daemon=True).start()

    def pump():
        try:
            while True:
                kind, val = q.get_nowait()
                if kind == "log":
                    logbox.configure(state="normal")
                    logbox.insert("end", val + "\n")
                    logbox.see("end")
                    logbox.configure(state="disabled")
                elif kind == "progress":
                    bar.stop()
                    bar.configure(mode="determinate", maximum=max(1, val[1]), value=val[0])
                elif kind == "latest":
                    have = C.get("tag")
                    if val and have and version_tuple(val) and (version_tuple(have) or (0,)) < version_tuple(val):
                        upd_note.configure(text="A newer project version is available: %s (Update tab)." % val)
                elif kind in ("done", "error"):
                    busy.clear()
                    bar.stop()
                    bar.configure(mode="determinate", value=0)
                    for bt in buttons:
                        bt.configure(state="normal")
                    if kind == "error":
                        log("ERROR: " + val)
                        messagebox.showerror("Mat Hoffman 2 launcher", val)
                    else:
                        log(val)
                    refresh()
        except queue.Empty:
            pass
        win.after(100, pump)

    apply_theme(dark_var.get())
    win.bind("<FocusIn>", lambda e: refresh() if e.widget is win else None)   # picks up a game just dropped in the folder
    first = refresh()
    nb.select(tabs["Play"] if ready_to_play(P, C) else tabs[first or "Update"])
    pump()
    if notice:
        log("ERROR: " + notice)
        win.after(300, lambda: messagebox.showerror("Mat Hoffman 2 launcher", notice))
    win.mainloop()
    return 0


def main(argv):
    if sys.stdout is None:          # packaged without a console window
        sys.stdout = sys.stderr = open(os.devnull, "w")
    cmds = {"status", "download", "update", "build", "play", "update-launcher", "-h", "--help", "help"}
    if any(a in cmds for a in argv):
        return cli(argv)
    root = argv[argv.index("--root") + 1] if "--root" in argv and argv.index("--root") + 1 < len(argv) else None
    disc = argv[argv.index("--disc") + 1] if "--disc" in argv and argv.index("--disc") + 1 < len(argv) else None
    notice = None
    if disc:
        try:
            open_session(root, disc)
        except (Fail, OSError) as e:
            notice = str(e)
    return gui(root or default_root(), notice=notice)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
