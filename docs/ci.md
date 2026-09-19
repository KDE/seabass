<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# CI

`.gitlab-ci.yml` runs on KDE Invent. Four things run there, and they do
different jobs: the two lanes below, which build and TEST, and KDE's own
Craft templates, which build and PACKAGE (see "The Craft lanes" at the
bottom). Neither Craft job runs ctest, so the Linux lane is the only
thing on Invent that does.

Two lanes, because the platforms have very different constraints:

| | Linux | Windows |
|---|---|---|
| Runner | KDE Invent shared runners | self-hosted, tag `windows` (see below) |
| Runs on | every push/MR | nightly schedule + release tags only |
| Stages | build → test | build → test → package → installer-test |

The Linux lane is the fast gate: build, then `ctest` with the
`integration` label excluded -- twelve tests as of this writing, worth
about two minutes of the suite's wall clock (`corpus_test`,
`anonymization_verifier_test`, `library_backup_roundtrip_test` and the
rest; `ctest -N -L integration` lists them). Per `docs/testing.md` those
are meant to run "before merging a larger change or cutting a release,"
not on every push. The full lane -- both
platforms, every ctest label, plus building and exercising the actual
Windows installer -- runs nightly and on release tags.

## What still needs manual, one-time setup

Neither of these can be done from a repo checkout; both are GitLab
project/account actions.

### 1. A self-hosted Windows runner

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

### 2. A nightly pipeline schedule

GitLab doesn't support cron syntax inside `.gitlab-ci.yml` -- schedules
are a project setting: Invent's project -> Build -> Pipeline schedules ->
New schedule. Target branch `master`, a cron expression such as
`0 2 * * *`, and no extra variables needed -- `$CI_PIPELINE_SOURCE ==
"schedule"` is set automatically and is what `.gitlab-ci.yml`'s rules
key off of.

## Known-unverified part

The Linux job's `apt-get install` package list (Qt6 components, mainly)
is a best-effort guess, not yet confirmed against a real run -- the work
this came from was written on a Windows-only toolchain, so there was no
way to test the Linux lane directly. The first real pipeline run on
Invent will very likely need a package-list fix or two; that's expected,
not a sign the overall design is wrong.

The `tags: [linux]` on the job is the same kind of guess, and it fails
differently: a tag no runner offers leaves the job sitting pending
rather than failing, so a first pipeline that never starts means the
tag, not the build. Invent's project -> Settings -> CI/CD -> Runners
lists what the available runners actually answer to.

Four packages were added to that list when this landed, because the
build cannot work without them at all: `pkg-config` and `libtag1-dev`
(CMakeLists reaches TagLib 1.x through `taglib.pc`), `qt6-tools-dev` for
Qt6::Test/QuickTest, and `qt6-shadertools-dev` plus `xvfb`, without
which `seabass_qml_shader_tests` does not register -- and does not
register quietly, which is the failure mode to watch for: a green
pipeline that never ran the shader tests at all. CMake prints a warning
in that case; read it on the first run.

## The Craft lanes

Separate from the two lanes above, `.gitlab-ci.yml` includes KDE's
`craft-windows-x86-64-qt6` and `craft-macos-arm64-qt6` templates, which
build the `qt-apps/seabass` blueprint that ships in `craft-blueprint/`
and produce the installer and the signed .dmg KDE distributes. They
package; they do not test (craft-ci sets `/.buildTests = False`). The
Windows Craft job is currently `allow_failure: true` while libvpx is
broken on MSVC upstream -- `.gitlab-ci.yml` says when to take that off.

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
