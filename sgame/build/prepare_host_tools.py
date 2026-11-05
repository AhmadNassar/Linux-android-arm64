#!/usr/bin/env python3
"""Provide Kleaf's host rsync in a user directory on Ubuntu build servers."""
import hashlib
import json
import os
import platform
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def prepare(work):
    if shutil.which("rsync"):
        return
    release = platform.freedesktop_os_release()
    if release["ID"] != "ubuntu":
        raise SystemExit("Kleaf needs rsync; install the native host package and retry")
    suite = release["VERSION_CODENAME"]
    arch = subprocess.check_output(["dpkg", "--print-architecture"], text=True).strip()
    if arch != "amd64":
        raise SystemExit("the Android prebuilt build tools require an amd64 host")
    host = work / "host-tools"
    apt = host / "apt"
    for folder in (apt / "lists/partial", apt / "cache/archives/partial", apt / "empty", host / "debs"):
        folder.mkdir(parents=True, exist_ok=True)
    mirror = os.environ.get("UBUNTU_MIRROR", "https://mirrors.ustc.edu.cn/ubuntu").rstrip("/")
    sources = apt / "sources.list"
    sources.write_text("".join(
        f"deb [arch=amd64 signed-by=/usr/share/keyrings/ubuntu-archive-keyring.gpg] {mirror} {name} main\n"
        for name in (suite, suite + "-updates", suite + "-security")))
    options = ["-o", "Dir::State::lists=" + str(apt / "lists"),
               "-o", "Dir::Cache=" + str(apt / "cache"),
               "-o", "Dir::Etc::sourcelist=" + str(sources),
               "-o", "Dir::Etc::sourceparts=" + str(apt / "empty"),
               "-o", "Dir::Etc::parts=" + str(apt / "empty"),
               "-o", "APT::Get::List-Cleanup=0"]
    config = apt / "config"
    config.write_text(f'Dir::Etc::parts {json.dumps(str(apt / "empty"))};\nDir::Etc::main "/dev/null";\n')
    env = dict(os.environ, APT_CONFIG=str(config))
    subprocess.run(["apt-get", *options, "update"], env=env, check=True)
    extracted = host / "root"
    extracted.mkdir(exist_ok=True)
    records = []
    with tempfile.TemporaryDirectory(prefix="download-", dir=host) as temporary:
        subprocess.run(["apt-get", *options, "download", "rsync", "libpopt0"],
                       cwd=temporary, env=env, check=True)
        for package in sorted(Path(temporary).glob("*.deb")):
            subprocess.run(["dpkg-deb", "-x", str(package), str(extracted)], check=True)
            records.append({"file": package.name,
                            "sha256": hashlib.sha256(package.read_bytes()).hexdigest(),
                            "control": subprocess.check_output([
                                "dpkg-deb", "-f", str(package), "Package", "Version", "Architecture"], text=True)})
            shutil.copy2(package, host / "debs" / package.name)
    tools = work / "tools"
    tools.mkdir(exist_ok=True)
    wrapper = tools / "rsync"
    library = shlex.quote(str(extracted / "usr/lib/x86_64-linux-gnu"))
    binary = shlex.quote(str(extracted / "usr/bin/rsync"))
    wrapper.write_text(f'#!/bin/sh\nLD_LIBRARY_PATH={library} exec {binary} "$@"\n')
    wrapper.chmod(0o755)
    print(subprocess.check_output([str(wrapper), "--version"], text=True).splitlines()[0], flush=True)
    (host / "packages.json").write_text(json.dumps(records, indent=2) + "\n")


if __name__ == "__main__":
    prepare(Path(sys.argv[1]).resolve())
