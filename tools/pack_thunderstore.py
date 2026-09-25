"""A mod's Thunderstore package: dist/<team>-<Name>-<version>.zip, holding
manifest.json (made from the mod's mod.toml), README.md, CHANGELOG.md and
icon.png from mods/<mod>/thunderstore/, and the built .nrm, all at the zip's
root, the layout Thunderstore's upload page checks and the one Snap64 Recomp
1.1.0 unpacks when the zip is dropped on its window.

    python tools/pack_thunderstore.py unlimited_film [unlock_everything ...]
        [--team JackandBeans] [--build build] [--out dist]

Needs Python 3.11 (tomllib); nothing else.
"""
import argparse
import json
import os
import re
import struct
import sys
import tomllib
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEBSITE = "https://github.com/JackandBeans/Snap64RecompMods"


def fail(message):
    sys.exit("pack_thunderstore: " + message)


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        fail(f"{path} is not a PNG")
    return struct.unpack(">II", head[16:24])


def pack(mod, team, build, out):
    toml_path = os.path.join(ROOT, "mods", mod, "mod.toml")
    with open(toml_path, "rb") as f:
        spec = tomllib.load(f)
    manifest = spec["manifest"]
    name = re.sub(r"[^A-Za-z0-9_]", "", manifest["display_name"].replace(" ", "_"))
    version = manifest["version"]
    description = manifest["short_description"]
    if not re.fullmatch(r"\d+\.\d+\.\d+", version):
        fail(f"{mod}: version {version!r} is not major.minor.patch")
    if len(description) > 250:
        fail(f"{mod}: the short description is {len(description)} characters; Thunderstore takes 250")

    here = os.path.join(ROOT, "mods", mod, "thunderstore")
    icon = os.path.join(here, "icon.png")
    if png_size(icon) != (256, 256):
        fail(f"{icon} is {png_size(icon)}; Thunderstore wants 256x256")
    nrm = os.path.join(ROOT, build, mod, spec["inputs"]["mod_filename"] + ".nrm")
    if not os.path.isfile(nrm):
        fail(f"{nrm} is missing; build the mod first")

    ts_manifest = {
        "name": name,
        "version_number": version,
        "website_url": WEBSITE,
        "description": description,
        "dependencies": [],
    }
    os.makedirs(os.path.join(ROOT, out), exist_ok=True)
    zip_path = os.path.join(ROOT, out, f"{team}-{name}-{version}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("manifest.json", json.dumps(ts_manifest, indent=4, ensure_ascii=False) + "\n")
        for extra in ("README.md", "CHANGELOG.md", "icon.png"):
            path = os.path.join(here, extra)
            if os.path.isfile(path):
                z.write(path, extra)
            elif extra != "CHANGELOG.md":
                fail(f"{path} is missing")
        z.write(nrm, os.path.basename(nrm))
    print(f"{os.path.relpath(zip_path, ROOT)}: {name} {version}, {os.path.getsize(zip_path)} bytes")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("mods", nargs="+")
    parser.add_argument("--team", default="JackandBeans")
    parser.add_argument("--build", default="build")
    parser.add_argument("--out", default="dist")
    args = parser.parse_args()
    for mod in args.mods:
        pack(mod, args.team, args.build, args.out)


if __name__ == "__main__":
    main()
