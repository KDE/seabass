#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

"""Two questions .gitlab-ci.yml asks Invent about earlier pipelines.

    tools/ci-pipelines.py nightly-needed [--sha SHA] [--job NAME]
    tools/ci-pipelines.py tag-tested [--sha SHA] [--wait-minutes N]

nightly-needed: exit 0 when the nightly has work to do, 3 when master is
still at the commit the last successful nightly already tested, so the
job can end before it installs or builds anything. "The last successful
nightly" is the newest successful scheduled pipeline on master whose
nightly job (linux:nightly) passed: the weekly schedule is a scheduled
pipeline too, and it does not run that job. No earlier nightly, or an
answer from Invent that cannot be read, means the nightly runs: a night
spent on a commit already tested costs an hour, a night skipped on one
that was not costs the one check that lane exists for.

tag-tested: exit 0 when a branch pipeline at this commit passed with
linux:build-test in it, 1 otherwise. A release package is built from a
tested commit or not at all. Pipelines at the commit still running are
waited for (a tag is often pushed with, or just after, its branch);
one that passed without linux:build-test (a push that changed only
documentation skips it) does not count. A linux:qt68 that ran and failed
in a passing pipeline (it is a button there, allowed to fail) fails this
too: that is the release check for the Qt the Mac ships on.

Only the public API is used, so no token is needed: multimedia/seabass is
a public project with public pipelines.
"""

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

API = os.environ.get("CI_API_V4_URL", "https://invent.kde.org/api/v4")
PROJECT = os.environ.get("CI_PROJECT_ID") or urllib.parse.quote("multimedia/seabass", safe="")

UNFINISHED = {"created", "waiting_for_resource", "preparing", "pending", "running", "scheduled"}


def get(path, **params):
    url = f"{API}/projects/{PROJECT}/{path}"
    if params:
        url += "?" + urllib.parse.urlencode(params)
    request = urllib.request.Request(url, headers={"User-Agent": "seabass-ci"})
    with urllib.request.urlopen(request, timeout=60) as response:
        return json.load(response)


def jobs(pipeline):
    """The latest attempt of every job in `pipeline`, by name."""
    found = {}
    page = 1
    while True:
        batch = get(f"pipelines/{pipeline['id']}/jobs", per_page=100, page=page)
        for job in batch:
            found[job["name"]] = job["status"]
        if len(batch) < 100:
            return found
        page += 1


def nightly_needed(sha, job):
    try:
        pipelines = get("pipelines", ref="master", source="schedule", status="success", per_page=30)
        for pipeline in pipelines:
            if jobs(pipeline).get(job) == "success":
                if pipeline["sha"] == sha:
                    print(f"master is still at {sha[:12]}, which nightly {pipeline['web_url']} "
                          f"already passed: nothing to test")
                    return 3
                print(f"last successful nightly {pipeline['web_url']} tested {pipeline['sha'][:12]}, "
                      f"master is at {sha[:12]}: running")
                return 0
        print(f"no successful nightly ({job}) among the last {len(pipelines)} scheduled pipelines on master: running")
        return 0
    except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
        print(f"could not ask Invent about earlier nightlies ({error}): running")
        return 0


def tag_tested(sha, wait_minutes):
    deadline = time.monotonic() + wait_minutes * 60
    while True:
        try:
            pipelines = get("pipelines", sha=sha, scope="branches", per_page=100)
            for pipeline in pipelines:
                if pipeline["status"] == "success":
                    pipeline["jobs"] = jobs(pipeline)
        except (urllib.error.URLError, OSError, ValueError, KeyError) as error:
            pipelines = None
            print(f"could not ask Invent ({error}); asking again")
        if pipelines is not None:
            waiting = []
            for pipeline in pipelines:
                line = f"{pipeline['web_url']} ({pipeline['ref']}, {pipeline['source']}): {pipeline['status']}"
                if pipeline["status"] in UNFINISHED:
                    waiting.append(line)
                    continue
                if pipeline["status"] != "success":
                    print(line)
                    continue
                found = pipeline["jobs"]
                if found.get("linux:qt68") == "failed":
                    print(f"{line}, but its linux:qt68 release check FAILED: not a tested commit")
                    return 1
                if found.get("linux:build-test") == "success":
                    print(f"{line}, linux:build-test passed: {sha[:12]} is a tested commit")
                    return 0
                print(f"{line}, without a passing linux:build-test")
            if not waiting:
                print(f"no branch pipeline at {sha[:12]} passed with linux:build-test in it.")
                print("Push the commit to its branch (or run a pipeline on the branch from Invent's")
                print("Build -> Pipelines -> Run pipeline), wait for it to pass, then retry this job.")
                return 1
            for line in waiting:
                print(f"waiting for {line}")
        if time.monotonic() > deadline:
            print(f"gave up after {wait_minutes} minutes; retry this job once the branch pipeline has passed")
            return 1
        sys.stdout.flush()
        time.sleep(60)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("question", choices=["nightly-needed", "tag-tested"])
    parser.add_argument("--sha", default=os.environ.get("CI_COMMIT_SHA", ""))
    parser.add_argument("--job", default="linux:nightly", help="the nightly's job name (nightly-needed)")
    parser.add_argument("--wait-minutes", type=int, default=150, help="how long to wait for a running pipeline (tag-tested)")
    args = parser.parse_args()
    if len(args.sha) != 40:
        print(f"need the full 40-character commit sha, got '{args.sha}'", file=sys.stderr)
        return 1 if args.question == "tag-tested" else 0
    if args.question == "nightly-needed":
        return nightly_needed(args.sha, args.job)
    return tag_tested(args.sha, args.wait_minutes)


if __name__ == "__main__":
    sys.exit(main())
