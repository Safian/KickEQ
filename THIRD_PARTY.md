# Third-party software

KickEQ is licensed under AGPL-3.0-or-later. It is built against the following components;
nothing is vendored in this repository, everything is fetched at configure time by CMake
`FetchContent` at the exact revisions below (see `CMakeLists.txt` and `clap/CMakeLists.txt`).

`kickeq_core` (the DSP core) uses nothing but the C++ standard library. The native CLAP shell (`clap/`,
`KickEQ-native.clap`, `libkickeq_clap_native.a`) uses only the CLAP headers; JUCE, clap-juce-extensions and the
VST3 SDK are needed only when the JUCE shell is built (`KICKEQ_BUILD_JUCE=ON`), and are not fetched otherwise.

| Component | Version / revision | Licence (as used here) | Source |
|-----------|--------------------|------------------------|--------|
| JUCE | tag `8.0.17-security` (commit `dabd5ad13cd869cba41046947fffc8e5ce5cd662`) | AGPL-3.0 (JUCE is dual-licensed AGPLv3 / commercial; the AGPLv3 option is used) | https://github.com/juce-framework/JUCE |
| clap-juce-extensions | commit `7adee3a1bd4684d4caa5601100e364abccff4b4d` (`0.26.0-117-g7adee3a`, 2026-10-06; the last tag, 0.26.0, dates from 2022) | MIT | https://github.com/free-audio/clap-juce-extensions |
| CLAP headers for the native shell (`clap/CMakeLists.txt`, fetched but not added as a CMake project) | tag `1.2.7` (`29ffcc273be7c7c651f6c9953b99e69700e2387a`) | MIT (headers only, nothing is linked) | https://github.com/free-audio/clap |
| CLAP (`clap-libs/clap`, submodule of clap-juce-extensions; used by the JUCE CLAP wrapper) | 1.2.7 (`29ffcc273be7c7c651f6c9953b99e69700e2387a`) | MIT | https://github.com/free-audio/clap |
| clap-helpers (`clap-libs/clap-helpers`, submodule) | `a61bcdf0ecc2c8db1e80bfe8bf9cb7e8d9fd2bbc` | MIT | https://github.com/free-audio/clap-helpers |
| Steinberg VST3 SDK (bundled inside JUCE) | 3.8.0 | MIT | https://github.com/steinbergmedia/vst3sdk |

JUCE itself bundles further third-party code inside its modules (for example HarfBuzz in
`juce_graphics`, libpng and libjpeg image codecs, FLAC in `juce_audio_formats`); their licences are
listed in JUCE's own `LICENSE.md` and the respective module directories, and are not repeated here.
The pinned JUCE version is built from source; nothing of it is copied into this repository. The editor uses only
JUCE's own widgets and drawing code (software renderer; no OpenGL, no web view, no fonts or images are bundled).

## Tools used for verification only (not linked, not distributed)

| Tool | Version | Licence | Used for |
|------|---------|---------|----------|
| pluginval | v1.0.4 (release assets pinned by SHA-256 in `.github/workflows/ci.yml`) | GPL-3.0 | VST3 validation |
| clap-validator | 0.4.1 (built from source with cargo) | MIT | CLAP validation |
| GitHub actions `checkout`, `upload-artifact`, `download-artifact`, `cache` | pinned by commit SHA in `.github/workflows/ci.yml` | MIT | CI |

VST is a registered trademark of Steinberg Media Technologies GmbH.
