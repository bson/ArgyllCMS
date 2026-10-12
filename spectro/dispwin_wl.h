#ifndef DISPWIN_WL_H

/*
 * Argyll Color Management System
 * Wayland (KWin) display enumeration and test patch window,
 * used by dispwin.c in place of the X11 code on UNIX_WAYLAND builds.
 *
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 */

#ifdef __cplusplus
	extern "C" {
#endif

/* Return a NULL terminated list of the enabled outputs, primary first. */
/* Return NULL on failure. */
disppath **wl_get_displays(void);

/* Connect to the compositor and, unless nowin, put up the test window. */
/* width/height are in mm, hoff/voff in -1.0 .. 1.0 as for new_dispwin(). */
/* Sets p->fdepth etc., p->edid and the window/patch geometry. */
/* If (native & 1), the output is switched to a neutral color mode until */
/* wl_dispwin_restore() is called. */
/* Return nz on failure. */
int wl_dispwin_open(dispwin *p, disppath *disp, double width, double height,
                    double hoff, double voff, int native);

/* Show p->r_rgb[] in the patch area and wait until it has been presented. */
/* Return nz on failure. */
int wl_dispwin_set_color(dispwin *p);

/* Undo output mode and Night Light changes made by wl_dispwin_open(). */
/* Safe to call more than once. */
void wl_dispwin_restore(dispwin *p);

/* Tear down the window and connection. Calls wl_dispwin_restore(). */
void wl_dispwin_close(dispwin *p);

/* Make the compositor use the given ICC profile for this output, */
/* or go back to sRGB if path is NULL. Return nz on failure. */
int wl_dispwin_set_profile(dispwin *p, char *path);

/* Return the ICC profile path the compositor is using for this output */
/* (malloc'd), or NULL if it isn't using one. */
char *wl_dispwin_get_profile_path(dispwin *p);

#ifdef __cplusplus
	}
#endif

#define DISPWIN_WL_H
#endif /* DISPWIN_WL_H */
