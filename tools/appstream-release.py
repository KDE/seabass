#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Records a release in the AppStream metadata, newest first.

    tools/appstream-release.py <metainfo.xml> <X.Y.Z> <alpha|beta|stable> [--date YYYY-MM-DD]

tools/release.sh runs this just before it tags, so the tagged tree lists
its own release and software centres show the history without anybody
having to remember it. An alpha or beta is a "development" release in
AppStream's terms, a stable one "stable". An entry that exists already
for the version keeps its description and has its type and date brought
up to date; running it twice changes nothing the second time.

The file is edited as text, not parsed and written back: an XML library
would drop the SPDX comment and reflow everything else, and the diff of a
release commit should be the one line it is about.
"""

import argparse
import datetime
import re
import sys
from pathlib import Path


def release_type(channel: str) -> str:
    return "stable" if channel == "stable" else "development"


def record(text: str, version: str, channel: str, date: str) -> str:
    kind = release_type(channel)
    existing = re.compile(
        r'[ \t]*<release\s+version="' + re.escape(version) + r'"[^>]*?(?:/>|>(?P<body>.*?)</release>)[ \t]*\n',
        re.DOTALL,
    )
    body = None
    match = existing.search(text)
    if match:
        body = match.group("body")
        text = text[: match.start()] + text[match.end() :]

    opening = re.search(r'^(?P<indent>[ \t]*)<releases>[ \t]*\n', text, re.MULTILINE)
    if opening is None:
        closing = re.search(r'^(?P<indent>[ \t]*)</component>', text, re.MULTILINE)
        if closing is None:
            raise ValueError("no <component> element to put <releases> into")
        indent = closing.group("indent") + "  "
        text = text[: closing.start()] + f"{indent}<releases>\n{indent}</releases>\n" + text[closing.start() :]
        opening = re.search(r'^(?P<indent>[ \t]*)<releases>[ \t]*\n', text, re.MULTILINE)

    indent = opening.group("indent") + "  "
    attributes = f'version="{version}" type="{kind}" date="{date}"'
    if body is not None and body.strip():
        entry = f"{indent}<release {attributes}>{body}</release>\n"
    else:
        entry = f"{indent}<release {attributes}/>\n"
    return text[: opening.end()] + entry + text[opening.end() :]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("metainfo", type=Path)
    parser.add_argument("version")
    parser.add_argument("channel", choices=["alpha", "beta", "stable"])
    parser.add_argument("--date", default=datetime.date.today().isoformat())
    args = parser.parse_args()
    if not re.fullmatch(r"\d+\.\d+\.\d+", args.version):
        print(f"{args.version} is not X.Y.Z", file=sys.stderr)
        return 2
    if not re.fullmatch(r"\d{4}-\d{2}-\d{2}", args.date):
        print(f"{args.date} is not YYYY-MM-DD", file=sys.stderr)
        return 2
    before = args.metainfo.read_text(encoding="utf-8")
    after = record(before, args.version, args.channel, args.date)
    if after != before:
        args.metainfo.write_text(after, encoding="utf-8")
    return 0


if __name__ == "__main__":
    sys.exit(main())
