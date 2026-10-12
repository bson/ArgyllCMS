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
  sets `fdepth = rdepth = 8`, `edepth = 16`, `nent = ndepth = 0`. With a
  window it then:
  - inhibits KWin Night Light over D-Bus (`org.kde.KWin.NightLight.inhibit`)
    and polls `currentTemperature` until it reaches 6500K (KWin fades). The
    private bus connection is held open because KWin drops an inhibit when
    its owner disconnects.
  - if `native & 1`, switches the output's `color_profile_source` to sRGB and
    `color_power_tradeoff` to accuracy through `kde_output_configuration_v2`,
    saving the old values. This stands in for loading a linear VideoLUT.
  - creates a layer-shell OVERLAY surface (black single-pixel buffer, empty
    input region, no keyboard focus, exclusive zone -1) with a synchronized
    subsurface for the patch, both sized with `wp_viewporter`.
  - tags both surfaces with the output's own image description
    (`wp_color_manager_v1`), so KWin passes values through instead of
    treating them as sRGB.
  - adds an idle inhibitor.
- `wl_dispwin_set_color()`: quantizes `r_rgb` to `fdepth` like the X11 path,
  scales to 32 bits for `create_u32_rgba_buffer`, commits the patch and the
  parent, then waits up to 1s for `wp_presentation` feedback (or a frame
  callback) before returning.
- `wl_dispwin_restore()`: restores the saved color mode and releases Night
  Light. It is called from `restore_display()`, so it also runs from the
  signal handler. Talking to the compositor from the handler can deadlock if
  a signal arrives during the presentation wait, the same hazard Xlib had.
- `wl_dispwin_set_profile()` / `wl_dispwin_get_profile_path()`: set or read
  the output's ICC profile path and source. While neutral mode is active they
  update the saved state, so the restore doesn't undo an install.

## spectro/dispwin.c, dispwin.h

- `disppath`/`dispwin`: `edid`/`edid_len` stay under `UNIX_X11`, X fields move
  to `UNIX_X11_DISP`, `wl_uuid` and `struct _wlwin *wl` are under
  `UNIX_WAYLAND`. `get_a_display()` strdup's `wl_uuid` because of its
  structure copy.
- `dispwin_get_ramdac()` returns NULL and `dispwin_set_ramdac()` fails, so
  `new_dispwin()` reports `noramdac` and callers warn as they already do.
- Profile install/uninstall/get keep the ucmm/colord logic. On Wayland the
  X11 atom work is replaced by pointing KWin at the installed copy
  (`ucmm_get_monitor_profile()` after install), resetting to sRGB on
  uninstall, and falling back to KWin's reported profile path in
  `get_profile`.
- `new_dispwin()`: Wayland block calls `wl_dispwin_open()` and installs the
  signal handlers. `dispwin_del()` calls `wl_dispwin_close()`.
- `main()`: `-display`, the `,m` VideoLUT screen of `-d n,m`, `-x` and `-X`
  are X11-only. On Wayland `-x`/`-X` are unknown options, and `-d n,m` is
  read as `-d n`.

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

## Not yet verified

Nothing that puts a window on screen or changes output state has been run
yet. Display enumeration (names, UUID, EDID) has been checked against the
running KWin 6.7.5.
