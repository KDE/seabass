// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

import QtQuick
import QtQuick.Controls
import QtTest
import SeabassGui

// Exercises FormatUsbPage.qml entirely headless -- no real disk, no real
// udisks2/PowerShell call, no real RemovableMediaLocator result. Possible
// because FormatUsbController is an untyped, required page property (see
// FormatUsbPage.qml's own comment) rather than a self-instantiated real
// controller: every test here hands the page a plain JS object standing
// in for the whole controller, the same technique tst_TrackDetailPage.qml
// already uses for its own controllers.
TestCase {
    id: testCase
    name: "FormatUsbPage"
    width: 480
    height: 800
    visible: true
    when: windowShown

    // Explicit size, not left to Page's own implicit-width computation --
    // pushed for real inside a StackView, the surrounding window always
    // constrains Page's width immediately; here, with nothing external
    // doing that, ScrollView's contentWidth: availableWidth and its
    // ColumnLayout's width: parent.width feed back on each other (a real
    // "Binding loop detected for property implicitWidth" warning at
    // runtime) and leave geometry degenerate enough that mouseClick can't
    // reliably hit real content.
    Component {
        id: pageComponent
        FormatUsbPage { width: 460; height: 700 }
    }

    function makeFakeController(disks, fat32MaxBytes) {
        return {
            disks: disks,
            busy: false,
            errorMessage: "",
            statusMessage: "",
            fat32MaxBytes: fat32MaxBytes === undefined ? -1 : fat32MaxBytes,
            lastFormatCall: null,
            recommendedFilesystem: function(capacityBytes) {
                var threshold = 32 * 1024 * 1024 * 1024;
                return capacityBytes <= threshold ? "fat32" : "exfat";
            },
            format: function(wholeDiskPath, filesystem, volumeLabel) {
                this.lastFormatCall = {wholeDiskPath: wholeDiskPath, filesystem: filesystem, volumeLabel: volumeLabel};
            },
        };
    }

    function makeDisk(overrides) {
        var disk = {
            label: "TESTSTICK",
            wholeDiskPath: "/dev/sdx",
            devicePath: "",
            capacityBytes: 8 * 1024 * 1024 * 1024,
            mounted: false,
            hasNoFilesystem: true,
            hasDjLibrary: false,
            rootEntries: [],
        };
        for (var key in overrides) {
            disk[key] = overrides[key];
        }
        return disk;
    }

    function test_instantiatesWithNoDisks() {
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([])});
        verify(page !== null);
        compare(page.selectedIndex, -1);
        verify(page.selectedDisk === null);
    }

    // Formatting is destructive enough that this page never guesses on
    // anyone's behalf -- not a blank drive, not the only drive, not one
    // this page was opened from. Nothing is selected until the person
    // clicks a drive themselves (see applySelection() calls throughout
    // the rest of this file for that explicit step).
    function test_nothingPreselectedWithMixOfBlankAndExisting() {
        var existing = makeDisk({label: "WHALESHARK2", hasNoFilesystem: false, hasDjLibrary: true,
                                   capacityBytes: 32 * 1024 * 1024 * 1024, rootEntries: ["PIONEER/", "Engine Library/"]});
        var blank = makeDisk({label: "SanDisk Extreme 256GB", hasNoFilesystem: true,
                                capacityBytes: 256 * 1024 * 1024 * 1024});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([existing, blank])});
        verify(page !== null);
        compare(page.selectedIndex, -1);
        verify(page.selectedDisk === null);
    }

    // Reproduces the real-world overflow report: a stick whose rootEntries
    // join into a very long string (the actual WHALESHARK2 example that
    // triggered this) must not force any RadioButton wider than the page
    // itself. wrapMode/elide only change how text *renders* once a width
    // is imposed -- they don't change a Label's own implicitWidth, which
    // Qt Quick Layouts otherwise treats as an unshrinkable floor for
    // Layout.fillWidth unless Layout.minimumWidth is set to override it.
    function test_longStringsDoNotOverflowNarrowPage() {
        var longDisk = makeDisk({
            label: "WHALESHARK2",
            wholeDiskPath: "/dev/sdb",
            hasNoFilesystem: false,
            hasDjLibrary: true,
            rootEntries: ["System Volume Information", "PIONEER", "Contents", "PIONEER REC",
                "Engine Library", "Sessions", ".djconvert-backups", ".djconvert.log"],
            capacityBytes: 126 * 1024 * 1024 * 1024,
        });
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([longDisk])});
        verify(page !== null);
        page.applySelection(0);
        // A multi-level nested Layout chain (Page > ScrollView >
        // ColumnLayout > GroupBox > ColumnLayout > RadioButton) needs a
        // couple of polish/relayout cycles to fully converge after a
        // synchronous property change -- checking geometry in the very
        // same tick it changed can catch it mid-settle.
        wait(50);

        var driveRadio = findChild(page, "driveRadio");
        var fat32Radio = findChild(page, "fat32Radio");
        var exfatRadio = findChild(page, "exfatRadio");
        var driveSubtitleLabel = findChild(page, "driveSubtitleLabel");
        var fat32Label = findChild(page, "fat32Label");
        var exfatLabel = findChild(page, "exfatLabel");
        verify(driveRadio !== null);
        verify(fat32Radio !== null);
        verify(exfatRadio !== null);
        verify(driveSubtitleLabel !== null);
        verify(fat32Label !== null);
        verify(exfatLabel !== null);

        // page.width (460, fixed by pageComponent above) is the hard
        // ceiling every one of these controls must fit inside. Any of
        // them wider than that means an implicit-width floor is refusing
        // to let a wrapping/eliding Label shrink -- the exact bug
        // reported twice on this page already.
        verify(driveRadio.width <= page.width);
        verify(fat32Radio.width <= page.width);
        verify(exfatRadio.width <= page.width);

        // The stronger check: a Label's own .width can look correct while
        // its *rendered* text (paintedWidth) still spills out past it --
        // wrap/elide only take effect once a definite width is actually
        // applied in time, and Items don't clip their children by default,
        // so overflowing text paints right over whatever sits next to it
        // (here: the StatusBadge/GB label to the right of the drive row's
        // subtitle) rather than being visibly truncated or wrapped.
        verify(driveSubtitleLabel.paintedWidth <= driveSubtitleLabel.width + 1);
        verify(fat32Label.paintedWidth <= fat32Label.width + 1);
        verify(exfatLabel.paintedWidth <= exfatLabel.width + 1);
    }

    // Same check, but with a small (<=32GB) drive so FAT32 -- not exFAT --
    // is the one that gets the longer "  (Recommended)" suffix, covering
    // the other direction of the same paintedWidth check above.
    function test_recommendedFat32LabelDoesNotOverflowNarrowPage() {
        var smallDisk = makeDisk({label: "SMALLSTICK", capacityBytes: 8 * 1024 * 1024 * 1024});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([smallDisk])});
        verify(page !== null);
        page.applySelection(0);
        compare(page.recommendedFilesystem, "fat32");
        wait(50);  // see test_longStringsDoNotOverflowNarrowPage's own comment

        var fat32Label = findChild(page, "fat32Label");
        verify(fat32Label !== null);
        verify(fat32Label.paintedWidth <= fat32Label.width + 1);
    }

    function test_nothingPreselectedWhenNoneAreBlank() {
        var a = makeDisk({label: "A", hasNoFilesystem: false});
        var b = makeDisk({label: "B", hasNoFilesystem: false});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([a, b])});
        verify(page !== null);
        compare(page.selectedIndex, -1);
    }

    // Selection is tracked by the disk's own wholeDiskPath, not a raw
    // index -- if the disks list is rebuilt (a hotplug refresh) and the
    // previously-picked disk is no longer in it, the selection must
    // clear rather than have selectedIndex silently keep pointing at
    // whatever now happens to sit at that same numeric position.
    function test_selectionClearsWhenChosenDiskDisappears() {
        var a = makeDisk({label: "A", wholeDiskPath: "/dev/sda"});
        var b = makeDisk({label: "B", wholeDiskPath: "/dev/sdb"});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([a, b])});
        verify(page !== null);

        page.applySelection(1);
        compare(page.selectedDisk.label, "B");

        // Reassigns page.controller wholesale (a real, notifiable QML
        // property) rather than mutating .disks on the existing plain JS
        // controller object in place -- a plain object's own property
        // mutation isn't reactive the way a genuine Q_PROPERTY's
        // disksChanged is on the real controller, so it wouldn't
        // propagate through root.disks here. "B" (the chosen disk) is
        // gone; "C" now occupies index 1 -- exactly the reordering
        // scenario a plain index would get wrong.
        page.controller = makeFakeController([a, makeDisk({label: "C", wholeDiskPath: "/dev/sdc"})]);
        compare(page.selectedIndex, -1);
        verify(page.selectedDisk === null);
    }

    // The same disk staying present (even if its own list position
    // shifts) keeps the selection intact, re-resolved by path rather
    // than lost just because something else changed.
    function test_selectionSurvivesReorderingByPath() {
        var a = makeDisk({label: "A", wholeDiskPath: "/dev/sda"});
        var b = makeDisk({label: "B", wholeDiskPath: "/dev/sdb"});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([a, b])});
        verify(page !== null);

        page.applySelection(1);
        compare(page.selectedDisk.label, "B");

        page.controller = makeFakeController([b, a]);  // "B" now first
        compare(page.selectedIndex, 0);
        compare(page.selectedDisk.label, "B");
    }

    // The size-based recommendation (<=32GB -> FAT32, >32GB -> exFAT)
    // tracks whichever drive is currently selected.
    function test_recommendedFilesystemTracksSelectedDriveSize() {
        // Distinct wholeDiskPath per disk matters now: selection is
        // tracked by path (see applySelection()'s own comment), so two
        // disks sharing makeDisk()'s default path would be
        // indistinguishable from each other.
        var small = makeDisk({label: "Small", wholeDiskPath: "/dev/sda", capacityBytes: 8 * 1024 * 1024 * 1024});
        var large = makeDisk({label: "Large", wholeDiskPath: "/dev/sdb", capacityBytes: 256 * 1024 * 1024 * 1024});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([small, large])});
        verify(page !== null);

        page.applySelection(0);
        compare(page.selectedFilesystem, "fat32");
        compare(page.recommendedFilesystem, "fat32");

        page.applySelection(1);
        compare(page.selectedFilesystem, "exfat");
        compare(page.recommendedFilesystem, "exfat");
    }

    // A platform-imposed FAT32 ceiling (Windows' real 32GB limit) is
    // exposed as a plain controller property the fake sets directly,
    // rather than the page querying the real host OS -- makes this
    // branch testable on any machine, Windows or not.
    function test_fat32UnavailableAboveThePlatformCeiling() {
        var large = makeDisk({label: "Large", capacityBytes: 64 * 1024 * 1024 * 1024});
        var thirtyTwoGb = 32 * 1024 * 1024 * 1024;
        var page = createTemporaryObject(pageComponent, testCase,
            {controller: makeFakeController([large], thirtyTwoGb)});
        verify(page !== null);
        page.applySelection(0);
        compare(page.fat32Available, false);
    }

    function test_fat32AvailableWithNoPlatformCeiling() {
        var large = makeDisk({label: "Large", capacityBytes: 64 * 1024 * 1024 * 1024});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([large], -1)});
        verify(page !== null);
        page.applySelection(0);
        compare(page.fat32Available, true);
    }

    // The confirm dialog's Format button stays disabled until the typed
    // text exactly matches the label about to be written, and enables the
    // moment it does.
    function test_confirmButtonGatedByExactLabelMatch() {
        var disk = makeDisk({label: "TESTSTICK", hasNoFilesystem: false});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);
        page.applySelection(0);

        var openButton = findChild(page, "openConfirmButton");
        var confirmField = findChild(page, "confirmField");
        var acceptButton = findChild(page, "formatAcceptButton");
        var confirmDialog = findChild(page, "confirmDialog");
        verify(openButton !== null);
        verify(confirmField !== null);
        verify(acceptButton !== null);
        verify(confirmDialog !== null);
        compare(openButton.enabled, true);

        // Opens the dialog via its own API rather than mouseClick(openButton)
        // -- this page's real content is taller than any reasonable
        // offscreen test window, so a screen-position click on a button
        // below the fold of a ScrollView isn't a reliable way to test
        // dialog logic headless. RadioButton/TextField interaction
        // elsewhere in this file still uses real click/text simulation;
        // this is specifically about reaching a control that scrolling
        // would otherwise have to bring into view first.
        confirmDialog.open();
        tryVerify(function() { return confirmDialog.visible; });
        compare(acceptButton.enabled, false);

        confirmField.text = "wrong name";
        compare(acceptButton.enabled, false);

        confirmField.text = "TESTSTICK";
        compare(acceptButton.enabled, true);
    }

    // Going through the confirm flow calls the controller's format() with
    // exactly the expected (wholeDiskPath, filesystem, label) args --
    // proves the wiring without ever touching a real device. Uses
    // confirmDialog.accept() rather than mouseClick(acceptButton) for the
    // same off-screen-content reason as the test above; the button's own
    // enabled-gating is already verified separately.
    function test_formatCallsControllerWithExactArgs() {
        var disk = makeDisk({label: "TESTSTICK", wholeDiskPath: "/dev/sdz", hasNoFilesystem: false,
                               capacityBytes: 8 * 1024 * 1024 * 1024});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);
        page.applySelection(0);
        // Read state back through page.controller throughout, never the
        // outer object literal passed to createTemporaryObject -- a `var`
        // page property doesn't keep reference identity with a plain JS
        // object handed in via createTemporaryObject()'s initial
        // properties (verified: `page.controller === <the outer object>`
        // is false), so the only reliable way to observe what the page
        // did to "its" controller is through the page's own reference.
        // Overrides applySelection()'s own size-based recommendation
        // (8GB recommends fat32) to prove the explicit choice, not the
        // recommendation, is what actually gets passed to format().
        page.selectedFilesystem = "exfat";

        var confirmField = findChild(page, "confirmField");
        var acceptButton = findChild(page, "formatAcceptButton");
        var confirmDialog = findChild(page, "confirmDialog");
        verify(confirmDialog !== null);

        confirmDialog.open();
        tryVerify(function() { return confirmDialog.visible; });
        confirmField.text = "TESTSTICK";
        compare(acceptButton.enabled, true);
        confirmDialog.accept();

        verify(page.controller.lastFormatCall !== null);
        compare(page.controller.lastFormatCall.wholeDiskPath, "/dev/sdz");
        compare(page.controller.lastFormatCall.filesystem, "exfat");
        compare(page.controller.lastFormatCall.volumeLabel, "TESTSTICK");
    }

    // Nothing is selected at construction (see the "nothing preselected"
    // tests above), so the warning can't have fired yet even for a
    // DJ-library disk -- it only ever shows in response to an actual,
    // explicit selection.
    function test_djLibraryWarningNotShownBeforeAnySelection() {
        var disk = makeDisk({label: "WHALESHARK2", hasNoFilesystem: false, hasDjLibrary: true});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var djWarning = findChild(page, "djLibraryWarningDialog");
        verify(djWarning !== null);
        compare(djWarning.visible, false);
    }

    // Explicitly selecting a drive with a recognized DJ library surfaces
    // the warning dialog.
    function test_djLibraryWarningShownOnExplicitSelection() {
        var disk = makeDisk({label: "WHALESHARK2", hasNoFilesystem: false, hasDjLibrary: true});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var djWarning = findChild(page, "djLibraryWarningDialog");
        verify(djWarning !== null);
        page.applySelection(0);
        tryVerify(function() { return djWarning.visible; });
    }

    // A blank or already-in-use-but-non-DJ drive never needs this warning,
    // even once explicitly selected.
    function test_djLibraryWarningNotShownForOrdinaryDrive() {
        var disk = makeDisk({label: "Blank", hasNoFilesystem: true, hasDjLibrary: false});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var djWarning = findChild(page, "djLibraryWarningDialog");
        verify(djWarning !== null);
        page.applySelection(0);
        compare(djWarning.visible, false);
    }

    // Its own button is the only way out -- Escape/click-outside must not
    // silently dismiss a warning about real, existing DJ data.
    function test_djLibraryWarningRequiresExplicitAcknowledgement() {
        var disk = makeDisk({label: "WHALESHARK2", hasNoFilesystem: false, hasDjLibrary: true});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var djWarning = findChild(page, "djLibraryWarningDialog");
        var acknowledgeButton = findChild(page, "djLibraryWarningAcknowledgeButton");
        verify(djWarning !== null);
        verify(acknowledgeButton !== null);
        page.applySelection(0);
        tryVerify(function() { return djWarning.visible; });
        compare(djWarning.closePolicy, Popup.NoAutoClose);

        acknowledgeButton.clicked();
        tryVerify(function() { return !djWarning.visible; });
    }

    // Re-selecting a DJ-library drive after having selected something else
    // shows the warning again -- it's not a one-shot "seen it already"
    // flag, since forgetting between two picks is exactly the mistake
    // this exists to prevent.
    function test_djLibraryWarningShownAgainOnReselection() {
        var djDisk = makeDisk({label: "WHALESHARK2", hasNoFilesystem: false, hasDjLibrary: true});
        var blankDisk = makeDisk({label: "Blank", wholeDiskPath: "/dev/sdy", hasNoFilesystem: true});
        var page = createTemporaryObject(pageComponent, testCase,
            {controller: makeFakeController([djDisk, blankDisk])});
        verify(page !== null);
        compare(page.selectedIndex, -1);

        var djWarning = findChild(page, "djLibraryWarningDialog");
        var acknowledgeButton = findChild(page, "djLibraryWarningAcknowledgeButton");
        verify(djWarning !== null);
        compare(djWarning.visible, false);

        page.applySelection(0);
        tryVerify(function() { return djWarning.visible; });
        acknowledgeButton.clicked();
        tryVerify(function() { return !djWarning.visible; });

        page.applySelection(1);
        page.applySelection(0);
        tryVerify(function() { return djWarning.visible; });
    }

    // A drive that already has real files but no recognized DJ library
    // gets an explicit data-loss badge, not the same "safe" styling as a
    // genuinely blank drive.
    function test_dataWillBeLostBadgeForNonBlankNonDjDrive() {
        var disk = makeDisk({label: "OLDBACKUP", hasNoFilesystem: false, hasDjLibrary: false});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var badge = findChild(page, "driveStatusBadge");
        verify(badge !== null);
        compare(badge.label, "Data will be lost");
    }

    function test_blankDriveBadgeStaysBlank() {
        var disk = makeDisk({label: "FreshStick", hasNoFilesystem: true});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var badge = findChild(page, "driveStatusBadge");
        verify(badge !== null);
        compare(badge.label, "Blank");
    }

    // The final confirm dialog names the exact device (label + path), not
    // just a generic warning -- the "double check you picked the right
    // one" ask needs the actual identifying details in front of the
    // person, not just prose.
    function test_confirmDialogNamesTheSelectedDevice() {
        var disk = makeDisk({label: "WHALESHARK2", wholeDiskPath: "/dev/sdb", hasNoFilesystem: false});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);
        page.applySelection(0);

        var confirmDialog = findChild(page, "confirmDialog");
        var dataLossLabel = findChild(page, "confirmDialogDataLossLabel");
        var doubleCheckLabel = findChild(page, "confirmDialogDoubleCheckLabel");
        verify(confirmDialog !== null);
        verify(dataLossLabel !== null);
        verify(doubleCheckLabel !== null);
        confirmDialog.open();
        tryVerify(function() { return confirmDialog.visible; });

        verify(dataLossLabel.text.indexOf("WHALESHARK2") >= 0);
        verify(dataLossLabel.text.indexOf("/dev/sdb") >= 0);
        verify(dataLossLabel.text.toLowerCase().indexOf("permanently") >= 0);
        verify(doubleCheckLabel.text.toLowerCase().indexOf("double-check") >= 0);
        verify(doubleCheckLabel.text.toLowerCase().indexOf("correct storage device") >= 0);
    }

    // "2. Choose a format" and "3. Name it" stay visible (so the person
    // can see what's coming) but disabled until a drive is picked --
    // greyed out, not hidden and re-flowing the page every time
    // selection changes.
    function test_formatAndNameSectionsGreyedOutNotHiddenBeforeSelection() {
        var disk = makeDisk({label: "TESTSTICK"});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);

        var formatGroupBox = findChild(page, "formatGroupBox");
        var nameGroupBox = findChild(page, "nameGroupBox");
        verify(formatGroupBox !== null);
        verify(nameGroupBox !== null);
        compare(formatGroupBox.visible, true);
        compare(formatGroupBox.enabled, false);
        compare(nameGroupBox.visible, true);
        compare(nameGroupBox.enabled, false);

        page.applySelection(0);
        compare(formatGroupBox.enabled, true);
        compare(nameGroupBox.enabled, true);
    }

    // Clicking a single drive selects it, then clicking that same
    // already-selected drive again unselects it -- the one way this page
    // lets someone back out of a drive they've picked without having to
    // pick a *different* one instead. A single-disk model sidesteps any
    // ambiguity over which delegate findChild() would return with more
    // than one driveRadio in the tree.
    function test_clickingDriveTogglesSelection() {
        var disk = makeDisk({label: "TESTSTICK"});
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);
        compare(page.selectedIndex, -1);

        var driveRadio = findChild(page, "driveRadio");
        verify(driveRadio !== null);

        driveRadio.clicked();
        compare(page.selectedIndex, 0);
        compare(page.selectedDisk.label, "TESTSTICK");

        driveRadio.clicked();
        compare(page.selectedIndex, -1);
        verify(page.selectedDisk === null);
    }
    // --- Warning strength vs. what is actually on the drive ---------
    //
    // The confirm dialog used to claim "All data ... will be lost
    // permanently" on every drive, including one it had just described as
    // "Contains: empty" three rows further down. A warning that fires when
    // there is nothing to lose is the fastest way to teach someone to
    // click past the one that matters.

    // Opens the confirm dialog for real. Everything asserted below lives
    // inside it, and a closed Dialog's contents are invisible regardless
    // of their own bindings -- checking `visible` without this reports
    // false for every case and passes the "warning is hidden" tests for
    // entirely the wrong reason. (It did, on the first run of this file.)
    function openConfirmFor(disk) {
        var page = createTemporaryObject(pageComponent, testCase, {controller: makeFakeController([disk])});
        verify(page !== null);
        page.applySelection(0);
        wait(50);
        var confirmDialog = findChild(page, "confirmDialog");
        verify(confirmDialog !== null);
        confirmDialog.open();
        tryVerify(function() { return confirmDialog.visible; });
        return page;
    }

    // A blank, unpartitioned drive has nothing on it by definition.
    function test_blankDriveIsNotWarnedAboutAsDataLoss() {
        var page = openConfirmFor(makeDisk({hasNoFilesystem: true, mounted: false, rootEntries: []}));
        compare(page.nothingToLose, true);
        compare(findChild(page, "confirmDialogDataLossLabel").visible, false);
        verify(findChild(page, "confirmDialogHeadlineLabel").text.indexOf("empty") >= 0);
    }

    // A mounted drive with an empty root: formatted, but holding nothing.
    function test_mountedButEmptyDriveIsNotWarnedAboutAsDataLoss() {
        var page = openConfirmFor(makeDisk({hasNoFilesystem: false, mounted: true, rootEntries: []}));
        compare(page.nothingToLose, true);
        compare(findChild(page, "confirmDialogDataLossLabel").visible, false);
    }

    // The one that matters. rootEntries is only populated for a mounted
    // drive, so an unmounted drive always has an empty list -- and that
    // means "contents unknown", not "empty". Softening here would disarm
    // the warning on precisely the drives Seabass knows least about.
    function test_unmountedDriveKeepsTheFullWarningDespiteEmptyRootEntries() {
        var page = openConfirmFor(makeDisk({hasNoFilesystem: false, mounted: false, rootEntries: []}));
        compare(page.nothingToLose, false);
        compare(findChild(page, "confirmDialogDataLossLabel").visible, true);
        verify(findChild(page, "confirmDialogHeadlineLabel").text.indexOf("permanently erases") >= 0);
    }

    function test_driveWithContentsKeepsTheFullWarning() {
        var page = openConfirmFor(makeDisk({hasNoFilesystem: false, mounted: true,
                                            hasDjLibrary: true, rootEntries: ["PIONEER/", "Contents/"]}));
        compare(page.nothingToLose, false);
        compare(findChild(page, "confirmDialogDataLossLabel").visible, true);
    }

    // Softening the data-loss claim must not soften the identity check:
    // on an empty stick the residual risk is that this is not the drive
    // the user thinks it is, which an empty root says nothing about.
    function test_wrongDriveWarningSurvivesOnAnEmptyDrive() {
        var page = openConfirmFor(makeDisk({hasNoFilesystem: true, mounted: false, rootEntries: []}));
        compare(page.nothingToLose, true);
        compare(findChild(page, "confirmDialogDoubleCheckLabel").visible, true);
        // And the typed confirmation is still required.
        compare(findChild(page, "formatAcceptButton").enabled, false);
    }
}
