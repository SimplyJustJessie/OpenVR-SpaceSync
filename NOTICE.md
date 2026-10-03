# Notice of modification

OpenVR-SpaceSync is a modified version of OpenVR-SpaceOverride by Nyabsi
(https://github.com/Nyabsi/OpenVR-SpaceOverride). Modifications to the original work were made
beginning on 2026-08-23 by Shinyflvres. The original project is licensed under the GNU Affero
General Public License v3.0 (AGPL-3.0-only), and so is this one. Nothing about the license changed.

- Original work: OpenVR-SpaceOverride, Copyright (C) 2026 Nyabsi
- Modified by: Shinyflvres (https://github.com/shinyflvre/OpenVR-SpaceSync)
- First modified release: 2026-08-23, work continues since then
- License: AGPL-3.0-only, see LICENSE

OpenVR-SpaceOverride itself is based on OpenVR-SpaceCalibrator by pushrax (MIT, see LICENSE.MIT).

## What I changed

Starting point was OpenVR-SpaceOverride commit `6604e42`. Since then:

- Driver: the head tracker only counts while it reports `TrackingResult_Running_OK`, the drift
  estimate only learns from clean frames and keeps its rotation and translation consistent,
  head motion is weighted down while measuring, a leftover latency between headset and tracker
  is learned on the fly, a couple of races and missing bounds checks got fixed, and the driver
  now logs tracker state changes and drift jumps.
- New "Follow SLAM HMD" mode: the headset keeps its own SLAM pose and the lighthouse devices
  (head tracker included) get re-aligned to it every frame.
- Overlay: one transform message per device instead of disable/enable, a new UI (Theme.cpp and
  UserInterface.cpp, with Manrope and JetBrains Mono embedded), a UI scale setting, a calibration
  wizard, descriptions under every setting, and cancel/result handling for the calibration.
- Rebrand to SpaceSync: exe, driver, IPC pipe, registry key (old OpenVR-SpaceOverride profiles are
  still picked up), installer (removes an old OpenVR-SpaceOverride install), build.bat, icon, README.

Every source file I touched or added has a "Modified by" or "Added by" line under its SPDX header.
The full history is in this repo's git log.

## Linux port

Modified by simplyyjessie beginning on 2026-10-03, starting from OpenVR-SpaceSync commit
`7f59d10`. Same license (AGPL-3.0-only). The Windows code paths are left as they were; Linux gets
its own implementations behind `_WIN32` checks:

- Driver: hooks swap the vtable slot instead of using MinHook, IPC uses an abstract Unix socket
  instead of a named pipe (`IPCServerPosix.cpp`), timing uses `CLOCK_MONOTONIC`, logs go to
  `$XDG_STATE_HOME/spacesync`.
- Overlay: Unix socket IPC client (`IPCClientPosix.cpp`), profile in
  `$XDG_CONFIG_HOME/spacesync/profile.json` instead of the registry, sound through SDL3 audio,
  single instance and desktop/SteamVR handover over an abstract Unix socket, a generated SteamVR
  manifest with an absolute binary path, X11/Wayland Vulkan surface extensions, Noto CJK fonts.
- Basestation power control is not part of the Linux build (`LighthousePosix.cpp` keeps only the
  app log); a separate lighthouse manager handles it.
- Build: `build.sh`, CMake changes for Linux.

Every file changed for the port has a "Modified by simplyyjessie" or "Added by simplyyjessie" line.

## Monado companion

Added by simplyyjessie on 2026-10-03: `spacesync-monado` (`src/monado/`, `include/monado/`,
`tests/monado_tests.cpp`), a companion for WiVRn/Monado that runs SpaceSync's Stay Aligned
(`include/driver/StayAligned.h`, unchanged) outside SteamVR, plus a no-head-tracker calibration.
It reads poses through a headless OpenXR session (XR_MNDX_xdev_space) and applies the result as the
lighthouse tracking origin offset through libmonado, the approach motoc (galister) uses.
