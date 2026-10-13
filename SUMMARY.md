# Implementation summary

Details of the Wayland port, relative to the upstream ArgyllCMS 3.5.0 import
(the first commit).

## Build switches

- `Jamtop`: `USE_WAYLAND ?= true` (non-Apple Unix). When true it adds
  `DEFINES += UNIX_WAYLAND`, finds the libdbus include dirs by `GLOB` (jam
  can't run pkg-config), checks for the wayland-protocols and
  plasma-wayland-protocols XML dirs (`WLPROTO_SYS`, `WLPROTO_KDE`, overridable
  with `-s`), and links `-lwayland-client -ldbus-1` in place of the X libs.
  `?=` is deliberate so that `-sUSE_WAYLAND=false` on the command line wins.
- `Jamtop`: rule `WaylandProtocol basename : file.xml ;` generates
  `basename.h`/`basename.c` with `wayland-scanner` (client-header,
  private-code) in the current directory.
- `h/aconfig.h`: `UNIX_X11_DISP` is defined when `UNIX_X11` is and
  `UNIX_WAYLAND` isn't. `UNIX_X11` stays defined on Wayland builds because the
  Linux USB/HID code, `USE_UCMM` and the ICC header platform depend on it. All
  Xlib display code is now under `UNIX_X11_DISP`.
- `Jamfile`: the bundled zlib test used `HAVE_ZLIB`, but `Jamtop` sets
  `HAVE_Z`, so the bundled copy was always built (and fails with GCC 16).
  Fixed to test `HAVE_Z`.

## Protocol glue placement

Static-link order dictated where the generated glue lives:

- `plot/` (`libplot`, and the standalone `plot` test): `xdg-shell`,
  `xdg-decoration`, `wlr-layer-shell`. These reference each other
  (layer-shell popups are `xdg_popup`s), so they must be in one archive.
  `libdisp` users always link `libplot` after `libdisp`.
- `spectro/` (`libconv`): viewporter, presentation-time, single-pixel-buffer,
  color-management-v1, idle-inhibit, kde-output-device-v2,
  kde-output-management-v2. Only `dispwin_wl.c` uses these, and every
  `libdisp` user links `libconv` after it.
- `plot/wlproto/wlr-layer-shell-unstable-v1.xml` is a vendored copy (v5, MIT)
  because Fedora doesn't package wlr-protocols.

## spectro/dispwin_wl.c, dispwin_wl.h

- `wl_get_displays()`: enumerates `kde_output_device_v2` devices (registry
  bound at v21, the minimum KWin accepts; older KWin's per-device globals are
  bound at up to v20). Enabled devices are ordered by `priority` (primary
  first). `disppath` gets the logical geometry (mode size / scale, rotated for
  odd transforms), the connector name, the device UUID (`wl_uuid`) and the EDID
  (base64-decoded, capped at 256 bytes for ucmm).
- `wl_dispwin_open()`: reconnects, finds the device by UUID (falling back to
  the connector name) and its `wl_output` by name (needs `wl_output` v4). It
  sets `fdepth = rdepth = 8`, `edepth = 16` and, if KWin's output management
  supports profile source changes (v8+), `nent = 256`, `ndepth = 8`. It saves
  the output's profile source, profile path and color/power tradeoff, and the
  LUT those give (`base_lut`), for the restore. With a window it then:
  - inhibits KWin Night Light over D-Bus (`org.kde.KWin.NightLight.inhibit`)
    and polls `currentTemperature` until it reaches 6500K (KWin fades). The
    private bus connection is held open because KWin drops an inhibit when
    its owner disconnects.
  - if `native & 1`, switches `color_power_tradeoff` to accuracy through
    `kde_output_configuration_v2`. Linear output itself comes from the
    caller loading a linear LUT.
  - creates a layer-shell OVERLAY surface (black single-pixel buffer, empty
    input region, no keyboard focus, exclusive zone -1) with a synchronized
    subsurface for the patch, both sized with `wp_viewporter`.
  - tags both surfaces with the output's own image description
    (`wp_color_manager_v1`), so KWin passes values through instead of
    treating them as sRGB.
  - adds an idle inhibitor.
- `wl_dispwin_set_color()`: quantizes `r_rgb` to `fdepth` like the X11 path,
  looks it up in the software LUT while measuring (see below), scales to 32 bits for `create_u32_rgba_buffer`, commits the patch and the
  parent, then waits up to 1s for `wp_presentation` feedback (or a frame
  callback) before returning.
- `wl_dispwin_restore()`: undoes temporary changes (tradeoff, sRGB mode
  while measuring, generated LUT profile) from the saved state, deletes a temporary generated profile and
  releases Night Light. It is called from `restore_display()`, so it also
  runs from the signal handler. Talking to the compositor from the handler can
  deadlock if a signal arrives during the presentation wait, the same hazard
  Xlib had. A saved profile path that no longer exists (just uninstalled) is
  left out, because KWin rejects it.
- `wl_dispwin_set_profile()` / `wl_dispwin_get_profile_path()`: set or read
  the output's ICC profile. Setting one updates the saved state and resets
  `base_lut` to the profile's vcgt, so restoring the caller's copy of the
  calibration on exit is a no-op instead of replacing the new profile. While
  measuring it only updates the saved state, which the restore applies.

### VideoLUT emulation

KWin applies the vcgt of the output's profile after compositing, and our
tagged surfaces pass through the rest of the profile untouched as long as its
TRC is a pure power curve. So `wl_dispwin_set_lut()` writes a profile with sRGB
colorants, gamma 2.2 curves (`icmCurveGamma`, stored as u8Fixed8 2.199) and
the LUT as a 256 entry vcgt, and points KWin at it in ICC mode.

- Generated profiles go in `$XDG_DATA_HOME/ArgyllCMS/wayland/` and are named
  `vcgt-<uuid prefix>-<FNV-1a of the LUT>.icc`, because KWin may not re-read a
  profile whose path is unchanged. Superseded temporary ones are deleted.
- A non-persistent LUT equal to `base_lut` (e.g. `restore_display()` putting
  back the original) restores the saved profile instead of generating one.
- A persistent LUT (`dispwin cal`, `dispwin -c`) becomes the new saved state
  and `base_lut`, so the exit restore leaves it in place. The previous
  generated persistent profile is deleted.
- While measuring (`sw_lut`), the LUT is applied to the patches instead.
  KWin's ICC mode doesn't pass test values through with any profile:
  single-channel ramps measured with a generated profile clipped red at
  87.5% and blue at 97.5% and boosted green, and the vcgt is sampled about an
  entry off. Earlier, per-channel TRCs in dispcal/colprof profiles shifted
  grays as well. dispcal fits its model to those ramps, giving profiles whose
  white disagreed with their grays. sRGB mode gives a clean ramp. So
  `wl_dispwin_open()` switches the output to sRGB mode (`cps_changed`) before
  making the window, as the window's surfaces are tagged with the output's
  image description, and starts `cur_lut` at `base_lut`. Non-persistent LUT
  loads then only replace `cur_lut`, and `wl_dispwin_set_color()` draws
  `cur_lut(v)`, interpolated and unquantized, so native high-precision mode
  still reaches between 8 bit levels. Persistent loads and profile installs
  update the saved state for the restore without touching KWin. Without a
  window (e.g. `dispwin cal`), or if sRGB mode can't be set, LUT loads go
  through generated profiles as above.
- `wl_dispwin_get_lut()` returns `cur_lut` while measuring, otherwise the
  vcgt of the profile KWin is using, or linear without one. Argyll can't read ICC v4 profiles, so their vcgt is
  taken as linear.
- Each LUT load through KWin takes about 50 ms; while measuring, loads cost
  nothing.

## Platform check for front ends

`dispwin -Q` prints one line, `platform supported: <what>` or
`platform unsupported: <why>`, and exits 0 or 1. It runs right after option
parsing, before any display is opened, since opening one fails on a platform
the build doesn't support. It reports `dispwin_check_platform()`
(`dispwin.c`): on Wayland builds `wl_check_platform()`, which connects to the
compositor and requires `wl_compositor`, `wl_subcompositor`,
`zwlr_layer_shell_v1`, `wp_viewporter`, `wp_single_pixel_buffer_manager_v1`,
`wp_color_manager_v1`, `kde_output_device_v2` devices and
`kde_output_management_v2` v8+, naming whatever is missing. X11 builds check
that the X display opens; Windows and macOS builds report supported.
Without `WAYLAND_DISPLAY`, libwayland tries the default `wayland-0` socket, so
in a Plasma session it still connects; an X11 session has no such socket.

## spectro/dispwin.c, dispwin.h

- `disppath`/`dispwin`: `edid`/`edid_len` stay under `UNIX_X11`, X fields move
  to `UNIX_X11_DISP`, `wl_uuid` and `struct _wlwin *wl` are under
  `UNIX_WAYLAND`. `get_a_display()` strdup's `wl_uuid` because of its
  structure copy.
- `dispwin_get_ramdac()` / `dispwin_set_ramdac()` go through the VideoLUT
  emulation above. Without it (`nent == 0`), get returns NULL and callers
  treat the display as having no VideoLUT.
- Profile install/uninstall/get keep the ucmm/colord logic. On Wayland the
  X11 atom work is replaced by pointing KWin at the installed copy
  (`ucmm_get_monitor_profile()` after install), resetting to sRGB on
  uninstall, and falling back to KWin's reported profile path in
  `get_profile`.
- `new_dispwin()`: Wayland block calls `wl_dispwin_open()` and installs the
  signal handlers. `dispwin_del()` calls `wl_dispwin_close()`.
- `dispwin_uninstall_profile()`: on Wayland also makes `p->or` linear, since
  KWin applies no vcgt without a profile, so the exit restore doesn't bring
  back the uninstalled profile's curves.
- `main()`: `-display`, the `,m` VideoLUT screen of `-d n,m`, `-x` and `-X`
  are X11-only. On Wayland `-x`/`-X` are unknown options, and `-d n,m` is
  read as `-d n`. `-L` points KWin at the installed profile rather than
  loading its vcgt as a generated profile, which would replace the profile
  KWin uses to color manage other applications.

## Instrument resilience

The i1Display Pro used for testing (firmware v1.01, 2011) intermittently loses
reply packets: a USB analyzer showed the host leaving its IN data packets
unacknowledged (seen by Argyll as `-EPROTO`, `ICOM_USBR`). Directly on the
laptop port that hit 28% of transactions; behind a powered USB 2.0 hub, 0.15%,
all absorbed by the hub's retries. The software now rides out such errors:

- `spectro/dispsup.c`, `open_inst_retry()`: creates, connects and initialises
  the instrument for `new_disprd()` (dispcal, dispread, ccxxmake) and
  `disprd_calibration()`, retrying communication, protocol and hardware
  failures 6 times with pauses of 1, 2, 3, 5 and 8 s. A missing Spyder PLD
  pattern isn't retried.
- `disprd_reopen_inst()`: after a communication failure during a reading,
  closes the instrument, waits 2, 5 or 10 s (escalating while reconnects
  fail, up to 3 per reading), finds the instrument again by USB VID:PID or
  serial path, reopens it and re-applies `config_inst_displ()` and the
  target LED. It re-enumerates because `p->ipath` points into the caller's
  path list, which dispcal frees after `new_disprd()`. After the automatic
  reconnects the user is asked as before, and a retry now really retries:
  upstream fell through to "unhandled error" after the prompt. Ambient
  readings (`disprd_ambient()`) are unchanged, since a reopen configures
  display mode.
- `spectro/i1d3.c`, `i1d3_del()`: waits up to 2 s, not 250 ms, for the
  diffuser polling thread, which can be in a 1 s query after a failure.
- `spectro/conv.c`: the Unix `new_athread_reusable()` never set
  `terminate`, so `i1d3_del()`'s fallback called a NULL pointer.
- `spectro/usbio_lx.c`: failed URBs log their errno at debug level 1.

## spectro/dispcal.c, dispread.c, ccxxmake.c

Option handling for `-display`, `-d n,m` and `-n` (override redirect) moved
under `UNIX_X11_DISP`. The ICC header platform stays under `UNIX_X11`.

## plot/plot.c, plot_font.h

- New `UNIX_WAYLAND` section: an xdg-shell toplevel (server-side decoration
  requested) drawn into a POSIX shm buffer (`shm_open` + immediate
  `shm_unlink`, avoiding `memfd_create`'s `_GNU_SOURCE`). It contains a
  Bresenham rasterizer with a square pen and a 5-on/1-off dash for the axes,
  and Hershey stroke text with a cleared background box like
  `XDrawImageString`. The drawing logic mirrors the X11 `DoPlot()` line for
  line, including using `mcols` for symbol colors.
- The connection and window persist between calls like the X11 version. A key
  press ends a wait, as does closing the window, which is then recreated on
  the next call.
- `plot_font.h` is the "Rowman Single" (Hershey Roman Simplex) table copied
  from `render/render.c`, because `libplot` users don't link `librender`.

## Verified (KWin 6.7.5, Sharp 2560x1600 eDP panel, i1Display Pro)

- Patch window: exact size (100 mm -> 699 px at 366 mm / 2560 px), centered,
  above all windows, pixel values exact (screenshot). All patches got
  presentation feedback, and dispcal measured a 17 ms (one frame) update delay.
- Night Light is inhibited while a window is up and released on exit. Neutral
  mode (`dispwin -n`) switched the color/power preference to accuracy and
  restored it on normal exit and on SIGINT.
- Plot windows: graphs, vectors, dashed grid and Hershey text render
  correctly. Keys typed elsewhere go to a newly opened plot window (KWin gives
  it focus) and end its wait, as with X11.
- In sRGB mode, 33 patches (grays and channel ramps) measured with this build
  and with Fedora's X11 Argyll 3.4.1 under XWayland agree to mean dE 0.05,
  max 0.24.
- In ICC mode (EDID-derived matrix/TRC profile, no vcgt), this build's patches
  are unchanged from sRGB mode (mean dE 0.14, max 0.46), confirming the
  output image description tagging. The untagged XWayland build's primaries
  shift by up to dE 53, because KWin color manages its window.

- KWin applies a profile's vcgt to tagged surfaces. With `ref/sRGB.icm` plus
  a vcgt scaling red by 0.8 (made with `iccvcgt -i`; `applycal` bakes curves
  into the TRCs instead, and can't read v4 profiles), white moved from x 0.309
  to 0.264, red dropped to the predicted level and green and blue were
  unchanged. So a VideoLUT can be emulated by having KWin load a generated
  profile.
- The shadow loss in that run came from the base profile's tone curve, not
  the vcgt: plain `sRGB.icm` (sRGB TRC with a linear toe) crushes grays below
  about 40% (6.2% gray at 0.32x), while `ClayRGB1998.icm` (pure gamma 2.199)
  passes them within about 3% (6.2% gray at 0.98x), as does the user's
  EDID-derived profile (parametric gamma 2.2). Apparently KWin describes the
  output to tagged surfaces with a power-law transfer function but encodes
  with the profile's actual TRC. With Clay plus the red-scaling vcgt, red
  dropped to the predicted level at every step and green and blue were
  unchanged. So a VideoLUT can be emulated with a generated profile that has
  an exact gamma 2.2 TRC plus the calibration as its vcgt.

- VideoLUT emulation through Argyll's own paths: `dispread -k cal` matched
  the hand-made vcgt profile to within 0.003 on all 33 patches; `dispwin cal`
  persisted after exit, `dispwin -V` read it back (0.0%), `dispwin -c`
  replaced it with a linear LUT; per-patch native mode (`dispwin -n`) made 18
  LUT loads of about 50 ms each, then restored the settings and deleted the
  generated profiles. Rapid profile switching (about 480 switches during a
  dispread run) didn't disturb the instrument.
- Profile install (`-I`) points KWin at ucmm's installed copy, `-L` keeps it,
  and `-U` returns to sRGB without re-applying the uninstalled curves.
- Instrument open retry recovered from failed opens (1 to 4 retries per run).

- A complete `dispcal -ql -t 6500 -g 2.2` calibration through the emulated
  VideoLUT, recovering from a mid-run dropout: white point error 0.50 dE,
  average neutral error 0.52 dE. With the result loaded, white measured
  0.3122, 0.3297 (6526 K, 0.9 dE from the daylight locus, was 9.4) and
  gamma 2.18.
- Behind a powered hub, a 30 patch dispread with 24 black patches completed
  with no dropouts, and the same dispcal calibration ran with no reconnects
  or open retries: white point error 0.35 dE, average neutral error 0.42 dE;
  calibrated white 0.3121, 0.3289 (0.4 dE from the daylight locus).
- Software LUT while measuring, started from ICC mode: single-channel ramps
  (70-100%) matched sRGB mode to within 0.2% where ICC mode had clipped red
  and blue. A fresh `dispcal -ql -t 5500 -g 2.2 -o` profile has white point
  x 0.3325, y 0.3481 (was 0.311 before the change); verifying it on 57
  patches (grays, primaries and secondaries, ColorChecker) through its
  calibration gave CIEDE2000 average 0.61, maximum 2.09 (near black), gray
  average 1.00, where the earlier profile gave 5.59 and 14.89. The output
  was back in ICC mode with the user's profile afterwards.
- Plot symbols: all six shapes (diagonal and orthogonal cross, square,
  diamond, up and down triangle) draw correctly, in the default plot colors,
  with labels at the X11 offsets and the grid blanked behind the text.
