qt_add_executable(seabass
    WIN32
    main.cpp
)
if(WIN32)
    # App icon (.exe icon + taskbar/Alt-Tab), generated from the same
    # seabass_soundbass.svg the QML UI itself uses. Regenerate with:
    #   magick -background none qml/icons/seabass_soundbass.svg \
    #       -define icon:auto-resize=256,128,64,48,32,16 win/app_icon.ico
    target_sources(seabass PRIVATE win/app_icon.rc)
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
    qml/common/InfoButton.qml
    qml/common/MessageDialog.qml
    qml/common/MetadataListToolbar.qml
    qml/common/MetadataTrackDelegate.qml
    qml/common/ProgressReport.qml
    qml/common/ProgressTrack.qml
    qml/common/SelectableText.qml
    qml/common/MessagePopup.qml
    qml/common/HealthCheckCard.qml
    qml/common/HomeIcon.qml
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
    qml/common/PageScrollView.qml
    qml/common/DjSoftwareRunningDialog.qml
    qml/common/SaveOverlayButton.qml
    qml/common/UnsavedChangesDialog.qml
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
    qml/PendingDeletionsPage.qml
    qml/JunkCuePage.qml
    qml/LibraryConsistencyPage.qml
    qml/LibraryHealthHubPage.qml
    qml/StickStatisticsPage.qml
    qml/TreemapView.qml
    qml/EngineLibraryCreatorPage.qml
    qml/SyncPage.qml
    qml/BackupsPage.qml
    qml/LocalCuePage.qml
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
    qml/PlaylistRowDelegate.qml
    qml/PlaylistListView.qml
    qml/MatchingPage.qml
    qml/StarRating.qml
    qml/FormatUsbPage.qml
    qml/StickBackupPage.qml
    qml/RestoreStickBackupPage.qml
    qml/CloneStickPage.qml
    qml/TrackDetailPanel.qml
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
    engine_library_creator_controller.hpp
    engine_library_creator_controller.cpp
    format_usb_controller.hpp
    format_usb_controller.cpp
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
    sync_controller.hpp
    sync_controller.cpp
    backups_controller.hpp
    backups_controller.cpp
    local_cue_controller.hpp
    local_cue_controller.cpp
    metadata_backup_controller.hpp
    metadata_backup_controller.cpp
    metadata_restore_controller.hpp
    metadata_restore_controller.cpp
    playback_controller.hpp
    playback_controller.cpp
    settings_controller.hpp
    settings_controller.cpp
    app_settings_controller.hpp
    app_settings_controller.cpp
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
# Relative here (qt_add_qml_module derives each file's resource path from
# its path relative to the calling directory); the parent gets them
# re-rooted so the test module in the top-level directory resolves them.
set(SEABASS_GUI_QML_FILES_FROM_ROOT ${SEABASS_GUI_QML_FILES})
list(TRANSFORM SEABASS_GUI_QML_FILES_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_SOURCES_FROM_ROOT ${SEABASS_GUI_SOURCES})
list(TRANSFORM SEABASS_GUI_SOURCES_FROM_ROOT PREPEND "src/gui/")
set(SEABASS_GUI_QML_FILES_FROM_ROOT ${SEABASS_GUI_QML_FILES_FROM_ROOT} PARENT_SCOPE)
set(SEABASS_GUI_SOURCES_FROM_ROOT ${SEABASS_GUI_SOURCES_FROM_ROOT} PARENT_SCOPE)

qt_add_qml_module(seabass
    URI SeabassGui
    VERSION 1.0
    QML_FILES
        ${SEABASS_GUI_QML_FILES}
    RESOURCES
        qml/icons/seabass_soundbass.svg
    SOURCES
        ${SEABASS_GUI_SOURCES}
)

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
    Qt6::QuickEffects
    Qt6::QuickDialogs2
    Qt6::QuickShapes
    Qt6::Multimedia
    Qt6::Concurrent
    Qt6::Svg
)
