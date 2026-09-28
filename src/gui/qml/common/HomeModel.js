// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// What the home screen's rail and page both need to agree on: the four
// tool groups, and how a row of the stick model is read and keyed. Kept
// here once so HomeRail and StickListPage cannot come to differ.
.pragma library

// The tool groups in the rail's order. AppSettingsController keeps the
// same keys, in this order, in homeGroupKeys to validate the remembered
// group against; tst_HomeRail compares the two, so they cannot drift
// apart unnoticed.
function groups() {
    return [
        {key: "explore", name: "Explore", description: "See what is on the stick", icon: "view-media-track"},
        {key: "sync", name: "Sync", description: "Keep cues and catalogs in step", icon: "exchange-positions"},
        {key: "backup", name: "Backup", description: "Keep a copy on this computer", icon: "backup"},
        {key: "maintain", name: "Maintain", description: "Find and fix what is wrong", icon: "kt-check-data"},
    ];
}
function groupKeys() {
    return groups().map((group) => group.key);
}
// The group with this key; Explore for a key that is none of them.
function group(key) {
    const all = groups();
    for (let i = 0; i < all.length; ++i) {
        if (all[i].key === key) {
            return all[i];
        }
    }
    return all[0];
}

// The stick model, whichever shape it comes in: MediaController's
// DetectedStickListModel or a ListModel (count and get(i)), or a plain
// array of stick objects in the tests (length and indexes).
function rowCount(model) {
    if (model === null || model === undefined) {
        return 0;
    }
    if (model.length !== undefined) {
        return model.length;
    }
    return model.count !== undefined ? model.count : model.rowCount();
}
function rowAt(model, i) {
    return model.length !== undefined ? model[i] : model.get(i);
}
// mountPoint when there is one, else devicePath: the key a stick goes by
// on the rail (railStick:<key>) and in the pane (stickRow:<key>).
function keyOf(row) {
    if (!row) {
        return "";
    }
    const mountPoint = String(row.mountPoint || "");
    return mountPoint.length > 0 ? mountPoint : String(row.devicePath || "");
}
