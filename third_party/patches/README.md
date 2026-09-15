# Patches for vendored third-party code

Neither of the submodules under `third_party/` is patched in place: they are
checked out at the pinned commit and built as-is, so `git submodule status`
stays clean and `scripts/init-submodules.sh` needs no extra step. Anything
in this directory is a fix that belongs **upstream**, kept here so it can be
sent there and so a local build can apply it if needed.

Nothing applies these automatically.

## libdjinterop-0001-gate-tests-on-boost-components.patch

Against `xsco/libdjinterop` at `85f0622` (v0.27.3).

`CMakeLists.txt:414` requests Boost components but gates its test targets on
`Boost_FOUND`, which with `CMP0167 NEW` is true for a headers-only install
(`BoostConfig.cmake` is satisfied by `boost_headers` alone). The twelve test
targets are then configured with an empty `${Boost_LIBRARIES}`, and the two
that use Boost.Filesystem fail to link. `QUIET` hides the diagnostic, and the
`else()` branch that exists for exactly this case never runs.

The patch gates on `Boost_filesystem_FOUND`/`Boost_system_FOUND` as well, and
says in the message which libraries are missing. Sent upstream as
xsco/libdjinterop#200.

Verified on Ubuntu 24.04, CMake 3.30.5, GCC 13.3.0:

- Boost.Filesystem resolvable: all 12 targets build, `ctest` 12/12 in 7.2s.
- Headers only: no test targets configured, `cmake --build` exits 0, and the
  message says which libraries are missing.

Seabass builds these tests by default (`SEABASS_LIBDJINTEROP_TESTS=ON`) and
requires the Boost component itself, so the trap cannot be reached from a
Seabass build either way. The patch matters to anyone building libdjinterop
directly, and to us if `-DSEABASS_LIBDJINTEROP_TESTS=OFF` is ever the default
again. See sebasje/seabass#9 and #12.
