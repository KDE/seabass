// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import SeabassGui

// One seed of the storm: the real Main.qml and its real controllers,
// sticks going in and out (stormFixture), and a walk of `steps` things a
// person or the OS does, picked by a PRNG seeded with `seed`. After every
// step the invariants below are checked; the first one broken ends the
// walk with `failure` set and `done` true. See docs/testing.md, "The
// storm".
//
// Asynchronous on purpose, driven by one timer: the same walk runs inside
// a TestCase (tst_Storm.qml waits for `done`) and as the app itself in the
// quit leg (StormQuitDriver.qml), where nothing may block the event loop.
Item {
    id: runner

    property int seed: 1
    property int steps: 150
    property int stickCount: 3
    property string fixtureRoot: ""
    // The quit leg: at this step the window is closed as a person closes
    // it, and the walk is over. -1: never.
    property int quitStep: -1
    // Every this many steps, and at the end: the weather calms down and
    // everything has to come to rest.
    property int restEvery: 25

    property bool done: false
    property string failure: ""
    property var log: []
    property int step: 0

    property var window: null
    property var stack: null
    property var media: null

    // What each stick went through, by index: a save it was pulled out of.
    property var pulledMidSave: ({})

    signal finished()

    // ---- the PRNG: mulberry32, so a seed walks the same way everywhere ----
    property real prngState: 0
    function random() {
        let t = (runner.prngState + 0x6D2B79F5) | 0;
        runner.prngState = t;
        t = Math.imul(t ^ (t >>> 15), t | 1);
        t ^= t + Math.imul(t ^ (t >>> 7), t | 61);
        return ((t ^ (t >>> 14)) >>> 0) / 4294967296;
    }
    function below(n) { return Math.floor(runner.random() * n); }
    function chance(p) { return runner.random() < p; }
    function pick(list) { return list[runner.below(list.length)]; }

    // ---- one continuation at a time ----
    Timer {
        id: clock
        property var then: null
        repeat: false
        onTriggered: {
            stormFixture.beat(runner.seed, runner.step);
            const next = clock.then;
            clock.then = null;
            if (next && !runner.done) {
                try {
                    next();
                } catch (e) {
                    runner.fail("the walk threw: " + e + "\n" + (e.stack || ""));
                }
            }
        }
    }
    function after(ms, fn) {
        clock.then = fn;
        clock.interval = Math.max(0, ms);
        clock.restart();
    }
    // Polls `cond` every 20 ms; `fn(true)` once it holds, `fn(false)`
    // after `timeoutMs`.
    function waitFor(cond, timeoutMs, fn) {
        const started = Date.now();
        const poll = () => {
            let ok = false;
            try {
                ok = cond();
            } catch (e) {
                ok = false;
            }
            if (ok) {
                fn(true);
            } else if (Date.now() - started > timeoutMs) {
                fn(false);
            } else {
                runner.after(20, poll);
            }
        };
        poll();
    }

    Component {
        id: mainComponent
        Main {}
    }

    function note(text) {
        const line = runner.step + ": " + text;
        runner.log.push(line);
        stormFixture.noteStep(line);
        if (stormFixture.env("SEABASS_STORM_VERBOSE").length > 0) {
            stormFixture.log("storm " + runner.seed + " " + line);
        }
    }

    function fail(why) {
        if (runner.done) {
            return;
        }
        runner.failure = "seed " + runner.seed + ", step " + runner.step + ": " + why;
        runner.finish();
    }

    function finish() {
        stormFixture.disarm();
        runner.done = true;
        clock.stop();
        runner.finished();
    }

    function start() {
        runner.prngState = runner.seed * 2654435761;
        const error = stormFixture.prepare(runner.seed, runner.stickCount, runner.fixtureRoot);
        if (error.length > 0) {
            runner.fail("could not prepare the sticks: " + error);
            return;
        }
        for (let i = 0; i < runner.stickCount; ++i) {
            if (stormFixture.seedStore(i, 40) === 0) {
                runner.fail("could not seed the metadata store from S" + i);
                return;
            }
        }
        stormFixture.startCapture();
        stormFixture.takeWarnings();
        stormFixture.clearSteps();
        stormFixture.takeFreeze();
        stormFixture.beat(runner.seed, 0);
        runner.window = mainComponent.createObject(null);
        if (!runner.window) {
            runner.fail("Main.qml did not build");
            return;
        }
        runner.stack = stormFixture.findObject(runner.window, "QQuickStackView");
        runner.media = stormFixture.findObject(runner.window, "seabass::gui::MediaController");
        if (!runner.stack || !runner.media) {
            runner.fail("Main.qml has no StackView or MediaController");
            return;
        }
        // The sticks go in before the walk, as though they were in when
        // the app started: MediaController sees them on its first detect.
        runner.media.detect();
        runner.waitFor(() => runner.media.sticks.count === runner.stickCount, 5000, ok => {
            if (!ok) {
                runner.fail("MediaController listed " + runner.media.sticks.count + " of the "
                            + runner.stickCount + " sticks");
                return;
            }
            runner.weather();
            runner.after(0, runner.nextStep);
        });
    }

    // ---- the world ----

    function weather() {
        // Mostly an ordinary stick; now and then a slow one, one that
        // does not answer, or a file that will not read.
        const kind = runner.below(4);
        if (kind === 0) {
            stormFixture.setWeather(10, 0, 0, 100, 0);
            runner.note("weather: calm");
        } else if (kind === 1) {
            stormFixture.setWeather(50, 0, 3, 600, 0);
            runner.note("weather: slow");
        } else if (kind === 2) {
            stormFixture.setWeather(30, 25, 5, 400, 40);
            runner.note("weather: stuck");
        } else {
            stormFixture.setWeather(35, 12, 12, 300, 30);
            runner.note("weather: stormy");
        }
    }

    function pluggedSticks() {
        const out = [];
        for (let i = 0; i < runner.stickCount; ++i) {
            if (stormFixture.plugged(i)) {
                out.push(i);
            }
        }
        return out;
    }
    function pulledSticks() {
        const out = [];
        for (let i = 0; i < runner.stickCount; ++i) {
            if (!stormFixture.plugged(i)) {
                out.push(i);
            }
        }
        return out;
    }
    // A stick as the home page lists it, or null while it is not listed.
    function listed(i) {
        const root = stormFixture.stickRoot(i);
        for (let row = 0; row < runner.media.sticks.count; ++row) {
            const s = runner.media.sticks.get(row);
            if (s.mountPoint === root) {
                return s;
            }
        }
        return null;
    }
    function listedSticks() {
        return runner.pluggedSticks().filter(i => runner.listed(i) !== null);
    }

    function page() { return runner.stack.currentItem; }
    function pageName(p) {
        if (!p) {
            return "none";
        }
        const name = String(p).replace(/_QMLTYPE_\d+.*$/, "").replace(/\(.*$/, "");
        return name;
    }
    function home() { return runner.stack.get(0); }

    // ---- the actions ----

    readonly property var writingPages: ["SyncPage", "CleanupPage", "MetadataRestorePage", "JunkCuePage",
        "CuesAtZeroPage", "SampleRatesPage", "ImportPromptPage", "MetadataBackupPage", "StickBackupPage"]

    function actions() {
        const onHome = runner.stack.depth === 1;
        const current = runner.page();
        const name = runner.pageName(current);
        const writes = runner.writingPages.indexOf(name) >= 0;
        const cancels = !onHome && runner.visibleButtons(current, "Cancel").length > 0;
        const list = [];
        const add = (weight, name, fn) => { if (weight > 0) list.push({weight: weight, name: name, fn: fn}); };
        add(runner.pulledSticks().length > 0 ? 7 : 0, "insert", runner.actInsert);
        add(runner.pluggedSticks().length > 0 ? 5 : 0, "pull", runner.actPull);
        add(runner.pluggedSticks().length > 0 ? 5 : 0, "replug", runner.actReplug);
        add(runner.listedSticks().length > 0 ? (onHome ? 30 : 6) : 0, "open", runner.actOpen);
        add(!onHome ? 8 : 0, "sub", runner.actSub);
        add(cancels ? 12 : 0, "cancel", runner.actCancel);
        add(!onHome ? 9 : 0, "back", runner.actBack);
        add(!onHome ? 5 : 0, "reenter", runner.actReenter);
        add(!onHome ? 6 : 0, "scope", runner.actScope);
        add(writes ? 14 : 0, "save", runner.actSave);
        add(stormFixture.held() > 0 ? 8 : 0, "answer", runner.actAnswer);
        add(4, "idle", runner.actIdle);
        add(2, "weather", () => { runner.weather(); runner.after(0, runner.endStep); });
        let total = 0;
        for (const a of list) total += a.weight;
        let roll = runner.random() * total;
        for (const a of list) {
            roll -= a.weight;
            if (roll < 0) {
                return a;
            }
        }
        return list[list.length - 1];
    }

    function nextStep() {
        if (runner.done) {
            return;
        }
        if (runner.step >= runner.steps) {
            runner.rest("the end of the walk", () => runner.end());
            return;
        }
        runner.step += 1;
        if (runner.quitStep >= 0 && runner.step >= runner.quitStep) {
            runner.quit();
            return;
        }
        if (runner.step % runner.restEvery === 0) {
            runner.rest("a pause in the storm", () => { runner.weather(); runner.after(0, runner.act); });
            return;
        }
        runner.act();
    }

    function act() {
        // A dialog in front is answered first, most of the time: a person
        // cannot reach the page under a modal one.
        const popups = runner.answerablePopups();
        if (popups.length > 0 && runner.chance(0.8)) {
            runner.actDialog(popups);
            return;
        }
        const a = runner.actions();
        a.fn();
    }

    // Every step ends here: a short, random pause, then the invariants.
    function endStep() {
        runner.after(runner.below(4) === 0 ? runner.below(600) : runner.below(120), () => {
            if (runner.checkStep()) {
                runner.nextStep();
            }
        });
    }

    function actInsert() {
        const i = runner.pick(runner.pulledSticks());
        if (runner.chance(0.3)) {
            stormFixture.changeWhileOut(i);
            runner.note("S" + i + " changed while out");
        }
        runner.note("insert S" + i);
        stormFixture.insert(i);
        runner.endStep();
    }

    function actPull() {
        const i = runner.pick(runner.pluggedSticks());
        runner.note("pull S" + i);
        stormFixture.pull(i);
        runner.endStep();
    }

    function actReplug() {
        const i = runner.pick(runner.pluggedSticks());
        const gap = runner.below(700);
        const changed = runner.chance(0.4);
        runner.note("replug S" + i + " after " + gap + " ms" + (changed ? ", changed" : ""));
        stormFixture.pull(i);
        runner.after(gap, () => {
            if (changed) {
                stormFixture.changeWhileOut(i);
            }
            stormFixture.insert(i);
            runner.endStep();
        });
    }

    function stickArgs(i) {
        const s = runner.listed(i);
        return {
            label: s.label, rb: s.rekordboxPath, engine: s.enginePath, mount: s.mountPoint,
            device: s.devicePath, libraryId: s.libraryId,
        };
    }

    // The pages a stick's tools open from the home page.
    function opener(kind, i) {
        const s = runner.stickArgs(i);
        const h = runner.home();
        switch (kind) {
        case "browse": return () => h.browseRequested(s.label, s.rb, s.engine);
        case "duplicates": return () => h.duplicateTracksHubRequested(s.label, s.rb, s.engine);
        case "health": return () => h.libraryHealthRequested(s.label, s.rb, s.engine);
        case "statistics": return () => h.stickStatisticsRequested(s.label, s.rb, s.engine);
        case "performance": return () => h.stickPerformanceRequested(s.label, s.rb, s.engine, s.mount);
        case "metadataBackup": return () => h.metadataBackupRequested(s.label, s.rb, s.engine, s.libraryId);
        case "metadataRestore": return () => h.metadataRestoreRequested(s.label, s.rb, s.engine, s.libraryId);
        case "sync": return () => h.syncRequested(s.label, s.rb, s.engine);
        case "backups": return () => h.backupsHubRequested(s.label, s.rb, s.engine, s.mount, s.device);
        case "settings": return () => h.settingsRequested(s.label, s.rb);
        case "restoreStick": return () => h.restoreStickBackupRequested(s.mount, s.device, "", s.label);
        case "clone": {
            const others = runner.listedSticks().filter(j => j !== i);
            if (others.length === 0) {
                return null;
            }
            const t = runner.stickArgs(runner.pick(others));
            return () => h.cloneStickRequested(s.label, s.rb, s.engine, t.mount, t.label, true);
        }
        }
        return null;
    }
    // Weighted towards the pages that write, where a pull does the most.
    readonly property var openable: ["browse", "browse", "duplicates", "duplicates", "health", "health",
        "statistics", "performance", "metadataBackup", "metadataBackup", "metadataRestore", "metadataRestore",
        "metadataRestore", "sync", "sync", "sync", "backups", "settings", "restoreStick", "clone"]

    property var lastOpen: null

    function actOpen() {
        const i = runner.pick(runner.listedSticks());
        const kind = runner.pick(runner.openable);
        const open = runner.opener(kind, i);
        if (!open) {
            runner.actIdle();
            return;
        }
        const go = () => {
            runner.note("open " + kind + " on S" + i);
            runner.lastOpen = {kind: kind, stick: i};
            open();
            runner.endStep();
        };
        if (runner.stack.depth > 1) {
            // Home first, the way the header's home link goes.
            runner.leave(true, go);
        } else {
            go();
        }
    }

    // Leaves the page in front through its own header, so a page with
    // staged work asks first (and that dialog is then answered by a later
    // step, or by this one).
    function leave(toHome, then) {
        const p = runner.page();
        const crumbs = stormFixture.findObjects(p, "BackBreadcrumb", false);
        if (crumbs.length > 0) {
            runner.note((toHome ? "home" : "back") + " from " + runner.pageName(p));
            if (toHome) {
                crumbs[0].homeRequested();
            } else {
                crumbs[0].backRequested();
            }
        } else {
            runner.note("pop " + runner.pageName(p));
            if (toHome) {
                runner.stack.pop(null);
            } else {
                runner.stack.pop();
            }
        }
        // StackView changes on the next frames; an unsaved-changes dialog
        // keeps the page where it is.
        runner.after(30, then);
    }

    function actBack() {
        runner.leave(runner.chance(0.3), runner.endStep);
    }

    function actReenter() {
        const last = runner.lastOpen;
        if (!last || runner.listed(last.stick) === null) {
            runner.actBack();
            return;
        }
        runner.leave(true, () => {
            if (runner.stack.depth !== 1 || runner.listed(last.stick) === null) {
                runner.endStep();
                return;
            }
            const open = runner.opener(last.kind, last.stick);
            if (open) {
                runner.note("reopen " + last.kind + " on S" + last.stick);
                open();
            }
            runner.endStep();
        });
    }

    // A page opened from the page in front: two pages on one stick.
    function actSub() {
        const p = runner.page();
        const name = runner.pageName(p);
        const s = {label: p.stickLabel, rb: p.rekordboxPath, engine: p.enginePath};
        let fn = null;
        let what = "";
        if (name === "DuplicatesHubPage") {
            const which = runner.pick([0, 1, 1, 1, 2, 3, 3]);
            what = ["duplicates", "cleanup", "pending deletions", "junk cues"][which];
            fn = [() => p.duplicatesStatsRequested(s.label, s.rb, s.engine),
                  () => p.cleanupRequested(s.label, s.rb, s.engine),
                  () => p.pendingDeletionsRequested(s.label, s.rb, s.engine),
                  () => p.junkCueCleanupRequested(s.label, s.rb, s.engine)][which];
        } else if (name === "LibraryHealthHubPage") {
            what = runner.pick(["broken", "junkcues", "import", "samplerates", "artwork", "cleanupleftovers"]);
            fn = () => p.detailRequested(what);
        } else if (name === "StickStatisticsPage") {
            what = "sync";
            fn = () => p.syncRequested(s.label, s.rb, s.engine);
        } else if (name === "MetadataBackupPage") {
            what = "restore";
            fn = () => p.metadataRestoreRequested();
        } else if (name === "BackupsHubPage") {
            const which = runner.below(3);
            what = ["manage backups", "full stick backup", "restore"][which];
            fn = [() => p.manageBackupsRequested(s.label, ""),
                  () => p.fullStickBackupRequested(s.label, s.rb, s.engine),
                  () => p.restoreStickBackupRequested(p.mountPoint, p.devicePath, "")][which];
        }
        if (!fn) {
            runner.actScope();
            return;
        }
        runner.note("from " + name + " open " + what);
        fn();
        runner.endStep();
    }

    function visibleButtons(root, text) {
        return stormFixture.findObjects(root, "QQuickAbstractButton", true).filter(b => b.text === text);
    }

    function actCancel() {
        const p = runner.page();
        const buttons = runner.visibleButtons(p, "Cancel");
        if (buttons.length === 0) {
            runner.note("nothing to cancel on " + runner.pageName(p));
            runner.endStep();
            return;
        }
        runner.note("cancel on " + runner.pageName(p));
        runner.pick(buttons).clicked();
        runner.endStep();
    }

    // Another stick, playlist, source or format on the page in front,
    // picked the way the control hands it to the page.
    function actScope() {
        const p = runner.page();
        const choices = [];
        for (const combo of stormFixture.findObjects(p, "QQuickComboBox", true)) {
            if (combo.count < 2) {
                continue;
            }
            const isPlaylists = String(combo).indexOf("PlaylistPickerCombo") === 0;
            choices.push(() => {
                const index = runner.below(combo.count);
                runner.note("pick " + (isPlaylists ? "playlist " : "entry ") + index + " of " + combo.count
                            + " on " + runner.pageName(p));
                combo.currentIndex = index;
                if (isPlaylists) {
                    const entries = combo.model;
                    const entry = entries && entries[index] !== undefined ? entries[index] : {name: ""};
                    combo.playlistPicked(index, entry);
                } else {
                    combo.activated(index);
                }
            });
        }
        for (const toggle of stormFixture.findObjects(p, "LibrarySourceToggle", true)) {
            const values = toggle.entries.filter(e => e.selectable && e.value !== toggle.current).map(e => e.value);
            if (values.length === 0) {
                continue;
            }
            choices.push(() => {
                const value = runner.pick(values);
                runner.note("source " + value + " on " + runner.pageName(p));
                toggle.sourceRequested(value);
            });
        }
        if (choices.length === 0) {
            runner.note("nothing to pick on " + runner.pageName(p));
        } else {
            runner.pick(choices)();
        }
        runner.endStep();
    }

    function actAnswer() {
        const n = runner.below(3);
        runner.note("the stick answers (read " + n + ")");
        stormFixture.releaseOne(n);
        runner.endStep();
    }

    function actIdle() {
        const ms = 100 + runner.below(900);
        runner.note("wait " + ms + " ms");
        runner.after(ms, runner.endStep);
    }

    // Dialogs a person answers. The progress of a write is not one: it
    // is only ever cancelled, and that is a step of its own.
    function answerablePopups() {
        return stormFixture.openPopups(runner.window).filter(p => {
            const type = String(p);
            return type.indexOf("WriteProgressDialog") < 0 && type.indexOf("TransferOverlay") < 0;
        });
    }

    function actDialog(popups) {
        const popup = runner.pick(popups);
        const what = String(popup).replace(/_QMLTYPE_\d+.*$/, "").replace(/\(.*$/, "");
        const title = popup.title !== undefined ? " \"" + popup.title + "\"" : "";
        if (typeof popup.accept === "function" && runner.chance(0.6)) {
            runner.note("accept " + what + title);
            popup.accept();
        } else if (typeof popup.reject === "function" && runner.chance(0.5)) {
            runner.note("reject " + what + title);
            popup.reject();
        } else {
            runner.note("close " + what + title);
            popup.close();
        }
        runner.endStep();
    }

    // ---- saves ----

    function sessionFor(p) {
        const path = p.rekordboxPath || p.enginePath || "";
        if (path.length === 0) {
            return null;
        }
        const id = EditSessionRegistry.libraryIdForPath(path);
        return id.length > 0 ? EditSessionRegistry.sessionFor(id, p.stickLabel || "") : null;
    }

    // Stages everything the page in front offers, the way its buttons do,
    // or returns false when it offers nothing.
    function stageAll(p) {
        const name = runner.pageName(p);
        const controllerOf = type => stormFixture.findObject(p, type);
        if (name === "SyncPage") {
            const c = controllerOf("seabass::gui::SyncController");
            if (!c || c.busy) return false;
            c.setAllIncluded(true);
            c.stageSelected(false);
            return true;
        }
        if (name === "CleanupPage") {
            const c = controllerOf("seabass::gui::CleanupController");
            if (!c || c.busy) return false;
            c.setAllIncluded(true);
            c.apply(false);
            return true;
        }
        if (name === "MetadataRestorePage") {
            const c = p.controller;
            if (!c || c.busy) return false;
            c.stageAll();
            return true;
        }
        if (name === "JunkCuePage" || name === "CuesAtZeroPage") {
            const c = p.consistencyController || controllerOf("seabass::gui::LibraryConsistencyController");
            if (!c || c.busy) return false;
            c.removeAllJunkCues();
            return true;
        }
        if (name === "SampleRatesPage") {
            const c = p.consistencyController;
            if (!c || c.busy) return false;
            c.fillSampleRates();
            return true;
        }
        if (name === "ImportPromptPage") {
            const c = p.consistencyController;
            if (!c || c.busy) return false;
            c.markRekordboxImported();
            return true;
        }
        return false;
    }

    // A person waits for the page's read to finish, then stages what it
    // offers and saves. The stick answers a held read now and then while
    // they wait; a page that does not settle in 20 s is left alone.
    function actSave() {
        const p = runner.page();
        const started = Date.now();
        let polls = 0;
        const ready = () => {
            if (runner.page() !== p) {
                runner.note("the page went while waiting to save");
                runner.endStep();
                return;
            }
            if (stormFixture.busyNow(p).length > 0) {
                if (Date.now() - started > 20000) {
                    runner.note("gave up waiting to save on " + runner.pageName(p));
                    runner.endStep();
                    return;
                }
                if (++polls % 25 === 0 && stormFixture.held() > 0) {
                    stormFixture.releaseOne(0);
                }
                runner.after(20, ready);
                return;
            }
            runner.save(p);
        };
        ready();
    }

    function save(p) {
        const name = runner.pageName(p);
        if (name === "MetadataBackupPage") {
            const c = stormFixture.findObject(p, "seabass::gui::MetadataBackupController");
            runner.note("back up the metadata on " + name);
            c.stageAllForAdd();
            c.save();
            runner.endStep();
            return;
        }
        if (name === "StickBackupPage") {
            runner.note("start a full stick backup");
            p.controller.backUp();
            runner.endStep();
            return;
        }
        const session = runner.sessionFor(p);
        if (!session || session.writing) {
            runner.note("no session to save on " + name);
            runner.endStep();
            return;
        }
        if (!runner.stageAll(p) || session.pendingCount === 0) {
            runner.note("nothing to stage on " + name);
            runner.endStep();
            return;
        }
        const i = stormFixture.stickOf(p.rekordboxPath || p.enginePath);
        const pullAt = runner.chance(0.5) ? runner.below(1500) : -1;
        runner.note("save " + session.pendingCount + " change(s) on " + name + " (S" + i + ")"
                    + (pullAt >= 0 ? ", pulled after " + pullAt + " ms" : ""));
        session.save();
        let pulled = false;
        if (pullAt >= 0) {
            runner.after(pullAt, () => {
                if (stormFixture.plugged(i)) {
                    pulled = session.writing;
                    runner.note("pull S" + i + (pulled ? " in the middle of the save" : " after the save"));
                    if (pulled) {
                        runner.pulledMidSave[i] = true;
                    }
                    stormFixture.pull(i);
                }
                runner.awaitSave(session, i, () => pulled);
            });
        } else {
            runner.awaitSave(session, i, () => false);
        }
    }

    // A save ends, and what it leaves is whole: its lock given back, no
    // note of a save in progress, unless the stick went away under it.
    function awaitSave(session, i, wasPulled) {
        let answered = 0;
        const started = Date.now();
        const poll = () => {
            if (!session.writing) {
                if (!wasPulled() && stormFixture.plugged(i)) {
                    const root = stormFixture.stickRoot(i);
                    const notes = stormFixture.interruptedSave(root);
                    if (notes.length > 0) {
                        runner.fail("a save that ended with S" + i + " in left a note of a save in progress: " + notes);
                        return;
                    }
                    if (!session.dirty && stormFixture.lockHeld(root)) {
                        runner.fail("a save that ended with nothing staged left S" + i + "'s write lock held");
                        return;
                    }
                    // What a save wrote can be undone: its backups are the journal.
                    const summary = session.lastSummary || {};
                    if (!summary.error && !summary.cancelled && summary.written > 0 && !session.canUndo) {
                        runner.fail("a save on S" + i + " wrote " + summary.written + " and cannot be undone");
                        return;
                    }
                }
                const summary = session.lastSummary || {};
                runner.note("the save ended: " + (summary.written || 0) + " written"
                            + (summary.error ? ", error: " + summary.error : "")
                            + (summary.cancelled ? ", cancelled" : ""));
                runner.endStep();
                return;
            }
            // A save reads through the same stick: it answers eventually.
            if (stormFixture.held() > 0 && ++answered % 10 === 0) {
                stormFixture.releaseOne(0);
            }
            if (Date.now() - started > 120000) {
                runner.fail("a save on S" + i + " did not end within 120 s");
                return;
            }
            runner.after(50, poll);
        };
        poll();
    }

    // ---- invariants ----

    function checkStep() {
        const freeze = stormFixture.takeFreeze();
        if (freeze.length > 0) {
            runner.fail(freeze);
            return false;
        }
        for (const written of stormFixture.takeWrittenWhileOut()) {
            runner.note("written to a pulled stick's mount point: " + written);
        }
        const warnings = stormFixture.takeWarnings().filter(w => runner.warningCounts(w));
        if (warnings.length > 0) {
            runner.fail("QML warned:\n  " + warnings.join("\n  "));
            return false;
        }
        const p = runner.page();
        const wrong = runner.foreignData(p);
        if (wrong.length > 0) {
            runner.fail(wrong);
            return false;
        }
        return true;
    }

    // Which warnings the storm counts: QML's own, and anything the app
    // says is wrong in its logic. The Qt Multimedia and platform noise of
    // a display with no audio is not the app's.
    function warningCounts(w) {
        // The anonymized fixture carries no cover images: a cover the
        // catalog names and the stick lacks is a case ArtworkImage handles.
        if (w.indexOf("QQuickImage: Cannot open") >= 0) {
            return false;
        }
        if (w.indexOf("qrc:/") >= 0 || w.indexOf(".qml:") >= 0 || w.indexOf("TypeError") >= 0
            || w.indexOf("ReferenceError") >= 0 || w.indexOf("Binding loop") >= 0) {
            return true;
        }
        if (w.indexOf("ASSERT") >= 0 || w.indexOf("QObject::") >= 0 || w.indexOf("QGridLayoutEngine") >= 0 || w.indexOf("QFutureWatcher") >= 0
            || w.indexOf("QThread") >= 0 || w.indexOf("QBasicTimer") >= 0) {
            return true;
        }
        return false;
    }

    // A page shows only its own stick's tracks.
    function foreignData(p) {
        const name = runner.pageName(p);
        // Pages that show the metadata store, whose rows come from any
        // stick, or two sticks at once.
        if (["MetadataBackupPage", "MetadataRestorePage", "BackupsPage", "CloneStickPage",
             "RestoreStickBackupPage", "StickListPage"].indexOf(name) >= 0) {
            return "";
        }
        const path = p.rekordboxPath || p.enginePath || p.pioneerRoot || p.mountPoint || "";
        const i = stormFixture.stickOf(path);
        if (i < 0) {
            return "";
        }
        const tags = stormFixture.shownTags(p).filter(t => t !== "S" + i);
        if (tags.length > 0) {
            return name + " on S" + i + " shows tracks of " + tags.join(", ");
        }
        return "";
    }

    // Calm weather, every held read answered, and within a bound nothing
    // may still say it is busy: a page busy with nothing running is the
    // "stuck scanning" this storm is for.
    function rest(why, then) {
        runner.note("rest: " + why);
        stormFixture.setWeather(0, 0, 0, 0, 0);
        stormFixture.releaseAll();
        let idleSince = -1;
        const started = Date.now();
        const poll = () => {
            if (runner.done) {
                return;
            }
            // A stick pulled out from under a save is back for the rest,
            // so what the save left is looked at by the recovery path.
            const busy = stormFixture.busyNow(runner.window);
            const running = stormFixture.liveWorkers() + stormFixture.activeWrites();
            if (busy.length === 0) {
                if (!runner.checkStep()) {
                    return;
                }
                runner.checkLocks();
                if (!runner.done) {
                    then();
                }
                return;
            }
            if (running === 0) {
                if (idleSince < 0) {
                    idleSince = Date.now();
                } else if (Date.now() - idleSince > 3000) {
                    runner.fail("busy with nothing running for 3 s after " + why + ": " + busy.join(", "));
                    return;
                }
            } else {
                idleSince = -1;
            }
            if (Date.now() - started > 60000) {
                runner.fail("still busy 60 s after " + why + " (" + running + " workers running): "
                            + busy.join(", "));
                return;
            }
            runner.after(50, poll);
        };
        runner.after(50, poll);
    }

    // At rest: no stick's lock is held unless its session still has work
    // staged, and a save a pull interrupted is seen by the recovery path.
    function checkLocks() {
        for (let i = 0; i < runner.stickCount; ++i) {
            if (!stormFixture.plugged(i) || runner.listed(i) === null) {
                continue;
            }
            const s = runner.listed(i);
            const session = EditSessionRegistry.hasSession(s.libraryId)
                ? EditSessionRegistry.sessionFor(s.libraryId, s.label) : null;
            const root = stormFixture.stickRoot(i);
            if (stormFixture.lockHeld(root) && !(session && (session.dirty || session.writing))) {
                runner.fail("at rest, S" + i + "'s write lock is held with nothing staged or writing");
                return;
            }
            // A save the stick was pulled out of, whose record can be
            // restored, has to be something the session can undo. A session
            // that still holds the backups of an earlier save offers that
            // undo instead: a save that lost its stick before changing
            // anything leaves its note behind with nothing to undo, which is
            // stale but harmless (the save changes nothing before its record
            // is whole).
            const notes = stormFixture.interruptedSave(root, true);
            if (notes.length > 0 && session && !session.writing && !session.canUndo) {
                runner.fail("S" + i + " carries a save that never finished (" + notes.join(", ")
                            + "), and its session offers no undo (interruptedSave "
                            + session.interruptedSave + ", dirty " + session.dirty + ")");
                return;
            }
        }
    }

    // ---- the end ----

    function end() {
        runner.note("close the window");
        const started = Date.now();
        runner.window.destroy();
        runner.window = null;
        runner.after(0, () => {
            const took = Date.now() - started;
            if (took > 5000) {
                runner.fail("closing the pages took " + took + " ms");
                return;
            }
            const warnings = stormFixture.takeWarnings().filter(w => runner.warningCounts(w));
            stormFixture.stopCapture();
            if (warnings.length > 0) {
                runner.fail("QML warned while closing:\n  " + warnings.join("\n  "));
                return;
            }
            runner.finish();
        });
    }

    // The quit leg: the window is closed the way a person closes it. A
    // save still running keeps it open (Main.qml), so it is closed again
    // until it goes; staged changes are thrown away when asked.
    function quit() {
        runner.note("quit");
        // From here the parent's bound is the watch: closing may wait for a save.
        stormFixture.disarm();
        stormFixture.log("STORM QUIT LEG WALKED seed " + runner.seed + " to step " + runner.step);
        const started = Date.now();
        let answered = false;
        let pulled = false;
        const tryClose = () => {
            if (!runner.window) {
                return;
            }
            // A save keeps the window open, and a save can be waiting on a
            // stick that does not answer. The person waits, then the stick
            // answers after all, or they pull it: the save ends either way.
            if (!answered && Date.now() - started > 10000) {
                answered = true;
                stormFixture.log("storm: the window is still open 10 s after closing it; every held read answers");
                stormFixture.setWeather(0, 0, 0, 0, 0);
                stormFixture.releaseAll();
            }
            if (!pulled && Date.now() - started > 20000) {
                pulled = true;
                stormFixture.log("storm: the window is still open 20 s after closing it; every stick is pulled");
                for (const i of runner.pluggedSticks()) {
                    stormFixture.pull(i);
                }
            }
            const popups = runner.answerablePopups();
            for (const popup of popups) {
                if (String(popup).indexOf("UnsavedChangesDialog") >= 0 && typeof popup.discardRequested === "function") {
                    popup.discardRequested();
                }
            }
            runner.window.close();
            runner.after(200, tryClose);
        };
        tryClose();
    }
}
