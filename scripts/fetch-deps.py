#!/usr/bin/env python3
"""Fetch pinned open-source dependencies. Never downloads synthesizer firmware."""
import hashlib
from pathlib import Path
import subprocess
import tarfile
import urllib.request

ROOT = Path(__file__).resolve().parent.parent
EXT = ROOT / "external"
DYNARMIC = "a46601580d5512d324104f985b5f0209dc980ddc"
JUCE = "91ad83ae34a81e0833b1a2b0866f54846370ae53"  # 8.0.15
BOOST_SHA = "1bed88e40401b2cb7a1f76d4bab499e352fa4d0c5f31c0dbae64e24d34d7513b"

def git(path, *args, **kwargs):
    return subprocess.run(["git", "-C", str(path), *args], check=True, **kwargs)

def checkout(name, url, revision):
    path = EXT / name
    if not path.exists():
        path.mkdir(parents=True)
        git(path, "init", "-q")
        git(path, "remote", "add", "origin", url)
    head = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], capture_output=True, text=True)
    if head.returncode:
        git(path, "fetch", "--depth", "1", "origin", revision)
        git(path, "checkout", "--detach", "FETCH_HEAD")
    elif head.stdout.strip() != revision:
        raise SystemExit(f"{path}: unexpected revision; move this directory aside before fetching (local work preserved)")
    return path

def patch(path, name):
    file = ROOT / "patches" / name
    reverse = subprocess.run(["git", "-C", str(path), "apply", "--reverse", "--check", str(file)], capture_output=True)
    if reverse.returncode:
        git(path, "apply", "--check", str(file))
        git(path, "apply", str(file))

EXT.mkdir(exist_ok=True)
dyn = checkout("dynarmic", "https://github.com/azahar-emu/dynarmic.git", DYNARMIC)
git(dyn, "submodule", "update", "--init", "--depth", "1", *["externals/" + x for x in ("fmt", "mcl", "robin-map", "xbyak", "zycore", "zydis")])
git(dyn / "externals/zydis", "submodule", "update", "--init", "--depth", "1")
patch(dyn, "dynarmic-system-instructions.patch")
patch(dyn, "dynarmic-vector-include.patch")
patch(dyn / "externals/fmt", "fmt-cstdlib.patch")
juce = checkout("JUCE", "https://github.com/juce-framework/JUCE.git", JUCE)
patch(juce, "juce-llvm-mingw.patch")
boost = EXT / "boost"
if not (boost / "boost/version.hpp").exists():
    archive = EXT / "boost_1_86_0.tar.bz2"
    if not archive.exists():
        urllib.request.urlretrieve("https://archives.boost.io/release/1.86.0/source/boost_1_86_0.tar.bz2", archive)
    if hashlib.file_digest(archive.open("rb"), "sha256").hexdigest() != BOOST_SHA:
        raise SystemExit("Boost checksum mismatch; remove the archive and retry")
    with tarfile.open(archive) as tf:
        tf.extractall(EXT, filter="data")
    (EXT / "boost_1_86_0").rename(boost)
print("Dependencies ready (Dynarmic pinned; JUCE 8.0.15; Boost 1.86.0).")
