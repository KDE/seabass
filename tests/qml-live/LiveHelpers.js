// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Shared by the live tests in this directory (see docs/testing.md,
// "Live tests against a real stick"). Plain JS: TestCase functions are
// not shareable across tst_*.qml files.
.pragma library

// The first QObject under `root` whose C++ class name starts with
// `typeName` -- the way a test reaches the controller a page created for
// itself (SettingsPage's SettingsController, ...). QML prints a C++
// object as "ClassName(0x...)", which is what this keys on.
function findByType(root, typeName) {
    var found = null;
    function walk(obj) {
        if (found !== null || obj === null || obj === undefined) return;
        // C++ types print namespaced ("seabass::gui::SettingsController(0x..)"),
        // QML component types with a suffix ("BackBreadcrumb_QMLTYPE_12(0x..)").
        var name = String(obj);
        if (name.indexOf(typeName + "(") === 0 || name.indexOf("::" + typeName + "(") >= 0
            || name.indexOf(typeName + "_QMLTYPE_") === 0) {
            found = obj;
            return;
        }
        // A Page/Dialog puts declared objects into contentData, an Item
        // into data/resources/children.
        var lists = [obj.contentData, obj.resources, obj.children, obj.data];
        for (var l = 0; l < lists.length && found === null; ++l) {
            var list = lists[l];
            if (list === undefined || list === null) continue;
            for (var i = 0; i < list.length && found === null; ++i) {
                walk(list[i]);
            }
        }
        if (found === null && obj.contentItem !== undefined && obj.contentItem !== null) walk(obj.contentItem);
        if (found === null && obj.footer !== undefined && obj.footer !== null) walk(obj.footer);
        if (found === null && obj.header !== undefined && obj.header !== null) walk(obj.header);
    }
    walk(root);
    return found;
}

function summaryLine(summary) {
    return summary.written + " of " + summary.total + " " + summary.unit + " " + (summary.verb || "written")
        + (summary.cancelled ? " (cancelled)" : "") + (summary.error ? " error: " + summary.error : "");
}
