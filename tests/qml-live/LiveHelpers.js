// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// Shared by the live tests in this directory (see docs/testing.md,
// "Live tests against a real stick") and, for the stick list's rows and
// cards, by tests/qml/tst_StickListPage.qml. Plain JS: TestCase functions
// are not shareable across tst_*.qml files.
.pragma library

// The first QObject under `root` whose C++ class name starts with
// `typeName` -- the way a test reaches the controller a page created for
// itself (SettingsPage's SettingsController, ...). QML prints a C++
// object as "ClassName(0x...)", which is what this keys on.
function findByType(root, typeName) {
    // C++ types print namespaced ("seabass::gui::SettingsController(0x..)"),
    // QML component types with a suffix ("BackBreadcrumb_QMLTYPE_12(0x..)").
    // Each String(obj) runs QMetaObject::indexOfMethod over the type's
    // whole method table, which is why find() visits every object once:
    // measured with eu-stack and perf on 2026-09-16, a walk that re-entered
    // subtrees sat at 100% CPU for nine minutes on an ordinary page.
    return find(root, function(obj) {
        var name = String(obj);
        return name.indexOf(typeName + "(") === 0 || name.indexOf("::" + typeName + "(") >= 0
            || name.indexOf(typeName + "_QMLTYPE_") === 0;
    });
}

function summaryLine(summary) {
    return summary.written + " of " + summary.total + " " + summary.unit + " " + (summary.verb || "written")
        + (summary.cancelled ? " (cancelled)" : "") + (summary.error ? " error: " + summary.error : "")
        + (summary.warning ? " warning: " + summary.warning : "");
}

// The objects one step under an item, the way the page's object graph
// really hangs together: children and resources reach the same objects
// by different paths, and a Control's contentItem, header and footer are
// not reliably among its children. Every finder here walks this one
// list, with a visited set so no subtree is entered twice.
function under(item) {
    var kids = [];
    // A Page/Dialog puts declared objects into contentData, an Item into
    // data/resources/children; the lists overlap, and find()'s visited
    // set is what keeps the overlap from costing a second walk.
    var lists = [item.contentData, item.resources, item.children, item.data];
    for (var l = 0; l < lists.length; ++l) {
        var list = lists[l];
        if (list === undefined || list === null) continue;
        for (var i = 0; i < list.length; ++i) kids.push(list[i]);
    }
    if (item.contentItem) kids.push(item.contentItem);
    if (item.footer) kids.push(item.footer);
    if (item.header) kids.push(item.header);
    return kids;
}
// The first object under root (root included) the predicate accepts.
function find(root, accept) {
    var seen = new Set();
    function walk(item) {
        if (!item || seen.has(item)) return null;
        seen.add(item);
        if (accept(item)) return item;
        var kids = under(item);
        for (var k = 0; k < kids.length; ++k) {
            var found = walk(kids[k]);
            if (found) return found;
        }
        return null;
    }
    return walk(root);
}
// Every object under root the predicate accepts, in no particular order
// (a ListView builds its rows in refill order, not model order); the
// walk does not descend into an accepted object.
function findAll(root, accept) {
    var seen = new Set();
    var out = [];
    function walk(item) {
        if (!item || seen.has(item)) return;
        seen.add(item);
        if (accept(item)) { out.push(item); return; }
        var kids = under(item);
        for (var k = 0; k < kids.length; ++k) walk(kids[k]);
    }
    walk(root);
    return out;
}
// The item with this objectName anywhere under root -- the dialogs the
// pages own are reached this way (tst_LiveQuit's F5).
function findByObjectName(root, name) {
    return find(root, function(item) { return item.objectName === name; });
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

// StickListPage's list and rows, by the objectNames the page gives them
// ("stickList", "stickRow:<mount point>" -- or the device path for a
// stick that is not mounted), and a row's cards by their title, the
// ActionCard's own cardTitle: tst_StickListPage and tst_LiveLock share
// these, so one place knows how the page is built.
function stickList(page) {
    return findByObjectName(page, "stickList");
}
function stickRow(page, mountPointOrDevice) {
    return findByObjectName(page, "stickRow:" + mountPointOrDevice);
}
// Every row the list has built -- a ListView instantiates only the rows
// near its viewport -- in no particular order.
function stickRows(page) {
    return findAll(page, function(item) {
        return typeof item.objectName === "string" && item.objectName.indexOf("stickRow:") === 0;
    });
}
// An ActionCard by its exact title under a row.
function cardIn(row, title) {
    return find(row, function(item) { return item.cardTitle !== undefined && String(item.cardTitle) === title; });
}
function cardInRow(page, mountPointOrDevice, title) {
    var row = stickRow(page, mountPointOrDevice);
    return row ? cardIn(row, title) : null;
}
// A row's own control (eject, close) by objectName.
function objectInRow(page, mountPointOrDevice, objectName) {
    var row = stickRow(page, mountPointOrDevice);
    return row ? findByObjectName(row, objectName) : null;
}
