
/*
 * Argyll Color Management System
 * Wayland (KWin) display enumeration and test patch window.
 *
 * The patch window is a wlr-layer-shell overlay surface on the chosen
 * output, so no window manager placement or decoration gets involved.
 * Patches are single-pixel buffers scaled with wp_viewporter, which lets
 * the compositor see exact values instead of a dithered or resampled image.
 *
 * Outputs are enumerated with the KDE output device protocol, since that is
 * the only Wayland interface that provides EDID (which ucmm keys profiles
 * on), the ICC profile in use, and a way to change the output color mode.
 *
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <math.h>
#include <unistd.h>
#include <wayland-client.h>
#include <dbus/dbus.h>
#include "copyright.h"
#include "aconfig.h"
#include "icc.h"
#include "numsup.h"
#include "cgats.h"
#include "conv.h"
#include "base64.h"
#include "xdg_bds.h"
#include "dispwin.h"
#include "dispwin_wl.h"
#include "wlp_xdg_shell.h"
#include "wlp_layer_shell.h"
#include "wlp_viewporter.h"
#include "wlp_single_pixel.h"
#include "wlp_presentation.h"
#include "wlp_idle_inhibit.h"
#include "wlp_color_mgmt.h"
#include "wlp_kde_output_device.h"
#include "wlp_kde_output_mgmt.h"

#define errout stderr
#define debugr(xx) if (p->ddebug) fprintf(errout, xx )
#define debugr2(xx) if (p->ddebug) fprintf xx
#define debugrr2(xx) if (callback_ddebug) fprintf xx

#define WL_MAX_DEVS 32
#define WL_MAX_OUTS 32

/* How long to wait for the compositor before giving up. A patch that */
/* isn't presented within this time is shown anyway, so it's only a guard */
/* against a stalled compositor, not a timing value. */
#define WL_PRESENT_TIMEOUT 1000
#define WL_CONFIG_TIMEOUT 5000

/* VideoLUT emulation entries, matching the 8 bit frame buffer we assume */
#define WL_LUT_SIZE 256

/* Generated VideoLUT profiles, under the user's XDG data directory */
#define WL_LUT_DIR "ArgyllCMS/wayland"

/* KWin color_profile_source and color_power_tradeoff values */
#define KDE_CPS_SRGB 0
#define KDE_CPS_ICC 1
#define KDE_CPT_ACCURACY 1

typedef struct _wlmode {
	struct kde_output_device_mode_v2 *mode;
	int w, h;
	struct _wlmode *next;
} wlmode;

typedef struct _wlwin wlwin;

typedef struct {
	struct _wlwin *w;
	struct kde_output_device_v2 *dev;
	char *name;			/* Connector name, e.g. "DP-1" */
	char *uuid;
	char *make, *model;
	char *edid_b64;
	char *icc_path;
	int x, y;			/* Position in compositor (logical) space */
	int pw_mm, ph_mm;	/* Physical size */
	int transform;
	struct kde_output_device_mode_v2 *cur_mode;
	double scale;
	int enabled;
	int removed;
	int priority;		/* 1 = primary, 0 = not reported */
	int hdr;
	int cps;			/* color_profile_source, -1 if not reported */
	int cpt;			/* color_power_tradeoff, -1 if not reported */
} wldev;

typedef struct {
	struct wl_output *out;
	uint32_t gname;
	char *name;
} wlout;

struct _wlwin {
	struct wl_display *dpy;
	struct wl_registry *reg;
	struct wl_compositor *comp;
	struct wl_subcompositor *subcomp;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct wp_viewporter *viewporter;
	struct wp_single_pixel_buffer_manager_v1 *spb;
	struct wp_presentation *presentation;
	struct zwp_idle_inhibit_manager_v1 *idle_mgr;
	struct wp_color_manager_v1 *cmgr;
	struct kde_output_device_registry_v2 *kreg;
	struct kde_output_management_v2 *kmgmt;
	uint32_t kmgmt_ver;

	wldev *devs[WL_MAX_DEVS];
	int ndevs;
	wlout outs[WL_MAX_OUTS];
	int nouts;
	wlmode *modes;		/* All mode records, for freeing */

	wldev *dev;			/* The output we're using */
	struct wl_output *out;

	/* Test window */
	struct wl_surface *base;		/* Black background, layer surface */
	struct zwlr_layer_surface_v1 *layer;
	struct wp_viewport *base_vp;
	struct wl_buffer *base_buf;
	struct wl_surface *patch;		/* Patch color, subsurface of base */
	struct wl_subsurface *patch_sub;
	struct wp_viewport *patch_vp;
	struct wl_buffer *patch_buf;
	struct zwp_idle_inhibitor_v1 *inhibitor;
	struct wp_color_management_surface_v1 *base_cms, *patch_cms;
	struct wp_image_description_v1 *out_desc;
	volatile int configured, closed;
	uint32_t conf_w, conf_h;

	/* Output state to restore on exit, saved at open. Persistent changes */
	/* (profile install, persistent LUT load) update it. */
	int saved_cps, saved_cpt;
	char *saved_icc;
	int cpt_changed;	/* nz if we temporarily changed color_power_tradeoff */
	int cps_changed;	/* nz if the saved profile source must be put back */
	int lut_active;		/* nz if a temporary generated LUT profile is in use */
	char *gen_path;		/* The generated profile in use, if any */
	double base_lut[3][WL_LUT_SIZE];	/* LUT the saved state gives */
	int sw_lut;			/* nz while the LUT is applied to patches by us */
	double cur_lut[3][WL_LUT_SIZE];		/* That LUT */

	DBusConnection *bus;	/* Held open, because KWin drops an inhibit on disconnect */
	dbus_uint32_t nl_cookie;
	int nl_inhibited;
};

/* ------------------------------------------------------------------ */
/* Event handling helpers */

/* Dispatch events until *flag is nz or timeout_ms elapses. */
/* Return nz on timeout or connection error. */
static int wl_wait_flag(struct wl_display *dpy, volatile int *flag, int timeout_ms) {
	unsigned int start = msec_time();

	while (!*flag) {
		struct pollfd pfd;
		int left = timeout_ms - (int)(msec_time() - start);

		if (left <= 0)
			return 1;

		while (wl_display_prepare_read(dpy) != 0) {
			if (wl_display_dispatch_pending(dpy) < 0)
				return 1;
			if (*flag)
				return 0;
		}
		if (wl_display_flush(dpy) < 0 && errno != EAGAIN) {
			wl_display_cancel_read(dpy);
			return 1;
		}
		pfd.fd = wl_display_get_fd(dpy);
		pfd.events = POLLIN;
		pfd.revents = 0;
		if (poll(&pfd, 1, left) <= 0) {
			wl_display_cancel_read(dpy);
			continue;
		}
		if (wl_display_read_events(dpy) < 0)
			return 1;
		if (wl_display_dispatch_pending(dpy) < 0)
			return 1;
	}
	return 0;
}

static char *wl_strdup_or_null(const char *s) {
	if (s == NULL)
		return NULL;
	return strdup(s);
}

static void wl_replace_str(char **dst, const char *src) {
	free(*dst);
	*dst = wl_strdup_or_null(src);
}

/* ------------------------------------------------------------------ */
/* KDE output device mode */

static void mode_size(void *data, struct kde_output_device_mode_v2 *m, int32_t w, int32_t h) {
	wlmode *md = (wlmode *)data;
	md->w = w;
	md->h = h;
}
static void mode_refresh(void *data, struct kde_output_device_mode_v2 *m, int32_t refresh) { }
static void mode_preferred(void *data, struct kde_output_device_mode_v2 *m) { }
static void mode_removed(void *data, struct kde_output_device_mode_v2 *m) { }
static void mode_flags(void *data, struct kde_output_device_mode_v2 *m, uint32_t flags) { }

static const struct kde_output_device_mode_v2_listener mode_listener = {
	mode_size, mode_refresh, mode_preferred, mode_removed, mode_flags
};

/* ------------------------------------------------------------------ */
/* KDE output device */

static void dev_geometry(void *data, struct kde_output_device_v2 *d, int32_t x, int32_t y,
                         int32_t pw, int32_t ph, int32_t subpixel, const char *make,
                         const char *model, int32_t transform) {
	wldev *dv = (wldev *)data;
	dv->x = x;
	dv->y = y;
	dv->pw_mm = pw;
	dv->ph_mm = ph;
	dv->transform = transform;
	wl_replace_str(&dv->make, make);
	wl_replace_str(&dv->model, model);
}
static void dev_current_mode(void *data, struct kde_output_device_v2 *d,
                             struct kde_output_device_mode_v2 *mode) {
	((wldev *)data)->cur_mode = mode;
}
static void dev_mode(void *data, struct kde_output_device_v2 *d,
                     struct kde_output_device_mode_v2 *mode) {
	wlwin *w = ((wldev *)data)->w;
	wlmode *md;

	if ((md = calloc(1, sizeof(wlmode))) == NULL) {
		wl_proxy_destroy((struct wl_proxy *)mode);
		return;
	}
	md->mode = mode;
	md->next = w->modes;
	w->modes = md;
	kde_output_device_mode_v2_add_listener(mode, &mode_listener, md);
}
static void dev_done(void *data, struct kde_output_device_v2 *d) { }
static void dev_scale(void *data, struct kde_output_device_v2 *d, wl_fixed_t factor) {
	((wldev *)data)->scale = wl_fixed_to_double(factor);
}
static void dev_edid(void *data, struct kde_output_device_v2 *d, const char *raw) {
	wl_replace_str(&((wldev *)data)->edid_b64, raw);
}
static void dev_enabled(void *data, struct kde_output_device_v2 *d, int32_t enabled) {
	((wldev *)data)->enabled = enabled;
}
static void dev_uuid(void *data, struct kde_output_device_v2 *d, const char *uuid) {
	wl_replace_str(&((wldev *)data)->uuid, uuid);
}
static void dev_serial_number(void *data, struct kde_output_device_v2 *d, const char *s) { }
static void dev_eisa_id(void *data, struct kde_output_device_v2 *d, const char *s) { }
static void dev_capabilities(void *data, struct kde_output_device_v2 *d, uint32_t flags) { }
static void dev_overscan(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_vrr_policy(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_rgb_range(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_name(void *data, struct kde_output_device_v2 *d, const char *name) {
	wl_replace_str(&((wldev *)data)->name, name);
}
static void dev_hdr(void *data, struct kde_output_device_v2 *d, uint32_t on) {
	((wldev *)data)->hdr = on;
}
static void dev_sdr_brightness(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_wcg(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_auto_rotate(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_icc_profile_path(void *data, struct kde_output_device_v2 *d, const char *path) {
	wl_replace_str(&((wldev *)data)->icc_path, path);
}
static void dev_brightness_metadata(void *data, struct kde_output_device_v2 *d,
                                    uint32_t a, uint32_t b, uint32_t c) { }
static void dev_brightness_overrides(void *data, struct kde_output_device_v2 *d,
                                     int32_t a, int32_t b, int32_t c) { }
static void dev_sdr_gamut_wideness(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_color_profile_source(void *data, struct kde_output_device_v2 *d, uint32_t v) {
	((wldev *)data)->cps = (int)v;
}
static void dev_brightness(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_color_power_tradeoff(void *data, struct kde_output_device_v2 *d, uint32_t v) {
	((wldev *)data)->cpt = (int)v;
}
static void dev_dimming(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_replication_source(void *data, struct kde_output_device_v2 *d, const char *s) { }
static void dev_ddc_ci_allowed(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_max_bpc(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_max_bpc_range(void *data, struct kde_output_device_v2 *d, uint32_t a, uint32_t b) { }
static void dev_auto_max_bpc_limit(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_edr_policy(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_sharpness(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_priority(void *data, struct kde_output_device_v2 *d, uint32_t v) {
	((wldev *)data)->priority = (int)v;
}
static void dev_auto_brightness(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_removed(void *data, struct kde_output_device_v2 *d) {
	((wldev *)data)->removed = 1;
}
static void dev_hdr_icc_profile_path(void *data, struct kde_output_device_v2 *d, const char *s) { }
static void dev_hdr_color_profile_source(void *data, struct kde_output_device_v2 *d, uint32_t v) { }
static void dev_abm_level(void *data, struct kde_output_device_v2 *d, uint32_t v) { }

static const struct kde_output_device_v2_listener dev_listener = {
	dev_geometry, dev_current_mode, dev_mode, dev_done, dev_scale, dev_edid,
	dev_enabled, dev_uuid, dev_serial_number, dev_eisa_id, dev_capabilities,
	dev_overscan, dev_vrr_policy, dev_rgb_range, dev_name, dev_hdr,
	dev_sdr_brightness, dev_wcg, dev_auto_rotate, dev_icc_profile_path,
	dev_brightness_metadata, dev_brightness_overrides, dev_sdr_gamut_wideness,
	dev_color_profile_source, dev_brightness, dev_color_power_tradeoff,
	dev_dimming, dev_replication_source, dev_ddc_ci_allowed, dev_max_bpc,
	dev_max_bpc_range, dev_auto_max_bpc_limit, dev_edr_policy, dev_sharpness,
	dev_priority, dev_auto_brightness, dev_removed, dev_hdr_icc_profile_path,
	dev_hdr_color_profile_source, dev_abm_level
};

static void wl_add_dev(wlwin *w, struct kde_output_device_v2 *d) {
	wldev *dv;

	if (w->ndevs >= WL_MAX_DEVS || (dv = calloc(1, sizeof(wldev))) == NULL) {
		kde_output_device_v2_destroy(d);
		return;
	}
	dv->w = w;
	dv->dev = d;
	dv->scale = 1.0;
	dv->cps = -1;
	dv->cpt = -1;
	w->devs[w->ndevs++] = dv;
	kde_output_device_v2_add_listener(d, &dev_listener, dv);
}

static void kreg_finished(void *data, struct kde_output_device_registry_v2 *r) { }
static void kreg_output(void *data, struct kde_output_device_registry_v2 *r,
                        struct kde_output_device_v2 *d) {
	wl_add_dev((wlwin *)data, d);
}

static const struct kde_output_device_registry_v2_listener kreg_listener = {
	kreg_finished, kreg_output
};

/* ------------------------------------------------------------------ */
/* wl_output, only used to match a KDE device to the wl_output that */
/* layer-shell needs, by connector name. */

static void out_geometry(void *data, struct wl_output *o, int32_t x, int32_t y, int32_t pw,
                         int32_t ph, int32_t sp, const char *make, const char *model,
                         int32_t tr) { }
static void out_mode(void *data, struct wl_output *o, uint32_t flags, int32_t w, int32_t h,
                     int32_t r) { }
static void out_done(void *data, struct wl_output *o) { }
static void out_scale(void *data, struct wl_output *o, int32_t f) { }
static void out_name(void *data, struct wl_output *o, const char *name) {
	wl_replace_str(&((wlout *)data)->name, name);
}
static void out_description(void *data, struct wl_output *o, const char *desc) { }

static const struct wl_output_listener out_listener = {
	out_geometry, out_mode, out_done, out_scale, out_name, out_description
};

/* ------------------------------------------------------------------ */
/* Registry */

static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver) {
	wlwin *w = (wlwin *)data;

#define WL_MIN(a,b) ((a) < (b) ? (a) : (b))
	if (strcmp(iface, wl_compositor_interface.name) == 0) {
		w->comp = wl_registry_bind(reg, name, &wl_compositor_interface, WL_MIN(ver, 4));
	} else if (strcmp(iface, wl_subcompositor_interface.name) == 0) {
		w->subcomp = wl_registry_bind(reg, name, &wl_subcompositor_interface, 1);
	} else if (strcmp(iface, wl_output_interface.name) == 0) {
		/* The name event needs v4 */
		if (ver >= 4 && w->nouts < WL_MAX_OUTS) {
			wlout *o = &w->outs[w->nouts++];
			o->gname = name;
			o->out = wl_registry_bind(reg, name, &wl_output_interface, 4);
			wl_output_add_listener(o->out, &out_listener, o);
		}
	} else if (strcmp(iface, zwlr_layer_shell_v1_interface.name) == 0) {
		w->layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
		                                  WL_MIN(ver, 4));
	} else if (strcmp(iface, wp_viewporter_interface.name) == 0) {
		w->viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	} else if (strcmp(iface, wp_single_pixel_buffer_manager_v1_interface.name) == 0) {
		w->spb = wl_registry_bind(reg, name, &wp_single_pixel_buffer_manager_v1_interface, 1);
	} else if (strcmp(iface, wp_presentation_interface.name) == 0) {
		w->presentation = wl_registry_bind(reg, name, &wp_presentation_interface, 1);
	} else if (strcmp(iface, zwp_idle_inhibit_manager_v1_interface.name) == 0) {
		w->idle_mgr = wl_registry_bind(reg, name, &zwp_idle_inhibit_manager_v1_interface, 1);
	} else if (strcmp(iface, wp_color_manager_v1_interface.name) == 0) {
		w->cmgr = wl_registry_bind(reg, name, &wp_color_manager_v1_interface, WL_MIN(ver, 2));
	} else if (strcmp(iface, kde_output_device_registry_v2_interface.name) == 0) {
		/* KWin rejects a registry bound below v21. Binding no higher than */
		/* our listeners know about keeps newer events from arriving. */
		if (ver >= 21) {
			w->kreg = wl_registry_bind(reg, name, &kde_output_device_registry_v2_interface, 21);
			kde_output_device_registry_v2_add_listener(w->kreg, &kreg_listener, w);
		}
	} else if (strcmp(iface, kde_output_device_v2_interface.name) == 0) {
		/* Older KWin advertises each device as a global instead */
		wl_add_dev(w, wl_registry_bind(reg, name, &kde_output_device_v2_interface,
		                               WL_MIN(ver, 20)));
	} else if (strcmp(iface, kde_output_management_v2_interface.name) == 0) {
		w->kmgmt_ver = WL_MIN(ver, 21);
		w->kmgmt = wl_registry_bind(reg, name, &kde_output_management_v2_interface,
		                            w->kmgmt_ver);
	}
#undef WL_MIN
}

static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t name) { }

static const struct wl_registry_listener reg_listener = {
	reg_global, reg_global_remove
};

/* ------------------------------------------------------------------ */
/* Connection setup/teardown */

static void wl_free_conn(wlwin *w);
static void wl_read_lut(dispwin *p, double lut[3][WL_LUT_SIZE], char *path);

/* Connect, bind globals and collect the initial output device state. */
static wlwin *wl_new_conn(void) {
	wlwin *w;
	int i;

	if ((w = calloc(1, sizeof(wlwin))) == NULL)
		return NULL;

	if ((w->dpy = wl_display_connect(NULL)) == NULL) {
		free(w);
		return NULL;
	}
	w->reg = wl_display_get_registry(w->dpy);
	wl_registry_add_listener(w->reg, &reg_listener, w);

	/* 1: globals, 2: devices announced and their properties, */
	/* 3: mode objects' sizes */
	for (i = 0; i < 3; i++) {
		if (wl_display_roundtrip(w->dpy) < 0) {
			wl_free_conn(w);
			return NULL;
		}
	}
	return w;
}

static void wl_free_conn(wlwin *w) {
	int i;

	if (w == NULL)
		return;

	while (w->modes != NULL) {
		wlmode *md = w->modes;
		w->modes = md->next;
		wl_proxy_destroy((struct wl_proxy *)md->mode);
		free(md);
	}
	for (i = 0; i < w->ndevs; i++) {
		wldev *dv = w->devs[i];
		kde_output_device_v2_destroy(dv->dev);
		free(dv->name);
		free(dv->uuid);
		free(dv->make);
		free(dv->model);
		free(dv->edid_b64);
		free(dv->icc_path);
		free(dv);
	}
	for (i = 0; i < w->nouts; i++) {
		wl_output_destroy(w->outs[i].out);
		free(w->outs[i].name);
	}
	if (w->kreg != NULL)
		kde_output_device_registry_v2_destroy(w->kreg);
	if (w->kmgmt != NULL)
		kde_output_management_v2_destroy(w->kmgmt);
	if (w->cmgr != NULL)
		wp_color_manager_v1_destroy(w->cmgr);
	if (w->idle_mgr != NULL)
		zwp_idle_inhibit_manager_v1_destroy(w->idle_mgr);
	if (w->presentation != NULL)
		wp_presentation_destroy(w->presentation);
	if (w->spb != NULL)
		wp_single_pixel_buffer_manager_v1_destroy(w->spb);
	if (w->viewporter != NULL)
		wp_viewporter_destroy(w->viewporter);
	if (w->layer_shell != NULL)
		zwlr_layer_shell_v1_destroy(w->layer_shell);
	if (w->subcomp != NULL)
		wl_subcompositor_destroy(w->subcomp);
	if (w->comp != NULL)
		wl_compositor_destroy(w->comp);
	if (w->reg != NULL)
		wl_registry_destroy(w->reg);
	wl_display_disconnect(w->dpy);
	free(w);
}

/* Mode size of a device, rotated to match its logical orientation */
static void wl_dev_mode_size(wldev *dv, int *mw, int *mh, int *pw_mm, int *ph_mm) {
	wlmode *md = NULL;

	if (dv->cur_mode != NULL)
		md = (wlmode *)wl_proxy_get_user_data((struct wl_proxy *)dv->cur_mode);

	*mw = md != NULL ? md->w : 0;
	*mh = md != NULL ? md->h : 0;
	*pw_mm = dv->pw_mm;
	*ph_mm = dv->ph_mm;

	/* Odd transforms are 90 and 270 degree rotations */
	if (dv->transform & 1) {
		int t;
		t = *mw; *mw = *mh; *mh = t;
		t = *pw_mm; *pw_mm = *ph_mm; *ph_mm = t;
	}
}

/* Usable devices in display order: primary first, unreported priority last */
static int wl_sorted_devs(wlwin *w, wldev **list) {
	int i, j, n = 0;

	for (i = 0; i < w->ndevs; i++) {
		if (w->devs[i]->enabled && !w->devs[i]->removed && w->devs[i]->name != NULL)
			list[n++] = w->devs[i];
	}
	for (i = 1; i < n; i++) {
		wldev *t = list[i];
		int pt = t->priority > 0 ? t->priority : 0x7fffffff;
		for (j = i; j > 0; j--) {
			int pj = list[j-1]->priority > 0 ? list[j-1]->priority : 0x7fffffff;
			if (pj <= pt)
				break;
			list[j] = list[j-1];
		}
		list[j] = t;
	}
	return n;
}

static int wl_decode_edid(wldev *dv, unsigned char **edid, int *edid_len) {
	int len;

	*edid = NULL;
	*edid_len = 0;
	if (dv->edid_b64 == NULL || dv->edid_b64[0] == '\000')
		return 0;
	if ((*edid = malloc(DBASE64LEN(strlen(dv->edid_b64)) + 1)) == NULL)
		return 1;
	dbase64(&len, *edid, dv->edid_b64);
	/* ucmm only understands the base block plus one extension */
	if (len > 256)
		len = 256;
	*edid_len = len;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Platform check */

int wl_check_platform(char *msg, int len) {
	wlwin *w;
	char missing[400] = "";
	int ok;

#define WL_NEED(cond, what) \
	if (!(cond)) { \
		if (missing[0] != '\000') \
			strncat(missing, ", ", sizeof(missing) - strlen(missing) - 1); \
		strncat(missing, what, sizeof(missing) - strlen(missing) - 1); \
	}

	if ((w = wl_new_conn()) == NULL) {
		snprintf(msg, len, "can't connect to a Wayland compositor (is this a Wayland session?)");
		return 0;
	}
	/* The test patch window */
	WL_NEED(w->comp != NULL, "wl_compositor");
	WL_NEED(w->subcomp != NULL, "wl_subcompositor");
	WL_NEED(w->layer_shell != NULL, "zwlr_layer_shell_v1");
	WL_NEED(w->viewporter != NULL, "wp_viewporter");
	WL_NEED(w->spb != NULL, "wp_single_pixel_buffer_manager_v1");
	/* Without it, the compositor would color manage the test values */
	WL_NEED(w->cmgr != NULL, "wp_color_manager_v1");
	/* Displays with EDID, profiles, and VideoLUT emulation: KWin only */
	WL_NEED(w->ndevs > 0, "kde_output_device_v2");
	WL_NEED(w->kmgmt != NULL && w->kmgmt_ver >= 8, "kde_output_management_v2 (version 8 or later)");
#undef WL_NEED

	ok = missing[0] == '\000';
	if (ok)
		snprintf(msg, len, "KDE Plasma (KWin) on Wayland");
	else
		snprintf(msg, len, "the Wayland compositor lacks %s, which only KDE Plasma's KWin provides "
		                   "together", missing);
	wl_free_conn(w);
	return ok;
}

/* ------------------------------------------------------------------ */
/* Enumeration */

disppath **wl_get_displays(void) {
	wlwin *w;
	wldev *list[WL_MAX_DEVS];
	disppath **disps;
	int i, n;

	if ((w = wl_new_conn()) == NULL) {
		debugrr2((errout, "wl_get_displays: can't connect to the Wayland compositor\n"));
		return NULL;
	}
	if (w->ndevs == 0) {
		/* Without the KDE output device protocol there's no EDID or */
		/* physical size, so profile association can't work. */
		debugrr2((errout, "wl_get_displays: compositor has no kde_output_device_v2 "
		                  "(only KWin is supported)\n"));
		wl_free_conn(w);
		return NULL;
	}

	n = wl_sorted_devs(w, list);

	if ((disps = calloc(n + 1, sizeof(disppath *))) == NULL) {
		wl_free_conn(w);
		return NULL;
	}

	for (i = 0; i < n; i++) {
		wldev *dv = list[i];
		int mw, mh, pw_mm, ph_mm;
		char desc[300];

		if ((disps[i] = calloc(1, sizeof(disppath))) == NULL)
			goto fail;

		wl_dev_mode_size(dv, &mw, &mh, &pw_mm, &ph_mm);
		disps[i]->sx = dv->x;
		disps[i]->sy = dv->y;
		disps[i]->sw = (int)(mw / dv->scale + 0.5);
		disps[i]->sh = (int)(mh / dv->scale + 0.5);

		if (dv->make != NULL && dv->model != NULL && dv->model[0] != '\000')
			snprintf(desc, sizeof(desc), "Monitor %d, Output %s (%s %s) at %d, %d, width %d, height %d",
			         i+1, dv->name, dv->make, dv->model,
			         disps[i]->sx, disps[i]->sy, disps[i]->sw, disps[i]->sh);
		else
			snprintf(desc, sizeof(desc), "Monitor %d, Output %s at %d, %d, width %d, height %d",
			         i+1, dv->name, disps[i]->sx, disps[i]->sy, disps[i]->sw, disps[i]->sh);

		if ((disps[i]->name = strdup(dv->name)) == NULL
		 || (disps[i]->description = strdup(desc)) == NULL)
			goto fail;
		if (dv->uuid != NULL && (disps[i]->wl_uuid = strdup(dv->uuid)) == NULL)
			goto fail;
		if (wl_decode_edid(dv, &disps[i]->edid, &disps[i]->edid_len))
			goto fail;

		debugrr2((errout, "wl_get_displays: %s uuid %s priority %d scale %f\n",
		          desc, dv->uuid != NULL ? dv->uuid : "(none)", dv->priority, dv->scale));
	}
	wl_free_conn(w);
	return disps;

  fail:
	free_disppaths(disps);
	wl_free_conn(w);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Output configuration changes */

typedef struct {
	volatile int done;
	int failed;
	char reason[200];
} wlcfgres;

static void cfg_applied(void *data, struct kde_output_configuration_v2 *c) {
	((wlcfgres *)data)->done = 1;
}
static void cfg_failed(void *data, struct kde_output_configuration_v2 *c) {
	((wlcfgres *)data)->failed = 1;
	((wlcfgres *)data)->done = 1;
}
static void cfg_failure_reason(void *data, struct kde_output_configuration_v2 *c,
                               const char *reason) {
	wlcfgres *r = (wlcfgres *)data;
	strncpy(r->reason, reason, sizeof(r->reason)-1);
}

static const struct kde_output_configuration_v2_listener cfg_listener = {
	cfg_applied, cfg_failed, cfg_failure_reason
};

/* Apply a change to our output. Pass -1 / NULL to leave a setting alone. */
/* Return nz on failure. */
static int wl_apply_config(dispwin *p, int cps, int cpt, char *icc_path) {
	wlwin *w = p->wl;
	struct kde_output_configuration_v2 *cfg;
	wlcfgres res;

	if (w->kmgmt == NULL) {
		debugr("wl_apply_config: no kde_output_management_v2\n");
		return 1;
	}
	if ((icc_path != NULL && w->kmgmt_ver < 6)
	 || (cps >= 0 && w->kmgmt_ver < 8)
	 || (cpt >= 0 && w->kmgmt_ver < 10)) {
		debugr2((errout, "wl_apply_config: kde_output_management_v2 v%d is too old\n",
		         w->kmgmt_ver));
		return 1;
	}

	memset(&res, 0, sizeof(res));
	cfg = kde_output_management_v2_create_configuration(w->kmgmt);
	kde_output_configuration_v2_add_listener(cfg, &cfg_listener, &res);
	if (icc_path != NULL)
		kde_output_configuration_v2_set_icc_profile_path(cfg, w->dev->dev, icc_path);
	if (cps >= 0)
		kde_output_configuration_v2_set_color_profile_source(cfg, w->dev->dev, cps);
	if (cpt >= 0)
		kde_output_configuration_v2_set_color_power_tradeoff(cfg, w->dev->dev, cpt);
	kde_output_configuration_v2_apply(cfg);

	if (wl_wait_flag(w->dpy, &res.done, WL_CONFIG_TIMEOUT)) {
		debugr("wl_apply_config: timed out waiting for the compositor\n");
		kde_output_configuration_v2_destroy(cfg);
		return 1;
	}
	kde_output_configuration_v2_destroy(cfg);

	if (res.failed) {
		debugr2((errout, "wl_apply_config: compositor refused: %s\n", res.reason));
		return 1;
	}
	/* Pick up the updated device properties */
	wl_display_roundtrip(w->dpy);
	return 0;
}

/* ------------------------------------------------------------------ */
/* KWin Night Light, via D-Bus. It would otherwise shift the white point */
/* of everything we measure. */

#define NL_SERVICE "org.kde.KWin.NightLight"
#define NL_PATH "/org/kde/KWin/NightLight"
#define NL_IFACE "org.kde.KWin.NightLight"
#define NL_NEUTRAL_TEMP 6500
#define NL_SETTLE_TIMEOUT 5000

/* Return the current color temperature, or -1 if unavailable */
static int nl_current_temp(DBusConnection *bus) {
	DBusMessage *msg, *reply;
	DBusMessageIter it, var;
	const char *iface = NL_IFACE, *prop = "currentTemperature";
	dbus_uint32_t temp;
	int rv = -1;

	if ((msg = dbus_message_new_method_call(NL_SERVICE, NL_PATH,
	                     "org.freedesktop.DBus.Properties", "Get")) == NULL)
		return -1;
	dbus_message_append_args(msg, DBUS_TYPE_STRING, &iface, DBUS_TYPE_STRING, &prop,
	                         DBUS_TYPE_INVALID);
	reply = dbus_connection_send_with_reply_and_block(bus, msg, 1000, NULL);
	dbus_message_unref(msg);
	if (reply == NULL)
		return -1;
	if (dbus_message_iter_init(reply, &it)
	 && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_VARIANT) {
		dbus_message_iter_recurse(&it, &var);
		if (dbus_message_iter_get_arg_type(&var) == DBUS_TYPE_UINT32) {
			dbus_message_iter_get_basic(&var, &temp);
			rv = (int)temp;
		}
	}
	dbus_message_unref(reply);
	return rv;
}

static void nl_inhibit(dispwin *p) {
	wlwin *w = p->wl;
	DBusMessage *msg, *reply;
	DBusError err;
	unsigned int start;
	int temp;

	dbus_error_init(&err);
	if ((w->bus = dbus_bus_get_private(DBUS_BUS_SESSION, &err)) == NULL) {
		debugr2((errout, "nl_inhibit: no session bus: %s\n", err.message));
		dbus_error_free(&err);
		return;
	}
	dbus_connection_set_exit_on_disconnect(w->bus, FALSE);

	if ((msg = dbus_message_new_method_call(NL_SERVICE, NL_PATH, NL_IFACE, "inhibit")) == NULL)
		return;
	reply = dbus_connection_send_with_reply_and_block(w->bus, msg, 1000, &err);
	dbus_message_unref(msg);
	if (reply == NULL) {
		/* Not running KWin, or Night Light isn't available */
		debugr2((errout, "nl_inhibit: %s\n", err.message));
		dbus_error_free(&err);
		return;
	}
	if (dbus_message_get_args(reply, &err, DBUS_TYPE_UINT32, &w->nl_cookie, DBUS_TYPE_INVALID))
		w->nl_inhibited = 1;
	else
		dbus_error_free(&err);
	dbus_message_unref(reply);

	if (!w->nl_inhibited)
		return;
	debugr2((errout, "nl_inhibit: inhibited Night Light, cookie %u\n", w->nl_cookie));

	/* KWin fades back to neutral rather than switching instantly */
	start = msec_time();
	while ((temp = nl_current_temp(w->bus)) >= 0 && temp != NL_NEUTRAL_TEMP) {
		if ((int)(msec_time() - start) > NL_SETTLE_TIMEOUT) {
			warning("Night Light is still at %dK after inhibiting it", temp);
			break;
		}
		msec_sleep(100);
	}
}

static void nl_uninhibit(dispwin *p) {
	wlwin *w = p->wl;

	if (w->bus == NULL)
		return;

	if (w->nl_inhibited) {
		DBusMessage *msg;
		if ((msg = dbus_message_new_method_call(NL_SERVICE, NL_PATH, NL_IFACE,
		                                        "uninhibit")) != NULL) {
			DBusMessage *reply;
			dbus_message_append_args(msg, DBUS_TYPE_UINT32, &w->nl_cookie, DBUS_TYPE_INVALID);
			reply = dbus_connection_send_with_reply_and_block(w->bus, msg, 1000, NULL);
			if (reply != NULL)
				dbus_message_unref(reply);
			dbus_message_unref(msg);
		}
		w->nl_inhibited = 0;
		debugr("nl_uninhibit: released Night Light\n");
	}
	dbus_connection_close(w->bus);
	dbus_connection_unref(w->bus);
	w->bus = NULL;
}

/* ------------------------------------------------------------------ */
/* Test window */

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial,
                            uint32_t width, uint32_t height) {
	wlwin *w = (wlwin *)data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	w->conf_w = width;
	w->conf_h = height;
	w->configured = 1;
}
static void layer_closed(void *data, struct zwlr_layer_surface_v1 *ls) {
	((wlwin *)data)->closed = 1;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	layer_configure, layer_closed
};

typedef struct {
	volatile int done;
	int presented;
} wlpres;

static void fb_sync_output(void *data, struct wp_presentation_feedback *f, struct wl_output *o) { }
static void fb_presented(void *data, struct wp_presentation_feedback *f, uint32_t sh,
                         uint32_t sl, uint32_t ns, uint32_t refresh, uint32_t qh,
                         uint32_t ql, uint32_t flags) {
	((wlpres *)data)->presented = 1;
	((wlpres *)data)->done = 1;
}
static void fb_discarded(void *data, struct wp_presentation_feedback *f) {
	((wlpres *)data)->done = 1;
}

static const struct wp_presentation_feedback_listener fb_listener = {
	fb_sync_output, fb_presented, fb_discarded
};

static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
	((wlpres *)data)->presented = 1;
	((wlpres *)data)->done = 1;
}

static const struct wl_callback_listener frame_listener = { frame_done };

typedef struct {
	volatile int done;
	int failed;
} wldescres;

static void desc_failed(void *data, struct wp_image_description_v1 *d, uint32_t cause,
                        const char *msg) {
	((wldescres *)data)->failed = 1;
	((wldescres *)data)->done = 1;
}
static void desc_ready(void *data, struct wp_image_description_v1 *d, uint32_t id) {
	((wldescres *)data)->done = 1;
}
static void desc_ready2(void *data, struct wp_image_description_v1 *d, uint32_t hi, uint32_t lo) {
	((wldescres *)data)->done = 1;
}

static const struct wp_image_description_v1_listener desc_listener = {
	desc_failed, desc_ready, desc_ready2
};

static void cm_supported_intent(void *data, struct wp_color_manager_v1 *m, uint32_t v) { }
static void cm_supported_feature(void *data, struct wp_color_manager_v1 *m, uint32_t v) { }
static void cm_supported_tf_named(void *data, struct wp_color_manager_v1 *m, uint32_t v) { }
static void cm_supported_primaries_named(void *data, struct wp_color_manager_v1 *m, uint32_t v) { }
static void cm_done(void *data, struct wp_color_manager_v1 *m) { }

static const struct wp_color_manager_v1_listener cm_listener = {
	cm_supported_intent, cm_supported_feature, cm_supported_tf_named,
	cm_supported_primaries_named, cm_done
};

/* Tag our surfaces with the output's own image description, so that the */
/* compositor passes the test values through instead of treating them as */
/* sRGB and converting them to the output's profile. That matches X11, */
/* where only the VideoLUT sits between the frame buffer and the display. */
static void wl_tag_surfaces(dispwin *p) {
	wlwin *w = p->wl;
	struct wp_color_management_output_v1 *cmo;
	wldescres res;

	if (w->cmgr == NULL) {
		debugr("wl_tag_surfaces: no wp_color_manager_v1, surfaces stay untagged\n");
		return;
	}

	memset(&res, 0, sizeof(res));
	cmo = wp_color_manager_v1_get_output(w->cmgr, w->out);
	w->out_desc = wp_color_management_output_v1_get_image_description(cmo);
	wp_image_description_v1_add_listener(w->out_desc, &desc_listener, &res);
	if (wl_wait_flag(w->dpy, &res.done, WL_CONFIG_TIMEOUT) || res.failed) {
		warning("Can't get the output's color description, "
		        "the compositor may color manage the test patches");
		wp_image_description_v1_destroy(w->out_desc);
		w->out_desc = NULL;
		wp_color_management_output_v1_destroy(cmo);
		return;
	}
	wp_color_management_output_v1_destroy(cmo);

	w->base_cms = wp_color_manager_v1_get_surface(w->cmgr, w->base);
	wp_color_management_surface_v1_set_image_description(w->base_cms, w->out_desc,
	                                     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	w->patch_cms = wp_color_manager_v1_get_surface(w->cmgr, w->patch);
	wp_color_management_surface_v1_set_image_description(w->patch_cms, w->out_desc,
	                                     WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	debugr("wl_tag_surfaces: surfaces tagged with output image description\n");
}

static int wl_create_window(dispwin *p, disppath *disp, double width, double height,
                            double hoff, double voff) {
	wlwin *w = p->wl;
	int mw, mh, pw_mm, ph_mm;
	double lw, lh;			/* Output size in logical pixels */
	int wi, he, xo = 0, yo = 0;
	uint32_t anchor;

	if (w->comp == NULL || w->subcomp == NULL || w->layer_shell == NULL
	 || w->viewporter == NULL || w->spb == NULL) {
		warning("The compositor lacks a protocol needed for the test window "
		        "(wl_subcompositor, zwlr_layer_shell_v1, wp_viewporter or "
		        "wp_single_pixel_buffer_manager_v1)");
		return 1;
	}

	wl_dev_mode_size(w->dev, &mw, &mh, &pw_mm, &ph_mm);
	if (mw <= 0 || mh <= 0) {
		debugr("wl_create_window: output mode size unknown\n");
		return 1;
	}
	lw = mw / w->dev->scale;
	lh = mh / w->dev->scale;

	/* Patch size in mm -> physical pixels -> logical pixels */
	if (pw_mm > 0 && ph_mm > 0) {
		wi = (int)(width * mw / pw_mm / w->dev->scale + 0.5);
		he = (int)(height * mh / ph_mm / w->dev->scale + 0.5);
	} else {
		/* Projectors and some TVs don't report a size. Assume ~96 DPI. */
		wi = (int)(width * 96.0/25.4 + 0.5);
		he = (int)(height * 96.0/25.4 + 0.5);
	}
	if (wi > lw) wi = (int)lw;
	if (he > lh) he = (int)lh;
	if (wi < 1) wi = 1;
	if (he < 1) he = 1;

	if (p->fullscreen) {	/* Window fills the screen, test area is within it */
		p->tx = (int)((hoff * 0.5 + 0.5) * (lw - wi) + 0.5);
		p->ty = (int)((voff * 0.5 + 0.5) * (lh - he) + 0.5);
		p->ww = (int)lw;
		p->wh = (int)lh;
	} else {				/* Test area completely fills the window */
		p->tx = 0;
		p->ty = 0;
		p->ww = wi;
		p->wh = he;
		xo = (int)((hoff * 0.5 + 0.5) * (lw - wi) + 0.5);
		yo = (int)((voff * 0.5 + 0.5) * (lh - he) + 0.5);
	}
	p->tw = wi;
	p->th = he;

	debugr2((errout, "wl_create_window: output %s logical %.0f x %.0f, window %d x %d at %d, %d, "
	         "patch %d x %d at %d, %d\n", w->dev->name, lw, lh, p->ww, p->wh, xo, yo,
	         p->tw, p->th, p->tx, p->ty));

	w->base = wl_compositor_create_surface(w->comp);
	w->patch = wl_compositor_create_surface(w->comp);
	w->patch_sub = wl_subcompositor_get_subsurface(w->subcomp, w->patch, w->base);
	/* Synchronized, so a patch change and the presentation request on the */
	/* parent take effect in the same frame */
	wl_subsurface_set_sync(w->patch_sub);
	wl_subsurface_set_position(w->patch_sub, p->tx, p->ty);
	w->base_vp = wp_viewporter_get_viewport(w->viewporter, w->base);
	w->patch_vp = wp_viewporter_get_viewport(w->viewporter, w->patch);
	wp_viewport_set_destination(w->patch_vp, p->tw, p->th);

	/* Input goes to the terminal Argyll is running in */
	{
		struct wl_region *empty = wl_compositor_create_region(w->comp);
		wl_surface_set_input_region(w->base, empty);
		wl_surface_set_input_region(w->patch, empty);
		wl_region_destroy(empty);
	}

	if (w->cmgr != NULL)
		wp_color_manager_v1_add_listener(w->cmgr, &cm_listener, w);
	wl_tag_surfaces(p);

	w->layer = zwlr_layer_shell_v1_get_layer_surface(w->layer_shell, w->base, w->out,
	                         ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "argyllcms-dispwin");
	zwlr_layer_surface_v1_add_listener(w->layer, &layer_listener, w);
	if (p->fullscreen) {
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM
		       | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
		zwlr_layer_surface_v1_set_size(w->layer, 0, 0);
	} else {
		anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
		zwlr_layer_surface_v1_set_size(w->layer, p->ww, p->wh);
		zwlr_layer_surface_v1_set_margin(w->layer, yo, 0, 0, xo);
	}
	zwlr_layer_surface_v1_set_anchor(w->layer, anchor);
	/* -1 so panels don't push us around: margins are from the output edge */
	zwlr_layer_surface_v1_set_exclusive_zone(w->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(w->layer,
	                         ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(w->base);

	if (wl_wait_flag(w->dpy, &w->configured, WL_CONFIG_TIMEOUT) || w->closed) {
		debugr("wl_create_window: layer surface wasn't configured\n");
		return 1;
	}
	if (w->conf_w > 0 && w->conf_h > 0) {
		p->ww = w->conf_w;
		p->wh = w->conf_h;
	}

	w->base_buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(w->spb,
	                                                    0, 0, 0, 0xffffffff);
	wl_surface_attach(w->base, w->base_buf, 0, 0);
	wp_viewport_set_destination(w->base_vp, p->ww, p->wh);
	wl_surface_damage_buffer(w->base, 0, 0, 1, 1);

	if (w->idle_mgr != NULL)
		w->inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(w->idle_mgr, w->base);
	else
		warning("Compositor lacks idle-inhibit, the screen may blank during measurement");

	/* The first patch is drawn by new_dispwin(), which commits base */
	return 0;
}

/* ------------------------------------------------------------------ */
/* Public functions */

int wl_dispwin_open(dispwin *p, disppath *disp, double width, double height,
                    double hoff, double voff, int native) {
	wlwin *w;
	int i;

	if ((w = p->wl = wl_new_conn()) == NULL) {
		debugr("wl_dispwin_open: can't connect to the Wayland compositor\n");
		return 1;
	}

	/* Find our device by UUID, which survives output reordering */
	for (i = 0; i < w->ndevs; i++) {
		wldev *dv = w->devs[i];
		if (dv->removed || !dv->enabled)
			continue;
		if (disp->wl_uuid != NULL && dv->uuid != NULL) {
			if (strcmp(disp->wl_uuid, dv->uuid) == 0)
				break;
		} else if (dv->name != NULL && strcmp(disp->name, dv->name) == 0)
			break;
	}
	if (i >= w->ndevs) {
		debugr2((errout, "wl_dispwin_open: output '%s' has gone away\n", disp->name));
		return 1;
	}
	w->dev = w->devs[i];

	for (i = 0; i < w->nouts; i++) {
		if (w->outs[i].name != NULL && strcmp(w->outs[i].name, w->dev->name) == 0) {
			w->out = w->outs[i].out;
			break;
		}
	}

	if (wl_decode_edid(w->dev, &p->edid, &p->edid_len))
		return 1;

	/* There's no Wayland way to find the scanout depth, and the compositor */
	/* dithers or rounds anything finer, so quantize as an 8 bit display. */
	p->fdepth = 8;
	p->rdepth = p->fdepth;
	p->edepth = 16;

	/* The VideoLUT is emulated by having KWin load a generated profile, */
	/* which needs the ICC path and profile source requests */
	if (w->kmgmt != NULL && w->kmgmt_ver >= 8 && w->dev->cps >= 0) {
		p->nent = WL_LUT_SIZE;
		p->ndepth = 8;
	} else {
		p->nent = 0;
		p->ndepth = 0;
	}

	w->saved_cps = w->dev->cps;
	w->saved_cpt = w->dev->cpt;
	w->saved_icc = wl_strdup_or_null(w->dev->icc_path);
	wl_read_lut(p, w->base_lut, w->saved_cps == KDE_CPS_ICC ? w->saved_icc : NULL);

	if (p->nowin)
		return 0;

	if (w->out == NULL) {
		debugr2((errout, "wl_dispwin_open: no wl_output named '%s'\n", w->dev->name));
		return 1;
	}

	if (w->dev->hdr)
		warning("HDR is on for %s, so measurements won't reflect SDR use", w->dev->name);

	nl_inhibit(p);

	/* Callers asking for native output load a linear LUT themselves. */
	/* KWin's "efficiency" mode may take shortcuts with color precision. */
	if ((native & 1) && w->dev->cpt >= 0 && w->dev->cpt != KDE_CPT_ACCURACY) {
		if (wl_apply_config(p, -1, KDE_CPT_ACCURACY, NULL) == 0) {
			w->cpt_changed = 1;
			debugr2((errout, "wl_dispwin_open: output %s set to accuracy mode, was %d\n",
			         w->dev->name, w->saved_cpt));
		} else {
			warning("Couldn't switch %s to its color accuracy mode", w->dev->name);
		}
	}

	/* KWin's ICC mode doesn't pass test values through, whatever the */
	/* profile: measured single channel ramps clipped red near 87% and */
	/* blue near 97%, and the vcgt is sampled about an entry off. Its sRGB */
	/* mode gives a clean gamma ramp, so measure in that and apply the */
	/* VideoLUT to the patch values ourselves. This must happen before the */
	/* window is made, as its surfaces are tagged with the output's */
	/* image description, which the mode changes. */
	if (p->nent == WL_LUT_SIZE) {
		int ok = 1;

		if (w->dev->cps != KDE_CPS_SRGB) {
			if (wl_apply_config(p, KDE_CPS_SRGB, -1, NULL) == 0) {
				w->cps_changed = 1;
				debugr2((errout, "wl_dispwin_open: output %s set to sRGB mode, was %d\n",
				         w->dev->name, w->saved_cps));
			} else {
				warning("Couldn't switch %s to sRGB mode, so its profile may "
				        "alter the test values", w->dev->name);
				ok = 0;
			}
		}
		/* Otherwise LUT loads fall back to generated profiles in KWin */
		if (ok) {
			memcpy(w->cur_lut, w->base_lut, sizeof(w->cur_lut));
			w->sw_lut = 1;
		}
	}

	if (wl_create_window(p, disp, width, height, hoff, voff))
		return 1;

	return 0;
}

/* Look v (0..1) up in a LUT, interpolating between entries */
static double wl_lut_lookup(double *lut, double v) {
	double x = v * (WL_LUT_SIZE - 1.0);
	int i = (int)x;

	if (i < 0)
		return lut[0];
	if (i >= WL_LUT_SIZE - 1)
		return lut[WL_LUT_SIZE - 1];
	x -= i;
	v = lut[i] + x * (lut[i + 1] - lut[i]);
	return v < 0.0 ? 0.0 : v > 1.0 ? 1.0 : v;
}

int wl_dispwin_set_color(dispwin *p) {
	wlwin *w = p->wl;
	struct wl_buffer *buf;
	struct wp_presentation_feedback *fb = NULL;
	struct wl_callback *cb = NULL;
	wlpres pres;
	uint32_t v[3];
	int j;

	if (w == NULL || w->patch == NULL)
		return 1;
	if (w->closed) {
		warning("The compositor closed the test window");
		return 1;
	}

	/* Quantize to the frame buffer depth as X11 does, look that up in the */
	/* emulated VideoLUT, then scale to the 32 bit range so the compositor */
	/* sees exactly that level. The LUT output isn't quantized, which is */
	/* what lets native (native & 1) output reach between 8 bit levels. */
	for (j = 0; j < 3; j++) {
		double fmax = (1 << p->fdepth) - 1.0;
		double q = (double)(int)(fmax * p->r_rgb[j] + 0.5) / fmax;
		if (w->sw_lut)
			q = wl_lut_lookup(w->cur_lut[j], q);
		v[j] = (uint32_t)(q * 4294967295.0 + 0.5);
	}

	buf = wp_single_pixel_buffer_manager_v1_create_u32_rgba_buffer(w->spb,
	                                                  v[0], v[1], v[2], 0xffffffff);
	wl_surface_attach(w->patch, buf, 0, 0);
	/* Always damage, so a repeated color still produces a new frame and */
	/* presentation feedback */
	wl_surface_damage_buffer(w->patch, 0, 0, 1, 1);
	wl_surface_commit(w->patch);

	memset(&pres, 0, sizeof(pres));
	if (w->presentation != NULL) {
		fb = wp_presentation_feedback(w->presentation, w->base);
		wp_presentation_feedback_add_listener(fb, &fb_listener, &pres);
	} else {
		cb = wl_surface_frame(w->base);
		wl_callback_add_listener(cb, &frame_listener, &pres);
	}
	wl_surface_commit(w->base);

	if (wl_wait_flag(w->dpy, &pres.done, WL_PRESENT_TIMEOUT))
		debugr("wl_dispwin_set_color: no presentation feedback\n");
	else if (!pres.presented)
		debugr("wl_dispwin_set_color: frame was discarded\n");

	if (fb != NULL)
		wp_presentation_feedback_destroy(fb);
	if (cb != NULL)
		wl_callback_destroy(cb);

	/* The old buffer is no longer on screen now */
	if (w->patch_buf != NULL)
		wl_buffer_destroy(w->patch_buf);
	w->patch_buf = buf;

	return 0;
}

/* nz if path is one of our generated LUT profiles */
static int wl_is_gen_path(char *path) {
	return path != NULL && strstr(path, "/" WL_LUT_DIR "/") != NULL;
}

/* The saved profile path to give KWin when restoring, or NULL to leave */
/* the path alone. KWin rejects a path to a missing file, which happens */
/* when the saved profile has just been uninstalled. */
static char *wl_restore_path(wlwin *w) {
	if (w->saved_icc == NULL || w->saved_icc[0] == '\000'
	 || access(w->saved_icc, R_OK) != 0)
		return NULL;
	return w->saved_icc;
}

void wl_dispwin_restore(dispwin *p) {
	wlwin *w = p->wl;

	if (w == NULL)
		return;

	if (w->cpt_changed || w->cps_changed || w->lut_active) {
		int cps = w->cps_changed || w->lut_active;
		char *icc = cps ? wl_restore_path(w) : NULL;

		if (wl_apply_config(p, cps ? w->saved_cps : -1,
		                       w->cpt_changed ? w->saved_cpt : -1, icc) != 0)
			warning("Couldn't restore the color settings of %s", w->dev->name);
		else
			debugr("wl_dispwin_restore: restored output color settings\n");
		w->cpt_changed = 0;
		w->cps_changed = 0;
		w->lut_active = 0;
	}
	w->sw_lut = 0;

	/* A temporary generated profile is no longer referenced by KWin */
	if (w->gen_path != NULL) {
		if (w->saved_icc == NULL || strcmp(w->gen_path, w->saved_icc) != 0)
			unlink(w->gen_path);
		free(w->gen_path);
		w->gen_path = NULL;
	}
	nl_uninhibit(p);
}

void wl_dispwin_close(dispwin *p) {
	wlwin *w = p->wl;

	if (w == NULL)
		return;

	wl_dispwin_restore(p);

	if (w->inhibitor != NULL)
		zwp_idle_inhibitor_v1_destroy(w->inhibitor);
	if (w->patch_cms != NULL)
		wp_color_management_surface_v1_destroy(w->patch_cms);
	if (w->base_cms != NULL)
		wp_color_management_surface_v1_destroy(w->base_cms);
	if (w->out_desc != NULL)
		wp_image_description_v1_destroy(w->out_desc);
	if (w->patch_vp != NULL)
		wp_viewport_destroy(w->patch_vp);
	if (w->patch_sub != NULL)
		wl_subsurface_destroy(w->patch_sub);
	if (w->patch != NULL)
		wl_surface_destroy(w->patch);
	if (w->patch_buf != NULL)
		wl_buffer_destroy(w->patch_buf);
	if (w->layer != NULL)
		zwlr_layer_surface_v1_destroy(w->layer);
	if (w->base_vp != NULL)
		wp_viewport_destroy(w->base_vp);
	if (w->base != NULL)
		wl_surface_destroy(w->base);
	if (w->base_buf != NULL)
		wl_buffer_destroy(w->base_buf);
	wl_display_flush(w->dpy);

	free(w->saved_icc);
	free(w->gen_path);
	wl_free_conn(w);
	p->wl = NULL;
}

int wl_dispwin_set_profile(dispwin *p, char *path) {
	wlwin *w = p->wl;
	int cps = path != NULL ? KDE_CPS_ICC : KDE_CPS_SRGB;
	char *old = w != NULL ? w->saved_icc : NULL;

	if (w == NULL || w->dev == NULL)
		return 1;

	/* While a generated LUT profile is in use, or we're measuring in sRGB */
	/* mode, just make the restore leave the new profile in effect */
	if (w->sw_lut)
		w->cps_changed = 1;
	else if (!w->lut_active && wl_apply_config(p, cps, -1, path) != 0)
		return 1;

	w->saved_cps = cps;
	if (path != NULL) {
		w->saved_icc = strdup(path);
		if (wl_is_gen_path(old) && strcmp(old, path) != 0)
			unlink(old);
		free(old);
	}
	/* So that restoring the caller's copy of this calibration on exit */
	/* doesn't generate a profile to replace the one just installed */
	wl_read_lut(p, w->base_lut, path);
	/* KWin would now be applying its calibration */
	if (w->sw_lut)
		memcpy(w->cur_lut, w->base_lut, sizeof(w->cur_lut));
	return 0;
}

char *wl_dispwin_get_profile_path(dispwin *p) {
	wlwin *w = p->wl;

	if (w == NULL || w->dev == NULL)
		return NULL;

	/* Report the user's profile, not one generated for a LUT */
	if (w->saved_cps != KDE_CPS_ICC || w->saved_icc == NULL || w->saved_icc[0] == '\000')
		return NULL;
	return strdup(w->saved_icc);
}

/* ------------------------------------------------------------------ */
/* VideoLUT emulation. KWin applies the vcgt of the output's profile */
/* after compositing, so loading a LUT means writing a profile with the */
/* LUT as its vcgt and pointing KWin at it. Our surfaces are tagged with */
/* the output's image description, so the rest of the profile doesn't */
/* alter the test values, as long as its TRC is a pure power curve: KWin */
/* encodes with the profile's TRC but describes the output to clients */
/* with a power curve, so an sRGB style TRC with a linear toe crushes */
/* the shadows. */

#define WL_GEN_GAMMA 2.2

/* Read the vcgt of the given profile into lut[], or a linear ramp if */
/* path is NULL or the profile has none or can't be read (Argyll can't */
/* read ICC V4 profiles). */
static void wl_read_lut(dispwin *p, double lut[3][WL_LUT_SIZE], char *path) {
	icmErr err = { 0, { '\000'} };
	icmFile *fp = NULL;
	icc *icco = NULL;
	icmVideoCardGamma *wo;
	int i, j, got = 0;

	if (path != NULL && path[0] != '\000'
	 && (fp = new_icmFileStd_name(&err, path, "r")) != NULL
	 && (icco = new_icc(&err)) != NULL
	 && icco->read(icco, fp, 0) == 0
	 && (wo = (icmVideoCardGamma *)icco->read_tag(icco, icSigVideoCardGammaTag)) != NULL
	 && wo->ttype == icSigVideoCardGammaType) {
		int nch = (wo->tagType == icVideoCardGammaTable && wo->u.table.channels == 1) ? 1 : 3;
		for (j = 0; j < 3; j++)
			for (i = 0; i < WL_LUT_SIZE; i++)
				lut[j][i] = wo->lookup(wo, nch == 1 ? 0 : j, i/(WL_LUT_SIZE-1.0));
		got = 1;
	} else if (path != NULL && path[0] != '\000') {
		debugr2((errout, "wl_read_lut: no readable vcgt in '%s', assuming linear\n", path));
	}
	if (icco != NULL)
		icco->del(icco);
	if (fp != NULL)
		fp->del(fp);

	if (!got) {
		for (j = 0; j < 3; j++)
			for (i = 0; i < WL_LUT_SIZE; i++)
				lut[j][i] = i/(WL_LUT_SIZE-1.0);
	}
}

/* Write a display profile with sRGB colorants, a pure gamma TRC and */
/* the given LUT as its vcgt. Return nz on failure. */
static int wl_write_lut_profile(dispwin *p, char *path, ramdac *r) {
	/* sRGB colorants and white, as in ref/sRGB.icm */
	static double wtpt[3] = { 0.95045471, 1.00000000, 1.08905029 };
	static double cols[3][3] = {
		{ 0.43603516, 0.22248840, 0.01391602 },
		{ 0.38511658, 0.71690369, 0.09706116 },
		{ 0.14305115, 0.06060791, 0.71392822 }
	};
	static icTagSignature colsig[3] = { icSigRedColorantTag, icSigGreenColorantTag,
	                                    icSigBlueColorantTag };
	static icTagSignature trcsig[3] = { icSigRedTRCTag, icSigGreenTRCTag, icSigBlueTRCTag };
	icmErr err = { 0, { '\000'} };
	icmFile *fp;
	icc *icco;
	int i, j, rv;

	if ((icco = new_icc(&err)) == NULL)
		return 1;
	icco->set_version(icco, ICMTV_22);
	icco->header->deviceClass     = icSigDisplayClass;
	icco->header->colorSpace      = icSigRgbData;
	icco->header->pcs             = icSigXYZData;
	icco->header->renderingIntent = icRelativeColorimetric;
	icco->header->platform        = icmSig_nix;

	{
		icmCommonTextDescription *wo;
		char *desc = "ArgyllCMS VideoLUT";
		if ((wo = (icmCommonTextDescription *)icco->add_tag(icco, icSigProfileDescriptionTag,
		                                      icmSigCommonTextDescriptionType)) == NULL)
			goto fail;
		wo->count = strlen(desc)+1;
		wo->allocate(wo);
		strcpy(wo->desc, desc);
	}
	{
		icmCommonTextDescription *wo;
		char *crt = "Generated by ArgyllCMS to load a calibration on Wayland";
		if ((wo = (icmCommonTextDescription *)icco->add_tag(icco, icSigCopyrightTag,
		                                      icmSigCommonTextDescriptionType)) == NULL)
			goto fail;
		wo->count = strlen(crt)+1;
		wo->allocate(wo);
		strcpy(wo->desc, crt);
	}
	{
		icmXYZArray *wo;
		if ((wo = (icmXYZArray *)icco->add_tag(icco, icSigMediaWhitePointTag,
		                                       icSigXYZArrayType)) == NULL)
			goto fail;
		wo->count = 1;
		wo->allocate(wo);
		wo->data[0].X = wtpt[0];
		wo->data[0].Y = wtpt[1];
		wo->data[0].Z = wtpt[2];
	}
	for (j = 0; j < 3; j++) {
		icmXYZArray *wo;
		icmCurve *wc;
		if ((wo = (icmXYZArray *)icco->add_tag(icco, colsig[j], icSigXYZArrayType)) == NULL)
			goto fail;
		wo->count = 1;
		wo->allocate(wo);
		wo->data[0].X = cols[j][0];
		wo->data[0].Y = cols[j][1];
		wo->data[0].Z = cols[j][2];

		if ((wc = (icmCurve *)icco->add_tag(icco, trcsig[j], icSigCurveType)) == NULL)
			goto fail;
		wc->ctype = icmCurveGamma;
		wc->count = 1;
		wc->allocate(wc);
		wc->data[0] = WL_GEN_GAMMA;
	}
	{
		icmVideoCardGamma *wo;
		if ((wo = (icmVideoCardGamma *)icco->add_tag(icco, icSigVideoCardGammaTag,
		                                             icSigVideoCardGammaType)) == NULL)
			goto fail;
		wo->tagType = icVideoCardGammaTable;
		wo->u.table.channels = 3;
		wo->u.table.entryCount = r->nent;
		wo->u.table.entrySize = 2;
		wo->allocate(wo);
		for (j = 0; j < 3; j++) {
			for (i = 0; i < r->nent; i++) {
				double vv = r->v[j][i];
				wo->u.table.data[j][i] = vv < 0.0 ? 0.0 : vv > 1.0 ? 1.0 : vv;
			}
		}
	}

	if ((fp = new_icmFileStd_name(&err, path, "w")) == NULL)
		goto fail;
	rv = icco->write(icco, fp, 0);
	fp->del(fp);
	if (rv != 0) {
		debugr2((errout, "wl_write_lut_profile: write failed: %s\n", icco->e.m));
		unlink(path);
		goto fail;
	}
	icco->del(icco);
	return 0;

  fail:
	debugr2((errout, "wl_write_lut_profile: creating '%s' failed\n", path));
	icco->del(icco);
	return 1;
}

int wl_dispwin_get_lut(dispwin *p, double *v[3]) {
	wlwin *w = p->wl;
	double lut[3][WL_LUT_SIZE];
	char *path = NULL;
	int i, j;

	if (w == NULL || w->dev == NULL || p->nent != WL_LUT_SIZE)
		return 1;

	if (w->sw_lut)
		memcpy(lut, w->cur_lut, sizeof(lut));
	else {
		if (w->dev->cps == KDE_CPS_ICC)
			path = w->dev->icc_path;
		wl_read_lut(p, lut, path);
	}
	for (j = 0; j < 3; j++)
		for (i = 0; i < WL_LUT_SIZE; i++)
			v[j][i] = lut[j][i];
	return 0;
}

int wl_dispwin_set_lut(dispwin *p, ramdac *r, int persist) {
	wlwin *w = p->wl;
	char **paths = NULL, name[100], *path;
	unsigned int hash = 2166136261u;		/* FNV-1a */
	xdg_error er;
	int i, j, isbase = 1;

	if (w == NULL || w->dev == NULL || r->nent != WL_LUT_SIZE)
		return 1;

	for (j = 0; j < 3; j++) {
		for (i = 0; i < r->nent; i++) {
			unsigned int q = (unsigned int)(r->v[j][i] * 65535.0 + 0.5);
			hash = (hash ^ (q & 0xff)) * 16777619u;
			hash = (hash ^ (q >> 8)) * 16777619u;
			if (fabs(r->v[j][i] - w->base_lut[j][i]) > 1e-4)
				isbase = 0;
		}
	}

	/* While measuring, the LUT only applies to the patches. That's all */
	/* calibration and native output (one load per patch) need, and it */
	/* never touches KWin. */
	if (w->sw_lut) {
		for (j = 0; j < 3; j++)
			for (i = 0; i < WL_LUT_SIZE; i++)
				w->cur_lut[j][i] = r->v[j][i];
		if (!persist)
			return 0;
	}

	/* Going back to the LUT the saved state gives (e.g. restoring the */
	/* original LUT on exit) just means restoring the saved state */
	if (isbase && !persist) {
		if (w->lut_active) {
			if (wl_apply_config(p, w->saved_cps, -1, wl_restore_path(w)) != 0)
				return 1;
			w->lut_active = 0;
			if (w->gen_path != NULL && (w->saved_icc == NULL
			                         || strcmp(w->gen_path, w->saved_icc) != 0))
				unlink(w->gen_path);
			free(w->gen_path);
			w->gen_path = NULL;
		}
		return 0;
	}

	/* Name by content, because KWin may not re-read a profile whose */
	/* path hasn't changed */
	snprintf(name, sizeof(name), WL_LUT_DIR "/vcgt-%.8s-%08x.icc",
	         w->dev->uuid != NULL ? w->dev->uuid : "output", hash);
	if (xdg_bds(&er, &paths, xdg_data, xdg_write, xdg_user, xdg_none, name) < 1) {
		debugr2((errout, "wl_dispwin_set_lut: no place for '%s': %s\n", name, xdg_errstr(er)));
		return 1;
	}
	if ((path = strdup(paths[0])) == NULL) {
		xdg_free(paths, 1);
		return 1;
	}
	xdg_free(paths, 1);

	if (access(path, R_OK) != 0 && wl_write_lut_profile(p, path, r) != 0) {
		free(path);
		return 1;
	}
	/* Already in use, or a persistent load while measuring in sRGB mode, */
	/* which wl_dispwin_restore() applies */
	if (!w->sw_lut
	 && !(w->lut_active && w->gen_path != NULL && strcmp(w->gen_path, path) == 0)
	 && wl_apply_config(p, KDE_CPS_ICC, -1, path) != 0) {
		free(path);
		return 1;
	}
	debugr2((errout, "wl_dispwin_set_lut: KWin %s using '%s'%s\n",
	         w->sw_lut ? "will be" : "now", path, persist ? " (persistent)" : ""));

	/* The previous temporary profile is no longer referenced */
	if (w->gen_path != NULL && strcmp(w->gen_path, path) != 0
	 && (w->saved_icc == NULL || strcmp(w->gen_path, w->saved_icc) != 0))
		unlink(w->gen_path);
	free(w->gen_path);
	w->gen_path = path;

	if (persist) {
		char *old = w->saved_icc;

		w->saved_cps = KDE_CPS_ICC;
		w->saved_icc = strdup(path);
		if (wl_is_gen_path(old) && strcmp(old, path) != 0)
			unlink(old);
		free(old);
		for (j = 0; j < 3; j++)
			for (i = 0; i < WL_LUT_SIZE; i++)
				w->base_lut[j][i] = r->v[j][i];
		w->lut_active = 0;
		if (w->sw_lut)
			w->cps_changed = 1;
	} else {
		w->lut_active = 1;
	}
	return 0;
}
