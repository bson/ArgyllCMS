# ArgyllCMS for Wayland

This is ArgyllCMS 3.5.0 with native Wayland display support added for KDE
Plasma. On a Plasma Wayland session, the calibration patch window, display
list, profile installation and plot windows talk to the compositor directly.
On X11 sessions (GNOME, Plasma or others) the original X11 code is used, and
on other Wayland desktops X11 through XWayland. One build does all of this,
choosing when each program starts. The upstream documentation in `doc/` and
`ReadMe.txt` still applies to everything else.

## Requirements

- For native Wayland: KDE Plasma 6 (KWin). Other compositors don't provide the
  output information (EDID, the ICC profile in use) or the output color mode
  controls this needs.
- Build: `jam`, `wayland-scanner`, and development files for `wayland-client`,
  `dbus-1` and X11 (`libX11`, `libXext`, `libXxf86vm`, `libXinerama`,
  `libXrandr`, `libXss`). The Wayland protocol definitions it needs come with the
  source, as some are newer than many distributions' packages.

## Building

    sh makeall.sh

To install the programs into `bin/` and the reference files into `ref/` under
a directory of your choice:

    jam -q -fJambase -sDESTDIR=/opt/argyll-kde-wayland install

Put its `bin/` ahead of any distribution ArgyllCMS on your `PATH`. The
distribution package can stay installed: its udev rule is what gives users
access to the instruments.

To leave out one of the two, add `-sUSE_WAYLAND=false` (the original X11
version) or `-sUSE_X11=false` (Wayland only, no X libraries needed).

To make a program use X11 or Wayland regardless of the session, set
`ARGYLL_WINDOW_SYSTEM=x11` or `ARGYLL_WINDOW_SYSTEM=wayland`.
`dispwin -Q` shows what it finds: the desktop, the Wayland compositor and X
server, which of the two it would use, and whether that is supported here.

## Behavior on Wayland

- Test patches are shown in an overlay on the chosen output, with the exact
  values handed to the compositor. Argyll waits until each patch has actually
  been shown on screen before it starts the settling delay.
- While measuring, KWin Night Light is suspended and the screen is kept from
  blanking.
- Wayland doesn't let applications load the video card's calibration tables
  (VideoLUT). While measuring, Argyll applies calibration curves to the test
  patches itself, with the output in KWin's sRGB mode, whose values pass
  through unaltered; so `dispcal` can calibrate as well as profile. Outside
  measurement, calibration curves are loaded by having KWin use a small
  generated profile that carries them. Temporary changes are undone when
  Argyll exits, and `dispwin` can still load a calibration that stays in
  effect.
- Installing a profile makes KWin use it for that output, including its
  calibration curves.
- If the instrument stops responding, when it's opened or in the middle of
  a run, Argyll pauses for a few seconds and reconnects to it, as the
  instrument makers' own software does.

## Troubleshooting

- Some older instruments (seen with an i1Display Pro) lose data on the USB
  ports of modern laptops, which shows up as repeated reconnects. A powered
  USB 2.0 hub between the laptop and the instrument fixes this.

## Limitations

- If Argyll is killed outright (`kill -9`) during a measurement, the output may
  be left using a temporary calibration profile. Re-select your profile in
  System Settings > Display & Monitor.
- On Wayland desktops other than Plasma, Argyll runs through XWayland, which
  generally can't load calibration curves into the display; profiling works,
  but calibrating with `dispcal` doesn't take effect.
