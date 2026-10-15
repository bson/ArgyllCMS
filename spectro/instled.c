
/*
 * Argyll Color Management System
 *
 * Turn an instrument's indicator LED on or off, without measuring, so a
 * program driving the Argyll tools can show the instrument is in use.
 * Instruments without a controllable LED are left as they are.
 *
 * This material is licenced under the GNU GENERAL PUBLIC LICENSE Version 2 or later :-
 * see the License2.txt file for licencing details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "copyright.h"
#include "aconfig.h"
#include "numlib.h"
#include "cgats.h"
#include "conv.h"
#include "xicc.h"
#include "ui.h"
#include "inst.h"
#include "icoms.h"
#include "instappsup.h"

void usage(char *diag, ...) {
	icompaths *icmps;

	fprintf(stderr,"Turn an instrument's indicator LED on or off, Version %s\n",ARGYLL_VERSION_STR);
	fprintf(stderr,"Licensed under the GPL Version 2 or later\n");
	if (diag != NULL) {
		va_list args;
		fprintf(stderr,"Diagnostic: ");
		va_start(args, diag);
		vfprintf(stderr, diag, args);
		va_end(args);
		fprintf(stderr,"\n");
	}
	fprintf(stderr,"usage: instled [-options] on|off\n");
	fprintf(stderr," -c listno            Set communication port from the following list (default 1)\n");
	if ((icmps = new_icompaths_sel(g_log, icomt_instrument | icomt_usb | icomt_hid)) != NULL) {
		icompath **paths;
		if ((paths = icmps->paths) != NULL) {
			int i;
			for (i = 0; ; i++) {
				if (paths[i] == NULL)
					break;
				fprintf(stderr,"    %d = '%s'\n",i+1,paths[i]->name);
			}
		} else
			fprintf(stderr,"    ** No ports found **\n");
		icmps->del(icmps);
	}
	fprintf(stderr," -v                   Verbose\n");
	fprintf(stderr," -D [level]           Print debug diagnostics to stderr\n");
	exit(1);
}

int
main(int argc, char *argv[]) {
	int fa, nfa;
	int verb = 0;
	int debug = 0;
	int comport = 1;
	int state = -1;				/* 1 = on, 0 = off */
	int mask = 0;
	icompaths *icmps;
	icompath *ipath;
	inst *it;
	inst_code rv;

	set_exe_path(argv[0]);
	check_if_not_interactive();

	for (fa = 1; fa < argc; fa++) {
		nfa = fa;
		if (argv[fa][0] == '-') {
			char *na = NULL;
			if (argv[fa][2] != '\000')
				na = &argv[fa][2];
			else if ((fa+1) < argc && argv[fa+1][0] != '-') {
				nfa = fa + 1;
				na = argv[nfa];
			}

			if (argv[fa][1] == '?') {
				usage(NULL);
			} else if (argv[fa][1] == 'c') {
				fa = nfa;
				if (na == NULL) usage("Paramater expected following -c");
				comport = atoi(na);
				if (comport < 1 || comport > 99) usage("-c parameter %d out of range",comport);
			} else if (argv[fa][1] == 'v') {
				verb = 1;
			} else if (argv[fa][1] == 'D') {
				debug = 1;
				if (na != NULL && na[0] >= '0' && na[0] <= '9') {
					debug = atoi(na);
					fa = nfa;
				}
				g_log->debug = debug;
			} else
				usage("Unknown flag '%s'",argv[fa]);
		} else if (strcmp(argv[fa], "on") == 0) {
			state = 1;
		} else if (strcmp(argv[fa], "off") == 0) {
			state = 0;
		} else
			usage("Unknown argument '%s'",argv[fa]);
	}
	if (state < 0)
		usage("on or off expected");

	/* USB and HID only: probing the serial ports takes seconds, and none of */
	/* the instruments with an LED is serial. They're listed before the */
	/* serial ports, so the numbers are the same as the other tools' -c. */
	if ((icmps = new_icompaths_sel(g_log, icomt_instrument | icomt_usb | icomt_hid)) == NULL)
		error("Finding instrument paths failed");
	if ((ipath = icmps->get_path(icmps, comport)) == NULL)
		error("No instrument at port %d",comport);

	if ((it = new_inst(ipath, 0, g_log, DUIH_FUNC_AND_CONTEXT)) == NULL)
		error("Unknown, inappropriate or no instrument detected");

	/* error() exits, so the message is taken before the instrument goes */
	if ((rv = it->init_coms(it, baud_nc, fc_nc, 15.0)) != inst_ok) {
		char *why = strdup(it->interp_error(it, rv));
		it->del(it);
		error("Failed to initialise communications with instrument ('%s')", why);
	}

	/* (Opening an instrument can itself change the LED, as the i1d3's does) */
	if ((rv = it->init_inst(it)) != inst_ok) {
		char *why = strdup(it->interp_error(it, rv));
		it->del(it);
		error("Instrument initialisation failed ('%s')", why);
	}

	if (it->get_set_opt(it, inst_opt_get_gen_ledmask, &mask) != inst_ok || (mask & 1) == 0) {
		if (verb)
			printf("The instrument has no controllable LED\n");
	} else {
		if ((rv = it->get_set_opt(it, inst_opt_set_led_state, state ? mask : 0)) != inst_ok) {
			char *why = strdup(it->interp_error(it, rv));
			it->del(it);
			error("Setting the LED failed ('%s')", why);
		}
		if (verb)
			printf("LED %s\n", state ? "on" : "off");
	}

	it->del(it);
	icmps->del(icmps);
	return 0;
}
