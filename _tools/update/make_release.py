#!/usr/bin/env python3
"""Builds one platform's update for the in-game updater (ogsr_engine/xr_3da/xr_update.h) -- see README.md here.

    python3 make_release.py keygen
    python3 make_release.py build --platform nx --root /mnt/d/switch_stage/ogsr --self ogsr.nro \
        --tag nx-b3 --build 3 --version 0.2.0 --notes-file notes.txt --out /mnt/d/upd_out/nx-b3

The output folder holds manifest-<plat>.txt (signed) plus every asset this release adds, and the OTHER platforms'
current manifests carried over unchanged -- "releases/latest" must answer for all three platforms, whichever one was
released last. Upload the whole folder as one GitHub release (the command is printed at the end).

Runs in WSL (python3 + the `cryptography` package). The private key never leaves C:\\Users\\<you>\\.ogsr\\.
"""
import argparse
import fnmatch
import hashlib
import os
import shutil
import sys
import urllib.error
import urllib.request

PLATFORMS = ("nx", "android", "pc")
DEFAULT_REPO = "babasha/OGSR-2"
# Signing key: $OGSR_UPDATE_KEY, else ~/.ogsr/update_ed25519.pem (from WSL point it at /mnt/c/Users/<you>/.ogsr/...).
DEFAULT_KEY = os.environ.get("OGSR_UPDATE_KEY", os.path.expanduser("~/.ogsr/update_ed25519.pem"))
BIG_FILE = 4 << 20  # files from this size up are assets of their own
PACK_LIMIT = 64 << 20  # smaller files go into pack-N.bin assets of up to this size
MAX_ASSETS = 900  # GitHub allows 1000 per release

# Never shipped, whatever the stage folder holds: the player's own things and this machine's leftovers.
DEFAULT_EXCLUDES = [
    "_update/*", "*.log", "_appdata_/*", "appdata/*", "savedgames/*", "*/savedgames/*", "user.ltx", "*/user.ltx",
    "args.txt", "args2.txt", "nx_*.txt", "*.bak", "*.bak_*", "vk_pipeline_cache.bin", "*.gcda", "stdout.log",
    "stderr.log", "*.tmp", "Thumbs.db", ".DS_Store",
]


def blake(path_or_bytes):
    h = hashlib.blake2b(digest_size=32)
    if isinstance(path_or_bytes, (bytes, bytearray)):
        h.update(path_or_bytes)
    else:
        with open(path_or_bytes, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
    return h.hexdigest()


def safe_path(p):
    """The same rule the game applies (xr_update.cpp safe_path): relative, '/'-separated, printable ASCII."""
    if p == "@self":
        return True
    if not p or len(p) > 400 or p.startswith("/") or p.startswith("_update/"):
        return False
    if any(ord(c) < 0x20 or ord(c) > 0x7E or c in "\\:" for c in p):
        return False
    return all(seg not in ("", ".", "..") for seg in p.split("/"))


# ------------------------------------------------------------------ keys ----

def load_key(path):
    from cryptography.hazmat.primitives import serialization
    with open(path, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def pub_bytes(key):
    from cryptography.hazmat.primitives import serialization
    return key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def cmd_keygen(a):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
    if os.path.exists(a.key):
        key = load_key(a.key)
        print(f"key exists: {a.key} (not overwritten)")
    else:
        os.makedirs(os.path.dirname(a.key), exist_ok=True)
        key = Ed25519PrivateKey.generate()
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
        with open(a.key, "wb") as f:
            f.write(pem)
        print(f"new key: {a.key}  -- BACK IT UP. Lose it and no build can ever be updated again.")
    pub = pub_bytes(key)
    print("public key for xr_update.cpp (kPublicKey):")
    for i in range(0, 32, 8):
        print("    " + ", ".join(f"0x{b:02x}" for b in pub[i:i + 8]) + ",")


# -------------------------------------------------------------- manifest ----

class Manifest:
    def __init__(self):
        self.platform = ""
        self.build = 0
        self.version = ""
        self.base = ""
        self.notes = []
        self.assets = {}  # src -> (hash, size)
        self.files = {}  # path -> (hash, size, src, off)
        self.apk = None  # (code, src)


def parse_manifest(text):
    m = Manifest()
    for line in text.split("\n"):
        if not line:
            continue
        k, _, rest = line.partition(" ")
        if k == "platform":
            m.platform = rest
        elif k == "build":
            m.build = int(rest)
        elif k == "version":
            m.version = rest
        elif k == "base":
            m.base = rest
        elif k == "notes":
            m.notes.append(rest)
        elif k == "a":
            h, size, src = rest.split(" ", 2)
            m.assets[src] = (h, int(size))
        elif k == "f":
            h, size, src, off, path = rest.split(" ", 4)
            m.files[path] = (h, int(size), src, int(off))
        elif k == "apk":
            code, src = rest.split(" ", 1)
            m.apk = (int(code), src)
    return m


def fetch_latest(repo, plat):
    url = f"https://github.com/{repo}/releases/latest/download/manifest-{plat}.txt"
    try:
        with urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "ogsr-release-tool"}), timeout=30) as r:
            return r.read()
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def sign_manifest(body, key):
    data = body.encode("utf-8")
    sig = key.sign(data)
    return data + b"sig " + sig.hex().encode() + b"\n"


# ----------------------------------------------------------------- build ----

def collect(root, excludes, self_name):
    out = {}
    for dp, dn, fn in os.walk(root):
        dn.sort()
        for name in sorted(fn):
            full = os.path.join(dp, name)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            if any(fnmatch.fnmatch(rel, pat) for pat in excludes):
                continue
            if self_name and rel == self_name:
                rel = "@self"
            if not safe_path(rel):
                sys.exit(f"path the game would refuse (non-ASCII, ':' or '\\'): {rel}")
            out[rel] = full
    return out


def cmd_build(a):
    if a.platform not in PLATFORMS:
        sys.exit(f"--platform: one of {PLATFORMS}")
    if a.self and a.platform != "nx":
        sys.exit("--self (the running NRO) is a Switch thing")
    key = load_key(a.key)
    excludes = DEFAULT_EXCLUDES + (a.exclude or [])
    base = a.base or f"https://github.com/{a.repo}/releases/download"

    prev = None
    if a.prev:
        prev = parse_manifest(open(a.prev, encoding="utf-8").read())
    elif not a.no_prev:
        raw = fetch_latest(a.repo, a.platform)
        prev = parse_manifest(raw.decode("utf-8")) if raw else None
    if prev:
        print(f"previous {a.platform}: build {prev.build} ({len(prev.files)} files)")
        if a.build <= prev.build:
            sys.exit(f"--build must be above {prev.build}")
    else:
        print(f"no previous {a.platform} manifest: everything is new")

    files = collect(a.root, excludes, a.self)
    if not files:
        sys.exit("nothing to ship under --root")
    total = sum(os.path.getsize(p) for p in files.values())
    print(f"{len(files)} files, {total / 1048576:.1f} MB under {a.root}")
    tops = {}
    for rel, full in files.items():
        t = rel.split("/")[0]
        tops[t] = tops.get(t, 0) + os.path.getsize(full)
    for t, s in sorted(tops.items(), key=lambda x: -x[1])[:15]:
        print(f"    {s / 1048576:9.1f} MB  {t}")

    os.makedirs(a.out, exist_ok=True)
    m_files = {}
    m_assets = {}
    new_by_hash = {}  # hash -> (src, off): a file this release already carries
    small = []  # (rel, full, hash, size) for packs
    reused = changed = 0
    for rel, full in sorted(files.items()):
        h = blake(full)
        size = os.path.getsize(full)
        if prev and rel in prev.files and prev.files[rel][0] == h:
            _, _, src, off = prev.files[rel]
            m_files[rel] = (h, size, src, off)
            m_assets[src] = prev.assets[src]
            reused += 1
            continue
        changed += 1
        if h in new_by_hash:
            src, off = new_by_hash[h]
            m_files[rel] = (h, size, src, off)
            continue
        if size >= BIG_FILE:
            name = f"{h[:16]}-{os.path.basename(full)}"
            name = "".join(c if c.isalnum() or c in "._-" else "_" for c in name)
            dst = os.path.join(a.out, name)
            if not os.path.exists(dst):
                shutil.copyfile(full, dst)
            src = f"{a.tag}/{name}"
            m_files[rel] = (h, size, src, 0)
            m_assets[src] = (h, size)
            new_by_hash[h] = (src, 0)
        else:
            small.append((rel, full, h, size))

    pack_n, pack_f, pack_off, pack_name = 0, None, 0, None

    def close_pack():
        nonlocal pack_f
        if pack_f:
            pack_f.close()
            p = os.path.join(a.out, pack_name)
            m_assets[f"{a.tag}/{pack_name}"] = (blake(p), os.path.getsize(p))
            pack_f = None

    for rel, full, h, size in small:
        if h in new_by_hash:
            src, off = new_by_hash[h]
            m_files[rel] = (h, size, src, off)
            continue
        if pack_f is None or pack_off + size > PACK_LIMIT:
            close_pack()
            pack_n += 1
            pack_name = f"pack-{pack_n}.bin"
            pack_f = open(os.path.join(a.out, pack_name), "wb")
            pack_off = 0
        with open(full, "rb") as f:
            pack_f.write(f.read())
        src = f"{a.tag}/{pack_name}"
        m_files[rel] = (h, size, src, pack_off)
        new_by_hash[h] = (src, pack_off)
        pack_off += size
    close_pack()

    apk_line = None
    if a.apk:
        if a.platform != "android":
            sys.exit("--apk is for --platform android")
        h = blake(a.apk)
        if prev and prev.apk and prev.assets.get(prev.apk[1], ("",))[0] == h:
            src = prev.apk[1]
            m_assets[src] = prev.assets[src]
        else:
            name = f"ogsr-{a.apk_code}.apk"
            shutil.copyfile(a.apk, os.path.join(a.out, name))
            src = f"{a.tag}/{name}"
            m_assets[src] = (h, os.path.getsize(a.apk))
        apk_line = f"apk {a.apk_code} {src}"
    elif prev and prev.apk:
        src = prev.apk[1]
        m_assets[src] = prev.assets[src]
        apk_line = f"apk {prev.apk[0]} {src}"

    new_assets = [s for s in m_assets if s.startswith(a.tag + "/")]
    if len(new_assets) > MAX_ASSETS:
        sys.exit(f"{len(new_assets)} assets: more than one release can hold")

    notes = []
    if a.notes_file:
        notes = [l.rstrip("\r\n") for l in open(a.notes_file, encoding="utf-8")]
    lines = ["ogsr-update 1", f"platform {a.platform}", f"build {a.build}", f"version {a.version}", f"base {base}"]
    lines += [f"notes {n}" for n in notes]
    if apk_line:
        lines.append(apk_line)
    lines += [f"a {h} {s} {src}" for src, (h, s) in sorted(m_assets.items())]
    lines += [f"f {h} {s} {src} {off} {rel}" for rel, (h, s, src, off) in sorted(m_files.items())]
    body = "\n".join(lines) + "\n"
    with open(os.path.join(a.out, f"manifest-{a.platform}.txt"), "wb") as f:
        f.write(sign_manifest(body, key))

    removed = [p for p in prev.files if p not in m_files] if prev else []
    new_bytes = sum(m_assets[s][1] for s in new_assets)
    print(f"files: {reused} unchanged, {changed} new or changed, {len(removed)} removed")
    print(f"this release uploads {len(new_assets)} assets, {new_bytes / 1048576:.1f} MB")

    for other in PLATFORMS:
        if other == a.platform:
            continue
        raw = fetch_latest(a.repo, other)
        if raw:
            with open(os.path.join(a.out, f"manifest-{other}.txt"), "wb") as f:
                f.write(raw)
            print(f"carried over manifest-{other}.txt")

    print("\nupload (Windows terminal):")
    print(f'  gh release create {a.tag} --repo {a.repo} --title "{a.version} ({a.platform} build {a.build})" '
          f'--notes-file <notes> "<out folder>\\*"')
    print("Never delete an older release: newer manifests keep pointing at the assets of the ones before.")


def cmd_verify(a):
    """Checks a manifest's signature the way the game does (handy before uploading)."""
    raw = open(a.manifest, "rb").read()
    i = raw.rfind(b"\nsig ")
    if i < 0:
        sys.exit("no sig line")
    data, sig = raw[:i + 1], bytes.fromhex(raw[i + 5:].strip().decode())
    key = load_key(a.key)
    key.public_key().verify(sig, data)
    m = parse_manifest(data.decode("utf-8"))
    print(f"OK: {m.platform} build {m.build} {m.version}, {len(m.files)} files, {len(m.assets)} assets")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--key", default=DEFAULT_KEY)
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("keygen")
    b = sub.add_parser("build")
    b.add_argument("--platform", required=True)
    b.add_argument("--root", required=True, help="the folder laid out as on the device (next to fsgame.ltx)")
    b.add_argument("--self", help="nx: the NRO's name under --root (shipped as @self, whatever the player named it)")
    b.add_argument("--tag", required=True, help="the GitHub release tag this goes out as, e.g. nx-b3")
    b.add_argument("--build", type=int, required=True, help="increasing per platform")
    b.add_argument("--version", required=True)
    b.add_argument("--notes-file")
    b.add_argument("--repo", default=DEFAULT_REPO)
    b.add_argument("--base", help="asset URL prefix instead of the repo's releases/download (a local test server)")
    b.add_argument("--prev", help="previous manifest file (default: the latest release's)")
    b.add_argument("--no-prev", action="store_true", help="first release: do not look for one")
    b.add_argument("--exclude", action="append", help="extra fnmatch pattern on the relative path")
    b.add_argument("--apk", help="android: the APK")
    b.add_argument("--apk-code", type=int, help="android: its versionCode (AndroidManifest.xml)")
    b.add_argument("--out", required=True)
    v = sub.add_parser("verify")
    v.add_argument("manifest")
    a = ap.parse_args()
    if a.cmd == "build" and a.apk and not a.apk_code:
        sys.exit("--apk needs --apk-code")
    {"keygen": cmd_keygen, "build": cmd_build, "verify": cmd_verify}[a.cmd](a)


if __name__ == "__main__":
    main()
