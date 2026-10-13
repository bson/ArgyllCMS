# Implementation summary

Details of the Wayland port, relative to the upstream ArgyllCMS 3.5.0 import
(the first commit). Builds contain both the Wayland and the original X11
display code, and each program picks one when it runs.

## Build switches

- `Jamtop`: `USE_WAYLAND ?= true` and `USE_X11 ?= true` (non-Apple Unix),
  independent; both false is an error. `USE_WAYLAND` adds
  `DEFINES += UNIX_WAYLAND`, finds the libdbus include dirs by `GLOB` (jam
  can't run pkg-config), checks for the wayland-protocols and
  plasma-wayland-protocols XML dirs (`WLPROTO_SYS`, `WLPROTO_KDE`, by default
  the copies in `wlproto/`, overridable with `-s`), and links
  `-lwayland-client -ldbus-1`. `USE_X11` finds the X headers and libraries as
  upstream did and links the X libs; when false it adds
  `DEFINES += NO_X11_DISP`. `LibWinH` collects both sets of include dirs.
  `?=` is deliberate so that `-s` on the command line wins.
- `Jamtop`: rule `WaylandProtocol basename : file.xml ;` generates
  `basename.h`/`basename.c` with `wayland-scanner` (client-header,
  private-code) in the current directory.
- `h/aconfig.h`: `UNIX_X11_DISP` is defined when `UNIX_X11` is and
  `NO_X11_DISP` isn't. `UNIX_X11` stays defined on Wayland-only builds because
  the Linux USB/HID code, `USE_UCMM` and the ICC header platform depend on it.
  All Xlib display code is under `UNIX_X11_DISP`.

## Choosing Wayland or X11 at run time

- `dispwin_wayland()` (`dispwin.c`, declared in `dispwin.h`; a constant macro
  in single-backend builds) decides once per process, so displays listed by
  one are never opened by the other: `ARGYLL_WINDOW_SYSTEM=x11|wayland` if
  set; X11 without `WAYLAND_DISPLAY`; Wayland if `wl_check_platform()` passes
  (KWin); otherwise X11 (XWayland on GNOME and other compositors), unless
  `DISPLAY` is unset too, in which case Wayland, so the error names what the
  compositor lacks. That automatic part is `dispwin_wayland_auto()`, which
  `dispwin -Q` also reports.
- Every Wayland block in `dispwin.c` is a run-time branch on it, placed
  before the X11 code and returning or skipping it with `} else`; the X11
  code itself is as upstream. Functions guarded only by `p->wl` or
  `p->mydisplay` NULL checks (`dispwin_del()`, `-L` in `main()`) are left as
  they were.
- `plot.c` decides separately (`plot_wayland()`): the same override and
  `WAYLAND_DISPLAY`/`DISPLAY` tests, then Wayland only if the compositor offers
  `zxdg_decoration_manager_v1`, as without it (GNOME) the plot window would
  have no title bar; X11 through XWayland gets one from the window manager.
  The implementations are `x11_do_plot_imp()` and `wl_do_plot_imp()`, behind
  a `do_plot_imp()` dispatcher. `wlp_connect(quiet)` returns NULL instead of
  exiting when probing.
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
- `wlproto/` holds unchanged copies of the other nine protocol definitions
  used (wayland-protocols 1.49, plasma-wayland-protocols 1.21.0, with their
  licences; see its README), because color-management-v1 and the KDE output
  protocols are newer than many distributions' packages (Ubuntu 24.04's, for
  one, which DisplayCraft's AppImage is built on). The defaults are
  `[ NormPaths wlproto/... ]`, relative to the top directory, where
  wayland-scanner runs; anchoring them like `DESTDIR` made them relative to
  each sub-Jamfile instead, and the scanner couldn't find them.

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

`dispwin -Q` (`dispwin_report_platform()`) describes the session, one
`key: value` per line, and exits 0 if the platform is supported (perhaps
limited) or 1 if not. It runs right after option parsing, before any display
is opened, since opening one fails on a platform the build doesn't support.

    desktop: KDE                      $XDG_CURRENT_DESKTOP, or unknown
    wayland compositor: KWin          or none
    x11 server: XWayland :0           Xorg :0, another vendor's name, or none
    x11 window manager: KWin          or none
    automatic display system: wayland
    display system: x11 (ARGYLL_WINDOW_SYSTEM)
    platform limited: X11 through XWayland :0 on KWin: ...

- The compositor (`wl_compositor_name()`, `dispwin_wl.c`): Wayland has no
  protocol for it, so it's the process at the other end of the socket
  (`SO_PEERCRED`, Linux only, with a local copy of `struct ucred`, which
  glibc only declares with `_GNU_SOURCE`), named from argv[0] in
  `/proc/<pid>/cmdline` (`comm` is cut at 15 characters). For KWin that's
  `kwin_wayland_wrapper`, which owns the listening socket. Known names are
  mapped (KWin, GNOME Shell, Mutter, Sway, Hyprland, Weston, COSMIC); others
  are printed as they are. Only tried with `WAYLAND_DISPLAY` set, as
  libwayland would otherwise try `wayland-0`, which in an X11 session may
  be another session's.
- The X server and window manager (`x11_describe()`, `dispwin.c`): XWayland
  by its `XWAYLAND` extension, Xorg by vendor string; the window manager's
  `_NET_WM_NAME` on the EWMH `_NET_SUPPORTING_WM_CHECK` window, read with
  the null error handler in case that window is stale.
- Single-backend builds print `unknown` for what they can't look into.
- The last line is `dispwin_check_platform()`, which returns 0
  (unsupported), 1 (supported) or 2 (limited). Wayland:
  `wl_check_platform()`, which connects to the compositor and requires
  `wl_compositor`, `wl_subcompositor`, `zwlr_layer_shell_v1`,
  `wp_viewporter`, `wp_single_pixel_buffer_manager_v1`,
  `wp_color_manager_v1`, `kde_output_device_v2` devices and
  `kde_output_management_v2` v8+, naming whatever is missing. X11:
  unsupported if the display won't open; limited through XWayland, as the
  compositor may color manage the test patch window and X11 gamma ramps
  aren't applied; otherwise supported, e.g. `X11 (Xorg :0, window manager
  KWin)`. Windows and macOS builds report supported.

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
  are X11-only. When Wayland is in use, `-x`/`-X` are an error (unknown
  options in a Wayland-only build), `-d n,m` is read as `-d n`, and the
  default display no longer comes from `DISPLAY`'s screen number (also in
  dispcal, dispread and ccxxmake). `-L` points KWin at the installed profile rather than
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

Option handling for `-display`, `-d n,m` and `-n` (override redirect) is
under `UNIX_X11_DISP`; the `DISPLAY` screen-number default is skipped when
`dispwin_wayland()`. The ICC header platform stays under `UNIX_X11`.
The usage line for `-d` is ` -d n[,m]` in every Linux/BSD build (dispwin,
dispcal, dispread, ccxxmake), as upstream's X11 builds print it, so front
ends parse one format; without X11 the `,m` is accepted (`atoi` stops at
the comma) and ignored.

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
