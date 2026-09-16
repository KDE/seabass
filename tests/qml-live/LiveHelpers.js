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
    // Every object visited once. A QML item graph reaches the same object
    // through several of the lists below -- `children` and `data` overlap by
    // construction, and `contentItem`'s subtree is also in `contentData` --
    // so without this the walk re-enters whole subtrees once per path that
    // reaches them, and each visit pays a String(obj) that runs
    // QMetaObject::indexOfMethod over the type's whole method table.
    // Measured with eu-stack and perf on 2026-09-16: a live test sat at 100%
    // CPU for nine minutes inside objectToString/methodMatch/memcmp with the
    // GC chasing the string garbage, on a page whose graph is perfectly
    // ordinary. It was 76 s when the page was smaller.
    var seen = new Set();
    function beenHere(obj) {
        if (seen.has(obj)) return true;
        seen.add(obj);
        return false;
    }
    function walk(obj) {
        if (found !== null || obj === null || obj === undefined) return;
        if (beenHere(obj)) return;
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
        + (summary.cancelled ? " (cancelled)" : "") + (summary.error ? " error: " + summary.error : "")
        + (summary.warning ? " warning: " + summary.warning : "");
}

// The item with this objectName anywhere under root -- the dialogs the
// pages own are reached this way (tst_LiveQuit's F5).
function findByObjectName(root, name, seen) {
    if (!root) return null;
    // Same overlapping-graph problem findByType has: children and resources
    // reach the same objects, so without this the walk re-enters subtrees
    // once per path into them.
    seen = seen || new Set();
    if (seen.has(root)) return null;
    seen.add(root);
    if (root.objectName === name) return root;
    var kids = [];
    if (root.contentItem) kids.push(root.contentItem);
    if (root.footer) kids.push(root.footer);
    if (root.header) kids.push(root.header);  // findByType looks here; this did not
    var children = root.children ? root.children : [];
    for (var i = 0; i < children.length; ++i) kids.push(children[i]);
    var resources = root.resources ? root.resources : [];
    for (var r = 0; r < resources.length; ++r) kids.push(resources[r]);
    for (var k = 0; k < kids.length; ++k) {
        var found = findByObjectName(kids[k], name, seen);
        if (found) return found;
    }
    return null;
}

// Whether a file is still on the stick, for a check that deletes for good
// (tst_LiveEditMode's W8). XMLHttpRequest is the only file probe QML has
// without a helper type. HEAD, not GET: the answer is whether the file is
// there, and a GET would read every surviving file into memory to find out.
//
// An empty file exists: judging by the response body called a truncated
// file deleted, which is precisely the difference W8 is asking about. The
// status is what decides, and reading it can throw for a path that is not
// there -- so every step sits inside the try, where a throw is the answer.
function fileExists(absolutePath) {
    try {
        var request = new XMLHttpRequest();
        request.open("HEAD", "file://" + absolutePath, false);
        request.send(null);
        // Local files answer 0 (no HTTP status) when they opened, 404 when
        // the path is not there; some builds report 200 instead.
        return request.status === 0 || request.status === 200;
    } catch (e) {
        return false;
    }
}
