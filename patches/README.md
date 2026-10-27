# Dependency patches

## libaribcaption-deferred-wrap.patch

Base: libaribcaption `37dde0f32652c32aa98bf92278944d1cfb2e7354` (applied by CMake through FetchContent, and by
the Flatpak manifest).

**Problem:** the decoder wraps the line as soon as a character fills it up to the right edge of the display
area (SDF). When the encoder sends a full line followed by `APR` — a practice used by Brazilian broadcasters
(e.g. Globo) and by the SkylarBox encoder — the next line moves down two positions and may leave the area.
Upstream issue: https://github.com/xqq/libaribcaption/issues/27

**Fix:** after a character is written, advancing only marks the wrap as pending; it happens when the next
character is written. Any explicit movement (`APR`, `APD`, `APF`, `APS`, `ACPS`, `SDP`, `CS`) cancels it.

Normative basis: ARIB TR-B14 §4.4.4 ties the wrap to displaying the following character. Not confirmed in the
ABNT NBR 15606-1 text (not available to the project); validated against a real Globo stream.

Test: `ctest --test-dir build` (`tests/test_caption_layout.cpp`).

Once upstream fixes the issue, remove the `PATCH_COMMAND` from `CMakeLists.txt` (and the patch source from the
Flatpak manifest) and update the `GIT_TAG`.
