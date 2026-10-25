# Changes

## v1.1.1 (2026-10-25)

Firmware v1.1.1, star chart v0.9.1, Settings v1.0.1 (contains the same star chart code). NS-4000 Control v1.1.0 is unchanged.

- **Star chart v0.9.1 - fix:** "Send time" sent the time zone as `:SG-09.0#`, which OnStep 4.24j rejects, so the time zone stayed unset and UTC / sidereal time were wrong (seen after the saved settings were reset). It now sends `:SG-09#` (or `sHH:30` / `sHH:45`).
- **Star chart v0.9.1:** "Send site" and "Send time" no longer ask for confirmation.
- **Star chart v0.9.1:** shows the mount type (German equatorial / Fork / Alt-Az); the meridian limits are shown only for German equatorial mounts.
- **Firmware - fork and alt-az builds:** set `MOUNT_TYPE` in `Config.h` to `FORK` or `ALTAZM` and build. For alt-az, set the azimuth limits `AXIS1_LIMIT_MIN` / `AXIS1_LIMIT_MAX` (for example -360 / 360). The released binaries are for German equatorial mounts (GEM).
- **Firmware:** when a firmware built for another mount type starts for the first time, the saved settings (axis limits, park position, alignment, site, backlash, GOTO base rate) are reset to the `Config.h` defaults, because they belong to the old mount type. Wi-Fi, PIN and the paired handbox are kept. Updating a GEM board from v1.1.0 keeps all settings.
- **Documents:** `:SX93` - removed the `a`..`z` speed steps (not enabled in the firmware); it returns nothing.

## v1.1.0 (2026-10-24)

First public release.
