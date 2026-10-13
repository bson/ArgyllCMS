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

To install the programs into `bin/` and the reference files into `ref/` under
a directory of your choice:

    jam -q -fJambase -sDESTDIR=/opt/argyll-kde-wayland install

Put its `bin/` ahead of any distribution ArgyllCMS on your `PATH`. The
distribution package can stay installed: its udev rule is what gives users
access to the instruments.

To build the original X11 version instead:

    jam -q -fJambase -sUSE_WAYLAND=false

## Behavior on Wayland

- Test patches are shown in an overlay on the chosen output, with the exact
  values handed to the compositor. Argyll waits until each patch has actually
  been shown on screen before it starts the settling delay.
- While measuring, KWin Night Light is suspended and the screen is kept from
  blanking.
- Wayland doesn't let applications load the video card's calibration tables
  (VideoLUT). Instead, Argyll loads calibration curves by having KWin use a
  small generated profile that carries them, so `dispcal` can calibrate as
  well as profile. Temporary changes are undone when Argyll exits, and
  `dispwin` can still load a calibration that stays in effect.
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
- The patch window doesn't work on X11 sessions or on compositors other than
  KWin.
