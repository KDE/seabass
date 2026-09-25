# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

qt_add_executable(seabass
    WIN32
    MACOSX_BUNDLE
    main.cpp
)
if(WIN32)
    # App icon (.exe icon + taskbar/Alt-Tab), generated from the same
    # geometry as the seabass_soundbass.svg the QML UI itself uses.
    # Regenerate both, from the repository root, with:
    #   python3 tools/flatten_logo.py
    target_sources(seabass PRIVATE win/app_icon.rc)
    # UTF-8 as the process ANSI code page (Windows 10 1903+), for the
    # linked code that takes narrow paths: see win/app.manifest. MSVC's
    # linker merges a .manifest source into the one CMake generates;
    # MinGW gets it as an RT_MANIFEST resource through the .rc.
    if(MSVC)
        target_sources(seabass PRIVATE win/app.manifest)
    endif()
endif()
if(APPLE)
    # The same icon as an .icns (also written by tools/flatten_logo.py),
    # copied into the bundle's Resources and named by Info.plist.
    target_sources(seabass PRIVATE mac/app_icon.icns)
    set_source_files_properties(mac/app_icon.icns PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(seabass PROPERTIES
        MACOSX_BUNDLE_BUNDLE_NAME "Seabass"
        MACOSX_BUNDLE_GUI_IDENTIFIER "org.kde.seabass"
        MACOSX_BUNDLE_ICON_FILE "app_icon.icns"
        MACOSX_BUNDLE_COPYRIGHT "© 2026 Sebastian Kügler")
endif()

# `pragma Singleton` alone isn't enough for this Qt version's
# qt_add_qml_module to register Theme.qml as an actual QML singleton.
# Without this, the generated qmldir lists it as a plain component type
# ("Theme 1.0 qml/Theme.qml" instead of "singleton Theme 1.0 ..."), so
# every `Theme.xxx` property access anywhere in the app silently
# evaluates to `undefined` at runtime (QML logs "Unable to assign
# [undefined] to ..." rather than a hard error) instead of throwing a
# clear "Theme is not a type" error, verified via the generated qmldir
# and confirmed live via journalctl. Must be set *before*
# qt_add_qml_module() is called. It reads source file properties at
# call time, not deferred, so setting this afterward has no effect
# (confirmed: moving it after here silently produced the same broken
# non-singleton qmldir entry even after a full clean rebuild).
set_source_files_properties(qml/Theme.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/FormatLabels.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)
set_source_files_properties(qml/CamelotWheelPosition.qml PROPERTIES QT_QML_SINGLETON_TYPE TRUE)

# The app's QML files and C++ sources, shared with seabass_qml_tests
# (top-level CMakeLists.txt) so the test binary registers the very same
# SeabassGui module -- real pages with their real controllers, which is
# what tests/qml-live/ drives against a real stick.
set(SEABASS_GUI_QML_FILES
    qml/common/ActionCard.qml
    qml/common/BackBreadcrumb.qml
    qml/common/BigScrollBar.qml
    qml/common/BusyOverlay.qml
    qml/common/Heartbeat.qml
    qml/common/InfoButton.qml
    qml/common/MessageDialog.qml
    qml/common/MetadataListToolbar.qml
    qml/common/MetadataTrackDelegate.qml
    qml/common/ProgressReport.qml
    qml/common/ProgressTrack.qml
    qml/common/RowHoverShade.qml
    qml/common/SelectableText.qml
    qml/common/MessagePopup.qml
    qml/common/HealthCheckCard.qml
    qml/common/HealthCheckPage.qml
    qml/common/HomeIcon.qml
    qml/common/HeartIcon.qml
    qml/common/SeabassIcon.qml
    qml/common/IconLabel.qml
    qml/common/PageTitle.qml
    qml/common/SeabassDialog.qml
    qml/common/SpaceReclaimBar.qml
    qml/common/UnreferencedFilesNotice.qml
    qml/common/StatValue.qml
    qml/common/StatusBadge.qml
    qml/common/Subtitle.qml
    qml/common/TableHeaderLabel.qml
    qml/common/ExperimentalBadge.qml
    qml/common/TransferProgressFrame.qml
    qml/common/TypedConfirmDialog.qml
    qml/common/TransferResultFrame.qml
    qml/common/TransferOverlay.qml
    qml/common/PageScrollView.qml
    qml/common/DjSoftwareRunningDialog.qml
    qml/common/SaveOverlayButton.qml
    qml/common/UnsavedChangesDialog.qml
    qml/common/WatermarkLayer.qml
    qml/common/WatermarkRing.qml
    qml/common/WriteProgressDialog.qml
    qml/common/OperationSummaryDialog.qml
    qml/common/LockedLibraryDialog.qml
    qml/common/StickRemovedDialog.qml
    qml/common/EditSessionHost.qml
    qml/Theme.qml
    qml/FormatLabels.qml
    qml/CamelotWheelPosition.qml
    qml/Main.qml
    qml/StickListPage.qml
    qml/DuplicatesHubPage.qml
    qml/BackupsHubPage.qml
    qml/ScanPage.qml
    qml/TrackDetailPage.qml
    qml/DuplicatesPage.qml
    qml/CleanupPage.qml
    qml/CoverArtPage.qml
    qml/PendingDeletionsPage.qml
    qml/JunkCuePage.qml
    qml/LibraryConsistencyPage.qml
    qml/CuesAtZeroPage.qml
    qml/ImportPromptPage.qml
    qml/SampleRatesPage.qml
    qml/OneLibraryLeftoversPage.qml
    qml/LibraryHealthHubPage.qml
    qml/StickStatisticsPage.qml
    qml/StickPerformancePage.qml
    qml/TreemapView.qml
    qml/EngineLibraryCreatorPage.qml
    qml/SyncPage.qml
    qml/BackupsPage.qml
    qml/MetadataBackupPage.qml
    qml/MetadataRestorePage.qml
    qml/FormatToggle.qml
    qml/LibrarySourceToggle.qml
    qml/KeyBadge.qml
    qml/CamelotWheelPopup.qml
    qml/AboutPage.qml
    qml/DonationPage.qml
    qml/PlayerBar.qml
    qml/WaveformView.qml
    qml/SettingsPage.qml
    qml/AppSettingsPage.qml
    qml/AnonymizeLibraryPage.qml
    qml/UsbStickIcon.qml
    qml/StickWriteWarning.qml
    qml/CueFallbackNotice.qml
    qml/TrackWaveformCard.qml
    qml/PlaylistPickerCombo.qml
    qml/PlayerTrackRing.qml
    qml/PlaylistRowDelegate.qml
    qml/PlaylistListView.qml
    qml/MatchingPage.qml
    qml/StarRating.qml
    qml/FormatUsbPage.qml
    qml/StickBackupPage.qml
    qml/RestoreStickBackupPage.qml
    qml/CloneStickPage.qml
    qml/TrackDetailPanel.qml
    qml/TrackRing.qml
    qml/TrackRingFullscreen.qml
)
set(SEABASS_GUI_SOURCES
    media_controller.hpp
    media_controller.cpp
    scan_controller.hpp
    scan_controller.cpp
    duplicates_controller.hpp
    duplicates_controller.cpp
    cleanup_controller.hpp
    cleanup_controller.cpp
    add_cue_controller.hpp
    add_cue_controller.cpp
    library_consistency_controller.hpp
    library_consistency_controller.cpp
    stick_statistics_controller.hpp
    stick_statistics_controller.cpp
    stick_performance_controller.hpp
    stick_performance_controller.cpp
    engine_library_creator_controller.hpp
    engine_library_creator_controller.cpp
    format_usb_controller.hpp
    format_usb_controller.cpp
    home_backups_controller.hpp
    home_backups_controller.cpp
    stick_backup_controller.hpp
    stick_backup_controller.cpp
    restore_stick_backup_controller.hpp
    restore_stick_backup_controller.cpp
    backup_advisor_controller.hpp
    backup_advisor_controller.cpp
    clone_stick_controller.hpp
    clone_stick_controller.cpp
    library_fingerprint_reader.hpp
    library_fingerprint_reader.cpp
    stick_backup_paths.hpp
    future_result.hpp
    staged_plan_model.hpp
    staged_cue_edit_controller.hpp
    staged_cue_edit_controller.cpp
    sync_plan_list_model.hpp
    sync_plan_list_model.cpp
    sync_controller.hpp
    sync_controller.cpp
    full_backups_controller.hpp
    full_backups_controller.cpp
    backup_changelog_text.hpp
    backup_changelog_text.cpp
    metadata_backup_controller.hpp
    metadata_backup_proposal_model.hpp
    metadata_backup_controller.cpp
    metadata_backup_proposal_model.cpp
    metadata_restore_controller.hpp
    metadata_restore_controller.cpp
    metadata_restore_proposal_model.hpp
    metadata_restore_proposal_model.cpp
    playback_controller.hpp
    playback_controller.cpp
    settings_controller.hpp
    settings_controller.cpp
    app_settings_controller.hpp
    app_settings_controller.cpp
    controls_style.hpp
    controls_style.cpp
    interface_font.hpp
    interface_font.cpp
    update_decision.hpp
    update_decision.cpp
    update_checker.hpp
    update_checker.cpp
    anonymize_library_controller.hpp
    anonymize_library_controller.cpp
    dj_software_guard_controller.hpp
    dj_software_guard_controller.cpp
    write_guard.hpp
    write_guard.cpp
    qt_progress_reporter.hpp
    stick_catalogs.hpp
    stick_catalogs.cpp
    edit/library_edit_session.hpp
    edit/library_edit_session.cpp
    edit/edit_session_registry.hpp
    edit/edit_session_registry.cpp
    edit/direct_write_hold.hpp
    edit/direct_write_hold.cpp
    system_font_metrics.hpp
    system_font_metrics.cpp
)

# Seabass as a media player on the session bus -- how media keys reach a
# player on a Linux desktop. D-Bus is there already (seabass_edit links
# it on these platforms, for the sleep inhibitor).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND SEABASS_GUI_SOURCES mpris_service.hpp mpris_service.cpp)
endif()
# The Breeze icons the UI draws (common/SeabassIcon.qml), copied by
# tools/update-breeze-icons.sh -- which is also where to add one.
set(SEABASS_GUI_ICONS
    qml/icons/breeze/application-menu.svg
    qml/icons/breeze/archive-insert.svg
    qml/icons/breeze/arrow-down.svg
    qml/icons/breeze/arrow-right.svg
    qml/icons/breeze/backup.svg
    qml/icons/breeze/bookmarks.svg
    qml/icons/breeze/checkmark.svg
    qml/icons/breeze/configure.svg
    qml/icons/breeze/deep-history.svg
    qml/icons/breeze/dialog-warning.svg
    qml/icons/breeze/document-import.svg
    qml/icons/breeze/document-revert.svg
    qml/icons/breeze/document-save.svg
    qml/icons/breeze/draw-eraser.svg
    qml/icons/breeze/drive-removable-media-usb-pendrive.svg
    qml/icons/breeze/edit-clear-all.svg
    qml/icons/breeze/edit-clear.svg
    qml/icons/breeze/edit-copy.svg
    qml/icons/breeze/edit-delete-shred.svg
    qml/icons/breeze/edit-delete.svg
    qml/icons/breeze/edit-duplicate.svg
    qml/icons/breeze/edit-find.svg
    qml/icons/breeze/edit-undo.svg
    qml/icons/breeze/exchange-positions.svg
    qml/icons/breeze/folder-open.svg
    qml/icons/breeze/folder.svg
    qml/icons/breeze/go-down.svg
    qml/icons/breeze/go-home.svg
    qml/icons/breeze/go-next.svg
    qml/icons/breeze/go-previous.svg
    qml/icons/breeze/go-up.svg
    qml/icons/breeze/help-about.svg
    qml/icons/breeze/kt-check-data.svg
    qml/icons/breeze/link.svg
    qml/icons/breeze/media-eject.svg
    qml/icons/breeze/media-flash-sd-mmc.svg
    qml/icons/breeze/media-playback-pause.svg
    qml/icons/breeze/media-playback-start.svg
    qml/icons/breeze/media-seek-backward.svg
    qml/icons/breeze/media-seek-forward.svg
    qml/icons/breeze/media-skip-backward.svg
    qml/icons/breeze/media-skip-forward.svg
    qml/icons/breeze/office-chart-bar.svg
    qml/icons/breeze/server-database.svg
    qml/icons/breeze/sidebar-collapse-left.svg
    qml/icons/breeze/sidebar-expand-left.svg
    qml/icons/breeze/speedometer.svg
    qml/icons/breeze/view-list-details.svg
    qml/icons/breeze/view-media-equalizer.svg
    qml/icons/breeze/view-media-track.svg
    qml/icons/breeze/view-refresh.svg
    qml/icons/breeze/view-sort-ascending.svg
    qml/icons/breeze/view-sort-descending.svg
    qml/icons/breeze/window-close.svg
)
# The three catalog glyphs (LibrarySourceToggle, FormatToggle), subset
# from Noto Sans Symbols2 so they render the same on every platform
# instead of in whatever fallback font the system picks.
set(SEABASS_GUI_FONTS
    qml/fonts/NotoSansSymbols2-Seabass.ttf
)
# Relative here (qt_add_qml_module derives each file's resource path from
# its path relative to the calling directory); the parent gets them
# re-rooted so the test module in the top-level directory resolves them.
set(SEABASS_GUI_QML_FILES_FROM_ROOT ${SEABASS_GUI_QML_FILES})
list(TRANSFORM SEABASS_GUI_QML_FILES_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_ICONS_FROM_ROOT ${SEABASS_GUI_ICONS})
list(TRANSFORM SEABASS_GUI_ICONS_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_FONTS_FROM_ROOT ${SEABASS_GUI_FONTS})
list(TRANSFORM SEABASS_GUI_FONTS_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_SOURCES_FROM_ROOT ${SEABASS_GUI_SOURCES})
list(TRANSFORM SEABASS_GUI_SOURCES_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_QML_FILES_FROM_ROOT ${SEABASS_GUI_QML_FILES_FROM_ROOT} PARENT_SCOPE)
set(SEABASS_GUI_ICONS_FROM_ROOT ${SEABASS_GUI_ICONS_FROM_ROOT} PARENT_SCOPE)
set(SEABASS_GUI_FONTS_FROM_ROOT ${SEABASS_GUI_FONTS_FROM_ROOT} PARENT_SCOPE)
set(SEABASS_GUI_SOURCES_FROM_ROOT ${SEABASS_GUI_SOURCES_FROM_ROOT} PARENT_SCOPE)

qt_add_qml_module(seabass
    URI SeabassGui
    VERSION 1.0
    QML_FILES
        ${SEABASS_GUI_QML_FILES}
    RESOURCES
        qml/icons/seabass_soundbass.svg
        qml/images/sebas.png
        ${SEABASS_GUI_ICONS}
        ${SEABASS_GUI_FONTS}
    SOURCES
        ${SEABASS_GUI_SOURCES}
)

# TrackRing's fragment shader, compiled to .qsb at build time. Optional:
# without Qt's shader tools there is no shader in the binary, TrackRing
# reports itself unavailable and its caller shows the plain cover art.
# A prefix of its own rather than the QML module's, which differs between
# this target and seabass_qml_tests -- the QML names one URL for both.
if(Qt6ShaderTools_FOUND)
    qt_add_shaders(seabass "seabass_shaders"
        PREFIX "/seabass"
        FILES shaders/track_ring.frag)
else()
    message(STATUS "Qt6 ShaderTools not found: TrackRing falls back to plain cover art")
endif()

# edit/ is on the path too because qmltyperegistrar includes every
# QML_ELEMENT header by bare file name (`__has_include(<x.hpp>)`) and
# silently skips one it cannot find, which then fails much later as
# "'X' is not a member of 'seabass::gui'".
target_include_directories(seabass PRIVATE ${CMAKE_SOURCE_DIR}/src ${CMAKE_CURRENT_SOURCE_DIR}/edit)
target_link_libraries(seabass PRIVATE
    seabass_core
    seabass_edit
    Qt6::Core
    Qt6::Gui
    Qt6::Qml
    Qt6::Quick
    Qt6::QuickControls2
    Qt6::QuickLayouts
    Qt6::QuickDialogs2
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::Svg
    # The update check, and nothing else: Seabass makes no other
    # network request. See gui/update_checker.hpp.
    Qt6::Network
)
# See the find_package(Qt6 ...) call in the top-level CMakeLists.txt for
# why these two are conditional: an official (aqtinstall-fetched) Qt kit
# ships only their *Private counterparts, unlike MSYS2's packaging.
if(TARGET Qt6::QuickEffects)
    target_link_libraries(seabass PRIVATE Qt6::QuickEffects)
endif()
if(TARGET Qt6::QuickShapes)
    target_link_libraries(seabass PRIVATE Qt6::QuickShapes)
endif()

# Every QML file is compiled straight into this binary via Qt's qmlcache
# mechanism (see the .rcc/qmlcache/*.cpp objects this target builds) --
# nothing loose to install alongside it.
# On macOS the target is an app bundle; KDE_INSTALL_BUNDLEDIR is what
# Craft passes for where its packager looks for one.
if(NOT DEFINED KDE_INSTALL_BUNDLEDIR)
    set(KDE_INSTALL_BUNDLEDIR "Applications/KDE")
endif()
install(TARGETS seabass
    RUNTIME DESTINATION bin
    BUNDLE DESTINATION ${KDE_INSTALL_BUNDLEDIR})
