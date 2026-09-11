#pragma once

#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace seabass::testing
{

// The temp directory this test process owns, and nobody else does.
//
// Every test in the tree builds its scratch path from
// fs::temp_directory_path() plus a name derived from the test, which is
// fine until two suites run on one machine at once -- which happens
// routinely here, because more than one working copy of Seabass is
// often being built and tested at the same time. Then both processes
// pick the same directory, one calls remove_all() on it while the other
// is walking it, and the second dies with
//
//     cannot increment recursive directory iterator: No such file or
//     directory
//
// which reads exactly like a bug in whatever was being tested. It cost
// two sessions an evening between them before anyone spotted that the
// failing test changed run to run and the common factor was the clock.
//
// The pid is enough: a scratch tree only has to outlive the process that
// made it. Tests keep their own descriptive names underneath, so a
// leftover directory still says which test left it.
//
// This does not cover $HOME. SEABASS_HOME (set per test by the top-level
// CMakeLists) already sandboxes that side.
inline std::filesystem::path scratchRoot()
{
#if defined(_WIN32)
    const auto pid = static_cast<long long>(_getpid());
#else
    const auto pid = static_cast<long long>(::getpid());
#endif
    std::filesystem::path root = std::filesystem::temp_directory_path() / ("seabass-test-" + std::to_string(pid));
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    return root;
}

// Points SEABASS_HOME at a directory under the test's scratch tree unless
// ctest has already sandboxed it, so a test run by hand can never write
// into the real ~/Seabass. Read every call by seabass_paths, so this
// takes effect immediately.
inline void sandboxSeabassHome(const std::filesystem::path &home)
{
    if (std::getenv("SEABASS_HOME") != nullptr) {
        return;
    }
#if defined(_WIN32)
    _putenv_s("SEABASS_HOME", home.string().c_str());
#else
    setenv("SEABASS_HOME", home.string().c_str(), 1);
#endif
}

// The settings store, which SEABASS_HOME does NOT cover.
//
// QSettings("seabass", "seabass") resolves through the platform's own
// config location, so a test that merely constructs a controller writes
// into the developer's real ~/.config/seabass/seabass.conf. open_folder
// _test did exactly that, four rows a run, for long enough to leave 232
// dead folder rows in one -- every one of them showing up as a card on
// the app's first page, pointing at a /tmp path that no longer existed.
//
// Redirecting with QSettings::setPath(IniFormat, ...) is what that test
// tried first and it does not work: the two-argument constructor takes
// the NATIVE format, which on Unix is a different QSettings::Format enum
// value than IniFormat even though both write .ini-shaped files, so the
// path set for IniFormat is never consulted. Moving the environment
// variable the platform itself reads is what actually redirects it, and
// it covers every QSettings in the process rather than one format.
//
// Must be called before the first QSettings is constructed -- Qt caches
// the resolved location per format+scope.
inline void sandboxSettings(const std::filesystem::path &configHome)
{
    std::error_code ec;
    std::filesystem::create_directories(configHome, ec);
#if defined(_WIN32)
    // QSettings' UserScope on Windows is the registry by default, but a
    // test process that sets APPDATA and asks for IniFormat writes here.
    _putenv_s("APPDATA", configHome.string().c_str());
#else
    setenv("XDG_CONFIG_HOME", configHome.string().c_str(), 1);
#endif
}

}  // namespace seabass::testing
