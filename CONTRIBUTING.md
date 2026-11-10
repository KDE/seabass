<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: CC-BY-SA-4.0
-->

# Contributing to Seabass

Seabass lives on KDE Invent, [multimedia/seabass](https://invent.kde.org/multimedia/seabass).
Building is described in the [README](README.md), the test suite in
[`docs/testing.md`](docs/testing.md), and the git hooks every clone
should use in [`.githooks/README.md`](.githooks/README.md).

## Before you push

Run the whole suite on your own machine:

```
tools/pre-push-check.sh
```

It configures (or reuses) `build-pre-push/`, builds it, and runs every
test there is: the C++ tests, both QML suites (the pinned Basic style and
the desktop style Linux ships with), the storm, and the `integration` and
`timing` tests. On Linux it runs them under `xvfb-run -a`, so nothing
opens on your desktop. It ends with a one-line verdict and exits non-zero
if any test failed **or was skipped**: a skipped test proved nothing.
`tools/pre-push-check.sh -h` lists its options (another build directory,
`-j`, a ctest `-R` filter for a quick look).

It takes a while, on purpose. CI on a push runs only the part of the suite
that is cheap and does not depend on a display or the clock, so most of
what can break is caught here or not until the nightly.

## What CI runs

| When | What runs |
|---|---|
| Push to any branch, merge request | `linux:build-test`: build without the QML suites, then the C++ tests except the `qml`, `storm`, `integration` and `timing` labels; plus `reuse` and the security scanners. A push that changes only documentation is not built. |
| Nightly (master) | `linux:nightly`: everything except the storm, on Ubuntu's Qt. Skipped when master has not moved since the last nightly that passed. |
| Weekly (master) | `linux:qt68` (Qt 6.8.3, the Qt the Mac package uses) with the storm, and the packages for Windows, Linux (AppImage) and macOS. |
| Button | `linux:qt68` as the release check on a `Seabass/X.Y` branch, `linux:coverage`, and the package jobs. |
| Release tag | Packages only. `tag:tested-commit` first refuses the tag unless a branch pipeline at the same commit passed. |

No job retries a failed test. A test that fails now and then is a bug,
in the test or in Seabass; open an issue with the job's log. The details,
and how the schedules are set up, are in [`docs/ci.md`](docs/ci.md).
