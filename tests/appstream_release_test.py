#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""tools/appstream-release.py against copies of the real metainfo file:
what release.sh relies on when it records a release before tagging."""

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TOOL = ROOT / "tools" / "appstream-release.py"
METAINFO = ROOT / "src" / "gui" / "org.kde.seabass.metainfo.xml"
APPSTREAMCLI = os.environ.get("SEABASS_APPSTREAMCLI") or shutil.which("appstreamcli")


class AppstreamReleaseTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.file = Path(self.dir.name) / "org.kde.seabass.metainfo.xml"
        shutil.copy(METAINFO, self.file)

    def tearDown(self):
        self.dir.cleanup()

    def run_tool(self, version, channel, date):
        subprocess.run([sys.executable, str(TOOL), str(self.file), version, channel, "--date", date], check=True)
        return self.file.read_text(encoding="utf-8")

    def versions(self, text):
        import re
        return re.findall(r'<release\s+version="([^"]+)"', text)

    def test_a_new_release_goes_first(self):
        text = self.run_tool("9.9.9", "stable", "2030-01-02")
        self.assertEqual(self.versions(text)[0], "9.9.9")
        self.assertIn('<release version="9.9.9" type="stable" date="2030-01-02"/>', text)
        # Everything that was there before is still there, after it.
        before = self.versions(METAINFO.read_text(encoding="utf-8"))
        self.assertEqual(self.versions(text)[1:], before)

    def test_a_pre_release_is_a_development_release(self):
        text = self.run_tool("9.9.10", "beta", "2030-01-02")
        self.assertIn('<release version="9.9.10" type="development" date="2030-01-02"/>', text)

    def test_an_existing_entry_keeps_its_description_and_takes_the_new_date(self):
        # Its own entry, written into the copy first: the real file's first
        # entry changes with every release (0.7.11's was recorded without a
        # description), and a test that read it broke on the release commit.
        text = self.file.read_text(encoding="utf-8")
        entry = ('    <release version="9.8.7" type="stable" date="2029-01-01">\n'
                 "      <description>\n        <p>Kept.</p>\n      </description>\n"
                 "    </release>\n")
        text = text.replace("<releases>\n", "<releases>\n" + entry, 1)
        self.file.write_text(text, encoding="utf-8")
        text = self.run_tool("9.8.7", "alpha", "2031-05-06")
        self.assertEqual(self.versions(text).count("9.8.7"), 1, "one entry per version")
        self.assertIn('<release version="9.8.7" type="development" date="2031-05-06">', text)
        kept = text.split('version="9.8.7"', 1)[1].split("</release>", 1)[0]
        self.assertIn("<p>Kept.</p>", kept)

    def test_running_it_twice_changes_nothing_the_second_time(self):
        once = self.run_tool("9.9.9", "stable", "2030-01-02")
        twice = self.run_tool("9.9.9", "stable", "2030-01-02")
        self.assertEqual(once, twice)

    def test_the_header_comment_survives(self):
        text = self.run_tool("9.9.9", "stable", "2030-01-02")
        # REUSE-IgnoreStart: a string about a header, not a header of this file
        self.assertIn("SPDX-License-Identifier: CC0-1.0", text)
        # REUSE-IgnoreEnd

    def test_a_bad_version_is_refused(self):
        result = subprocess.run([sys.executable, str(TOOL), str(self.file), "9.9", "stable"], capture_output=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.file.read_text(encoding="utf-8"), METAINFO.read_text(encoding="utf-8"))

    @unittest.skipIf(APPSTREAMCLI is None, "appstreamcli not installed")
    def test_the_result_still_validates(self):
        self.run_tool("9.9.9", "stable", "2030-01-02")
        self.run_tool("9.9.10", "alpha", "2030-02-03")
        result = subprocess.run([APPSTREAMCLI, "validate", "--pedantic", "--no-net", str(self.file)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
