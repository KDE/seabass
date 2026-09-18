// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

// A row stays where the user last saw it, and a new one arrives where it
// belongs.
//
// The list used to reset itself on every detection pass, which re-sorted
// it: a stick with a readable library sorts above one without, so
// unmounting a stick -- which makes its library unknowable -- moved the
// card out from under the user at the moment they had just acted on it.
// A reset also tells a view only "everything changed", so an arriving
// stick could not be animated into place; there was nothing to animate
// from.
//
// What this pins, in the model rather than through a view:
//   - the first detection sorts (libraries first);
//   - a stick that unmounts keeps its row;
//   - a stick that gains a library keeps its row rather than jumping up;
//   - a newly plugged stick is INSERTED, at the place the sort gives it,
//     and the model says so as an insert rather than a reset;
//   - a removed stick is reported as a remove.
// The last two are what an animation needs; without them a view can only
// redraw.

#include <QCoreApplication>
#include <QSignalSpy>

#include <cassert>
#include <iostream>
#include <string>
#include <vector>

#include "application/ports/removable_media_locator.hpp"
#include "gui/media_controller.hpp"

using seabass::application::DetectedStick;
using seabass::gui::DetectedStickListModel;

namespace
{

int failures = 0;

void check(bool ok, const std::string &what)
{
    std::cout << (ok ? "  ok   " : "  FAIL ") << what << "\n";
    if (!ok) {
        ++failures;
    }
}

DetectedStick stick(const std::string &device, const std::string &label, bool mounted, bool withLibrary)
{
    DetectedStick s;
    s.devicePath = device;
    s.label = label;
    s.mounted = mounted;
    s.mountPoint = mounted ? "/media/test/" + label : std::string();
    if (withLibrary && mounted) {
        s.rekordboxPath = s.mountPoint + "/PIONEER";
    }
    s.identity.label = label;
    return s;
}

std::vector<std::string> labels(const DetectedStickListModel &model)
{
    std::vector<std::string> out;
    for (const DetectedStick &s : model.sticks()) {
        out.push_back(s.label);
    }
    return out;
}

std::string joined(const std::vector<std::string> &values)
{
    std::string out;
    for (const std::string &value : values) {
        out += (out.empty() ? "" : ", ") + value;
    }
    return "[" + out + "]";
}

}  // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    DetectedStickListModel model;

    // --- the first pass sorts: a readable library first ---------------
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, false), stick("/dev/sdc1", "GIGSTICK", true, true)});
    check(labels(model) == std::vector<std::string>{"GIGSTICK", "PLAIN"},
          "the first detection sorts libraries first, was " + joined(labels(model)));

    // --- unmounting must not move the row -----------------------------
    QSignalSpy resets(&model, &DetectedStickListModel::modelReset);
    QSignalSpy inserts(&model, &DetectedStickListModel::rowsInserted);
    QSignalSpy removes(&model, &DetectedStickListModel::rowsRemoved);

    // GIGSTICK unmounts: its library is now unknowable, which is exactly
    // what the sort would demote it for.
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, false), stick("/dev/sdc1", "GIGSTICK", false, false)});
    check(labels(model) == std::vector<std::string>{"GIGSTICK", "PLAIN"},
          "an unmounted stick keeps its row, was " + joined(labels(model)));
    check(resets.count() == 0, "and the model does not reset to say so");
    check(inserts.count() == 0 && removes.count() == 0, "nothing was inserted or removed for a change of state");

    // --- and mounting again does not promote it either -----------------
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, false), stick("/dev/sdc1", "GIGSTICK", true, true)});
    check(labels(model) == std::vector<std::string>{"GIGSTICK", "PLAIN"},
          "a stick that regains its library stays put, was " + joined(labels(model)));

    // --- a new stick arrives, and arrives as an insert -----------------
    inserts.clear();
    resets.clear();
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, false), stick("/dev/sdc1", "GIGSTICK", true, true),
                     stick("/dev/sdd1", "NEWONE", true, true)});
    check(inserts.count() == 1, "a newly plugged stick is reported as an insert, not a reset");
    check(resets.count() == 0, "and still no reset");
    // It has a library, so it belongs above PLAIN; it must not simply be
    // appended, and it must not displace the stick already at the top.
    check(labels(model) == std::vector<std::string>{"GIGSTICK", "NEWONE", "PLAIN"},
          "the new stick lands where the sort puts it, was " + joined(labels(model)));
    if (inserts.count() == 1) {
        const int first = inserts.at(0).at(1).toInt();
        check(first == 1, "and the view is told which row it was, for the animation to start from");
    }

    // --- a stick pulled out is reported as a remove --------------------
    removes.clear();
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, false), stick("/dev/sdd1", "NEWONE", true, true)});
    check(removes.count() == 1, "a pulled stick is reported as a remove");
    check(labels(model) == std::vector<std::string>{"NEWONE", "PLAIN"},
          "and the rest keep their order, was " + joined(labels(model)));

    // --- a change of data on a row is a change, not a rebuild ----------
    QSignalSpy changes(&model, &DetectedStickListModel::dataChanged);
    model.setSticks({stick("/dev/sdb1", "PLAIN", true, true), stick("/dev/sdd1", "NEWONE", true, true)});
    check(changes.count() == 1, "a row whose library appeared reports dataChanged for itself alone");
    check(labels(model) == std::vector<std::string>{"NEWONE", "PLAIN"},
          "and does not jump to the top for it, was " + joined(labels(model)));

    if (failures > 0) {
        std::cout << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "all passed\n";
    return 0;
}
