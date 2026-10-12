# ArgyllCMS for Wayland

This is ArgyllCMS 3.5.0 with its Linux display code moved from X11 to
Wayland. The calibration patch window, display list, profile installation and
plot windows talk to the compositor directly. X11, XWayland and XRandR are not
used. The upstream documentation in `doc/` and `ReadMe.txt` still applies to
everything else.

## Requirements

- KDE Plasma 6 (KWin). Other compositors don't provide the output information
  (EDID, the ICC profile in use) or the output color mode controls this needs.
- Build: `jam`, `wayland-scanner`, and development files for `wayland-client`,
  `wayland-protocols`, `plasma-wayland-protocols` and `dbus-1`.

## Building

    sh makeall.sh

To build the original X11 version instead:

    jam -q -fJambase -sUSE_WAYLAND=false

## Behavior on Wayland

- Test patches are shown in an overlay on the chosen output, with the exact
  values handed to the compositor. Argyll waits until each patch has actually
  been shown on screen before it starts the settling delay.
- While measuring, KWin Night Light is suspended and the screen is kept from
  blanking.
- When Argyll asks for the display's native response, the output's color
  profile is switched off for the run and restored afterwards.
- Installing a profile makes KWin use it for that output, including its
  calibration curves.

## Limitations

- Applications can't load the video card's calibration tables (VideoLUT) on
  Wayland. `dispcal` can therefore only profile the display as it is. It can't
  build calibration curves, and it warns about this.
- If Argyll is killed outright (`kill -9`) during a measurement, the output may
  be left with its color profile switched off. Re-select the profile in System
  Settings > Display & Monitor.
- The patch window doesn't work on X11 sessions or on compositors other than
  KWin.
