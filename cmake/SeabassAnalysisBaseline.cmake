# SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
#
# SPDX-License-Identifier: BSD-2-Clause

# The phase 2 baseline of docs/track-analysis-plan.md: the beat and key
# analysis Mixxx runs, on the Queen Mary DSP library (qm-dsp, GPL-2.0 or
# later), for tools/analysis_benchmark and its tests only. Nothing here is
# linked into the app, seabass_core or the command line.
#
# qm-dsp is taken from Mixxx's own tree at the 2.5.3 release rather than
# from c4dm/qm-dsp, because Mixxx carries changes to it
# (lib/qm-dsp/mixxx-changes.patch: the key detector's chroma bins and
# profile alignment, the constant-Q kernel's phase, include paths) and
# the baseline is meant to be Mixxx's method as Mixxx ships it. The
# archive is pinned by commit and by hash; only lib/qm-dsp is built.

# SOURCE_SUBDIR and DOWNLOAD_EXTRACT_TIMESTAMP below need it; the rest of
# the build does not, so only this option asks for it.
if(CMAKE_VERSION VERSION_LESS 3.24)
    message(FATAL_ERROR "SEABASS_ANALYSIS_BASELINE needs CMake 3.24 or later")
endif()
include(FetchContent)

set(SEABASS_MIXXX_COMMIT 93275786c517e0804951f7428a5cf045aa541b8d)
FetchContent_Declare(seabass_mixxx_qmdsp
    URL https://github.com/mixxxdj/mixxx/archive/${SEABASS_MIXXX_COMMIT}.tar.gz
    URL_HASH SHA256=203ff177662e084c7c9a35e29388323f7b19d03bf3e0d448300a99d03938de7f
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    # A folder with no CMakeLists.txt, so that making it available only
    # unpacks it: Mixxx's own build is never configured.
    SOURCE_SUBDIR lib/qm-dsp)
FetchContent_MakeAvailable(seabass_mixxx_qmdsp)
set(SEABASS_QMDSP_DIR ${seabass_mixxx_qmdsp_SOURCE_DIR}/lib/qm-dsp)

# The files Mixxx's QueenMaryDsp target builds, minus the tonal change
# and downbeat code neither analyser here reaches.
add_library(seabass_qmdsp STATIC EXCLUDE_FROM_ALL
    ${SEABASS_QMDSP_DIR}/base/Pitch.cpp
    ${SEABASS_QMDSP_DIR}/dsp/chromagram/Chromagram.cpp
    ${SEABASS_QMDSP_DIR}/dsp/chromagram/ConstantQ.cpp
    ${SEABASS_QMDSP_DIR}/dsp/keydetection/GetKeyMode.cpp
    ${SEABASS_QMDSP_DIR}/dsp/onsets/DetectionFunction.cpp
    ${SEABASS_QMDSP_DIR}/dsp/onsets/PeakPicking.cpp
    ${SEABASS_QMDSP_DIR}/dsp/phasevocoder/PhaseVocoder.cpp
    ${SEABASS_QMDSP_DIR}/dsp/rateconversion/Decimator.cpp
    ${SEABASS_QMDSP_DIR}/dsp/signalconditioning/DFProcess.cpp
    ${SEABASS_QMDSP_DIR}/dsp/signalconditioning/FiltFilt.cpp
    ${SEABASS_QMDSP_DIR}/dsp/signalconditioning/Filter.cpp
    ${SEABASS_QMDSP_DIR}/dsp/signalconditioning/Framer.cpp
    ${SEABASS_QMDSP_DIR}/dsp/tempotracking/TempoTrackV2.cpp
    ${SEABASS_QMDSP_DIR}/dsp/transforms/FFT.cpp
    ${SEABASS_QMDSP_DIR}/ext/kissfft/kiss_fft.c
    ${SEABASS_QMDSP_DIR}/ext/kissfft/tools/kiss_fftr.c
    ${SEABASS_QMDSP_DIR}/maths/Correlation.cpp
    ${SEABASS_QMDSP_DIR}/maths/KLDivergence.cpp
    ${SEABASS_QMDSP_DIR}/maths/MathUtilities.cpp)
# As Mixxx builds it: kissfft in double precision.
target_compile_definitions(seabass_qmdsp PRIVATE kiss_fft_scalar=double)
if(MSVC)
    target_compile_definitions(seabass_qmdsp PRIVATE _USE_MATH_DEFINES)
endif()
target_include_directories(seabass_qmdsp SYSTEM PUBLIC ${SEABASS_QMDSP_DIR} ${SEABASS_QMDSP_DIR}/include)
set_target_properties(seabass_qmdsp PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Third-party code: its warnings are not ours to fix, and it is the hot
# loop of every baseline run, so it is optimised in a Debug build too.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(seabass_qmdsp PRIVATE -w $<$<CONFIG:Debug>:-O2>)
endif()

# The analysers: Mixxx's plugin code and its ironing, ported
# (tools/analysis_baseline, GPL-2.0 or later), behind the GridAnalyser
# port and a KeyAnalyser of the tool's own.
add_library(seabass_analysis_baseline STATIC EXCLUDE_FROM_ALL
    tools/analysis_baseline/mixxx_beatutils.cpp
    tools/analysis_baseline/mixxx_qm_plugins.cpp
    tools/analysis_baseline/baseline_analysers.cpp)
target_include_directories(seabass_analysis_baseline PUBLIC src tools)
target_link_libraries(seabass_analysis_baseline PUBLIC seabass_qmdsp)
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    set_source_files_properties(tools/analysis_baseline/mixxx_qm_plugins.cpp tools/analysis_baseline/mixxx_beatutils.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<CONFIG:Debug>:-O2>")
endif()
