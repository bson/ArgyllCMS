# Wayland protocol definitions

The Wayland build generates its protocol code from these XML files, copied
unchanged from:

- `wayland-protocols/`: wayland-protocols 1.49
  (https://gitlab.freedesktop.org/wayland/wayland-protocols), MIT licence
  (`COPYING`).
- `plasma-wayland-protocols/`: plasma-wayland-protocols 1.21.0
  (https://invent.kde.org/libraries/plasma-wayland-protocols), MIT-CMU
  licence (`MIT-CMU.txt`).

They're kept here because the color-management protocol and the KDE output
protocols are newer than many distributions' packages (Ubuntu 24.04's, for
one), and so that every build uses the same definitions. To build against a
system's copies instead, pass `-sWLPROTO_SYS=/usr/share/wayland-protocols`
and `-sWLPROTO_KDE=/usr/share/plasma-wayland-protocols` to jam.

To update, copy the same files from newer releases, keeping the folder
layout, and update the versions above.
