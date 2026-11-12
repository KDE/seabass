<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# CI

`.gitlab-ci.yml` runs on KDE Invent. The Linux jobs build and TEST; KDE's
Craft templates and `macos:universal` build and PACKAGE (see "The Craft
lanes" below). No packaging job runs ctest, so the Linux jobs are the only
thing on Invent that does.

## KDE's standard jobs

KDE's sysadmins ask projects to build with the job templates in
`sysadmin/ci-utilities` and nothing else (2026-11-12). A job that is not
one of those templates is a custom job, and a custom job runs in a Docker
slot, which has far less behind it than the VM runners the templates use.

Three are included, and run on every push, merge request and tag:

| Job | What |
|---|---|
| `suse_tumbleweed_qt612` | Build and test, openSUSE Tumbleweed, Qt 6.12 |
| `windows_qt612` | Build and test, MSVC 2022, Qt 6.12 |
| `reuse` | REUSE compliance |

The two build jobs are configured by `.kde-ci.yml`, not by
`.gitlab-ci.yml`: a Release build with ccache, `ctest -j` over the whole
suite as registered by default (so without the storm), and a failing
test fails the job on Linux. Two things are left out there, each with
its reason beside it in `.kde-ci.yml`: `sleep_inhibitor_test`, which the
CI machines' logind refuses, and the `unzip` and `7z` cases of the backup
archive cross-validation on an image without those tools. On Windows a failing test is reported and
the job stays green, until the suite has passed there once.

The custom Linux lanes in the table below are still there. They go once
the standard jobs have run green beside them.

## What runs when

| Pipeline | Jobs |
|---|---|
| Push to any branch, merge request | `linux:build-test` (configured with `-DSEABASS_QML_TESTS=OFF`; `ctest -j --timeout 300 -LE "qml\|storm\|integration\|timing"`), SAST and secret detection. A push that touches only documentation (the paths outside `.code_paths` in `.gitlab-ci.yml`) is not built. |
| Nightly schedule, master | `linux:nightly`: full build, every test but the storm (`timing` tests run one at a time after the rest). It ends at once, green, when master is still at the commit of the last nightly that passed. |
| Weekly schedule | `linux:qt68` (Qt 6.8.3, with `-DSEABASS_STORM_TESTS=ON`, every label), `craft_windows_qt6_x86_64`, `craft_appimage_qt6_x86_64`, `macos:universal`. |
| Buttons | `linux:qt68` on `Seabass/X.Y` (the release check before a tag), `linux:coverage` on any branch, the package jobs. |
| Release tag | `tag:tested-commit`, then the packages. No test lane: the job refuses the tag unless a branch pipeline at the same commit passed with `linux:build-test` in it (and, if `linux:qt68` ran there, it passed). |

The numbers behind the split: every build on Invent is cold (there is no
cache storage, so `cache:` does nothing), about 80 minutes on a loaded
runner, and a serial ctest took 3405 s on top, 1756 s of it the QML suites
and 836 s the storm. Before this, every push to master and to `Seabass/X.Y`
ran two such lanes and every tag ran them again.

No job uses `retry:`. A flaky test is a bug to be fixed, not a job to be
run twice.

`tools/pre-push-check.sh` is the other half of this: it runs everything
CI on a push does not, on the contributor's machine (see
`CONTRIBUTING.md`).

## What needs setting up by hand

None of these can be done from a repo checkout; they are GitLab
project actions.

### The two schedules

GitLab has no cron syntax inside `.gitlab-ci.yml`; schedules live under
the project's Build -> Pipeline schedules (Developer role is enough).
Two, both on target branch `master`:

| Description | Cron (example) | Variable |
|---|---|---|
| Nightly | `0 2 * * *` | `SEABASS_SCHEDULE` = `nightly` |
| Weekly | `0 3 * * 6` | `SEABASS_SCHEDULE` = `weekly` |

A schedule without the variable, or with any value but `weekly`, is
treated as the nightly. No schedule on `Seabass/X.Y` branches: a release
branch is checked with the `linux:qt68` button before its tag. A schedule
on a `Seabass/X.Y` branch would run nothing but the scanners, unless it is
a weekly, which would also sign its macOS package.

### A self-hosted Windows runner (not set up)

The MSYS2 `windows:*` chain stays switched off until one exists (see
`SEABASS_WINDOWS_RUNNER` in `.gitlab-ci.yml`); when it runs, it runs on
the weekly schedule, on tags, and as a button on `Seabass/X.Y`.

KDE Invent's shared runners are Linux-only -- there's no Windows shared
runner on the community tier. The Windows lane needs a machine with the
full toolchain already installed (MSYS2 UCRT64 with Qt6/ffmpeg/sqlcipher,
Inno Setup 6) registered as a GitLab Runner:

```powershell
# Download gitlab-runner.exe from https://docs.gitlab.com/runner/install/windows.html,
# then from wherever it lives:
.\gitlab-runner.exe register --url https://invent.kde.org --token <project-runner-token> `
    --executor shell --tag-list windows --description "seabass-windows"
.\gitlab-runner.exe install
.\gitlab-runner.exe start
```

Shell executor, not Docker -- the point is to reuse the toolchain that's
already installed rather than provisioning it per job, since that install
is the expensive, slow-changing part. Register it to run as a Windows
service so it survives reboots and logoffs. The project runner token
comes from Invent's project Settings -> CI/CD -> Runners.

## The Craft lanes

`.gitlab-ci.yml` includes KDE's Craft templates, which build the
`qt-apps/seabass` blueprint that ships in `craft-blueprint/` and produce
the Windows installer and the AppImage KDE distributes. They package;
they do not test (craft-ci sets `/.buildTests = False`). The Craft macOS
halves are parked; the Mac package is `macos:universal`, built on Qt 6.8
outside Craft. All of them run on the weekly schedule and on release
tags, and are buttons everywhere else.

So Windows is built twice, on purpose and by different toolchains:
Craft/MSVC for what KDE ships, MSYS2/UCRT64 in `windows:*` for what
`docs/windows-build.md` describes and what the installer test exercises.
If the self-hosted runner never happens, the Craft lane still covers
"does it build on Windows"; what would be missing is the test suite and
the installer check.

## Testing the installer locally

`tools\test-installer.ps1` is what `windows:installer-test` runs -- silent
install to an isolated directory, smoke-runs `seabass-cli.exe --help`
from the installed location (an unambiguous pass, unlike `--version`,
which isn't a recognized flag and falls through to help text anyway but
with a nonzero exit), then silent uninstall, checking nothing is left
behind either way. Run it yourself after `tools\windows-installer.iss`
produces an installer:

```powershell
.\tools\test-installer.ps1
```
