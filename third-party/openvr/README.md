`openvr.h` from https://github.com/ValveSoftware/openvr at commit
`0924064316de3effbcd1acf1e309182a2deb1c05` (sha256
`1e6ed57199896cc1f7c5484e50fa18955e97be15be690beb28d998c877ead7fd`), under Valve's
BSD-3-Clause license (see `LICENSE`).

Only used for type/interface declarations. chiaki-ng does not link against OpenVR;
on Linux it loads the installed SteamVR runtime's `libopenvr_api.so` at run time to
mark its Steam Frame theatre screen as side-by-side stereo (gui/src/framestereooverlay.cpp).
