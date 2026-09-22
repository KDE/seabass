<!--
SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>

SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
-->

# Commit hooks

Three house rules, enforced where they can still be fixed.

| Hook | What it does |
|---|---|
| `pre-commit` | Refuses a commit that adds a file with no SPDX licence header. KDE CI runs `reuse lint`, and a missing header reds the whole pipeline -- minutes later, publicly, and on whoever is watching. `.reuse/dep5`'s patterns are read rather than repeated, so exemptions cannot drift. `--all` checks every tracked file. |
| `commit-msg` | Rewrites the message: drops `Co-Authored-By: Claude` / `Claude-Session:` trailers, replaces em-dashes with `--` |
| `pre-push` | Refuses to push any commit, not already on the remote, whose message still carries either |

## Turning them on

Git does not clone hooks, so every checkout needs this once:

```sh
git config core.hooksPath .githooks
```

It is repository-local config, shared by every worktree of the same
clone, so one run covers them all. `tools/../scripts` does not do it for
you on purpose: a repo that silently installs executable hooks on
checkout is a thing to be wary of, not a convenience.

## Why a hook and not just review

Both rules protect against something that cannot be undone later.
Invent's `master` is a protected branch with force-push disabled, so a
commit message that lands there is final. On 2026-09-18 nineteen
trailered commits reached it across two sessions; the rewrite was
prepared and verified and then could not be pushed. Those trailers are
in the history permanently.

A `reuse lint` failure, by contrast, is fixed in the next commit, which
is why it belongs in CI (where it runs in about 12 seconds) and not
here.

## Why `commit-msg` rewrites instead of refusing

The tool that writes these trailers is the same tool that would have to
react to a refusal. Rewriting always works; a refusal is something to be
argued with or bypassed.

`pre-push` does refuse, because by then rewriting behind the author's
back would mean changing commits they have already made.

## Scope

The trailer match is anchored and names Claude explicitly, so a human
`Co-Authored-By:` survives untouched. Only commits that are **not yet on
the remote** are checked -- the ones already on `master` are history, and
checking them would fail every push forever.

`tests/no_claude_trailers_test.cpp` runs these scripts for real against a
throwaway repository, including the new-branch case where git reports the
remote hash as zeros. That case is the one worth having: git's `--not`
negates every ref after it, so `--not --remotes=origin $tip` silently
checks nothing while still exiting 0, and it reads almost identically to
the correct `$tip --not --remotes=origin`.
