#!/usr/bin/env python3
# KickEQ - release manifest generator
# Copyright (C) 2026 Safian Szabolcs
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Fills the `builds` array of the plugin manifest from the packaged release archives.

Archives are named KickEQ-<platform>.tar.gz (see scripts/package.sh); each becomes one entry
{platform, url, sha256, size} pointing at the GitHub release asset.
"""
import argparse
import hashlib
import json
import pathlib
import re


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", required=True)
    ap.add_argument("--repo", required=True, help="owner/name")
    ap.add_argument("--tag", required=True)
    ap.add_argument("--dist", required=True)
    ap.add_argument("--template", required=True, help="manifest/kickeq.manifest.json.in")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    text = pathlib.Path(args.template).read_text()
    text = text.replace("@PROJECT_VERSION@", args.version)
    text = re.sub(r"@KICKEQ_LICENSE_SPDX@", "AGPL-3.0-or-later", text)
    text = re.sub(r"@KICKEQ_MIN_HOST_VERSION@", "1.0.0", text)
    manifest = json.loads(text)

    builds = []
    for archive in sorted(pathlib.Path(args.dist).glob("KickEQ-*.tar.gz")):
        platform = archive.name[len("KickEQ-"):-len(".tar.gz")]
        data = archive.read_bytes()
        builds.append({
            "platform": platform,
            "url": f"https://github.com/{args.repo}/releases/download/{args.tag}/{archive.name}",
            "sha256": hashlib.sha256(data).hexdigest(),
            "size": len(data),
        })
    manifest["builds"] = builds
    pathlib.Path(args.out).write_text(json.dumps(manifest, indent=2) + "\n")


if __name__ == "__main__":
    main()
