
/* 
 * Argyll Color Management System
 * Create a linear display calibration file.
 *
 * Author: Graeme W. Gill
 * Date:   15/11/2005
 *
 * Copyright 1996 - 2025 Graeme W. Gill
 * All rights reserved.
 *
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 */

#undef DEBUG

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>
#include <time.h>
#include <string.h>
#include "copyright.h"
#include "aconfig.h"
#include "numlib.h"
#include "cgats.h"
#include "conv.h"
#include "xicc.h"

#include "sort.h"

#include <stdarg.h>

#if defined (NT)
#include <conio.h>
#endif


void
usage(int level) {
	int i;
	fprintf(stderr,"Create a synthetic calibration file, Version %s\n",ARGYLL_VERSION_STR);
	fprintf(stderr,"Author: Graeme W. Gill, licensed under the AGPL Version 3\n");
	fprintf(stderr,"usage: synthcal [-options] outfile\n");
	fprintf(stderr," -r res          Set the calibration resolution (default 256)\n");
	fprintf(stderr," -t N            i = input, o = output, d = display (default)\n");
	fprintf(stderr," -d col_comb     choose colorant combination from the following (default 3):\n");
	for (i = 0; ; i++) {
		char *desc; 
		if (icx_enum_colorant_comb(i, &desc) == 0)
			break;
		fprintf(stderr,"                 %d: %s\n",i,desc);
	}
	fprintf(stderr," -D colorant     Add or delete colorant from combination:\n");
	if (level == 0)
		fprintf(stderr,"                 (Use -?? to list known colorants)\n");
	else {
		fprintf(stderr,"                 %d: %s\n",0,"Additive");
		for (i = 0; ; i++) {
			char *desc; 
			if (icx_enum_colorant(i, &desc) == 0)
				break;
			fprintf(stderr,"                 %d: %s\n",i+1,desc);
		}
	}
	fprintf(stderr," -o o1,o2,o3,... Set non-linear curve offset, last to all chan. (default 0.0)\n");
	fprintf(stderr," -s s1,s2,s3,... Set non-linear curve scale, last to all chan. (default 1.0)\n");
	fprintf(stderr," -p p1,p2,p3,... Set non-linear curve powers, last to all chan. (default 1.0)\n");
	fprintf(stderr," -E description  Set the profile dEscription string\n");
	fprintf(stderr," outfile         Base name for output .cal file\n");
	exit(1);
	}

int main(int argc, char *argv[])
{
	int fa,nfa;							/* current argument we're looking at */
	int j;
	int verb = 0;
	static char outname[MAXNAMEL+1] = { 0 };	/* Output cgats file base name */
	char *profDesc = NULL;		/* Description */ 
	int devtype = -1;			/* device type, 0 = in, 1 = out, 2 = display */
	inkmask devmask = 0;		/* ICX ink mask of device space */
	int calres = 256;			/* Resolution of resulting file */
	int devchan;				/* Number of chanels in device space */
	char *ident;				/* Ink combination identifier (includes possible leading 'i') */
	char *bident;				/* Base ink combination identifier */
	double off[MAX_CHAN];		/* Output offset */
	double sca[MAX_CHAN];		/* Output scale */
	double gam[MAX_CHAN];		/* Gamma applied */
	int off_i = MAX_CHAN, sca_i = MAX_CHAN, gam_i = MAX_CHAN;	/* Last index set in param */


	for (j = 0; j < MAX_CHAN; j++)
		off[j] = 0.0, sca[j] = gam[j] = 1.0;

	error_program = "synthcal";
	if (argc <= 1)
		usage(0);

	/* Process the arguments */
	for (fa = 1;fa < argc;fa++) {
		nfa = fa;					/* skip to nfa if next argument is used */
		if (argv[fa][0] == '-')		/* Look for any flags */
			{
			char *na = NULL;		/* next argument after flag, null if none */

			if (argv[fa][2] != '\000')
				na = &argv[fa][2];		/* next is directly after flag */
			else {
				if ((fa+1) < argc) {
					if (argv[fa+1][0] != '-')
						{
						nfa = fa + 1;
						na = argv[nfa];		/* next is seperate non-flag argument */
					}
				}
			}

			if (argv[fa][1] == '?' || argv[fa][1] == '-') {
				if (argv[fa][2] == '?' || argv[fa][2] == '-')
					usage(1);
				usage(0);
			}

			else if (argv[fa][1] == 'v')
				verb = 1;

			/* Calibration file resolution */
			else if (argv[fa][1] == 'r') {
				fa = nfa;
				if (na == NULL) usage(0);
				calres = atoi(na);
				if (calres < 2 || calres > MAX_CAL_ENT)
					usage(0);
			}

			/* Select the device type */
			else if (argv[fa][1] == 't') {
				fa = nfa;
				if (na == NULL) usage(0);
				if (na[0] == 'i' || na[0] == 'I')
					devtype = 0;
				else if (na[0] == 'o' || na[0] == 'O')
					devtype = 1;
				else if (na[0] == 'd' || na[0] == 'D')
					devtype = 2;
				else
					usage(0);
			}

			/* Select the ink enumeration */
			else if (argv[fa][1] == 'd') {
				int ix;
				fa = nfa;
				if (na == NULL) usage(0);
				ix = atoi(na);
				if (ix == 0 && na[0] != '0')
					usage(0);
				if ((devmask = icx_enum_colorant_comb(ix, NULL)) == 0)
					usage(0);
			}

			/* Toggle the colorant in ink combination */
			else if (argv[fa][1] == 'D') {
				int ix, tmask;
				fa = nfa;
				if (na == NULL) usage(0);
				ix = atoi(na);
				if (ix == 0 && na[0] != '0')
					usage(0);
				if (ix == 0)
					tmask = ICX_ADDITIVE;
				else
					if ((tmask = icx_enum_colorant(ix-1, NULL)) == 0)
						usage(0);
				devmask ^= tmask;
			}

			/* curve offset */
			else if (argv[fa][1] == 'o' || argv[fa][1] == 'O') {
				fa = nfa;
				if (na == NULL) usage(0);
				for (j = 0; j < MAX_CHAN; j++) {
					int i;
					for (i = 0; ; i++) {
						if (na[i] == ','){
							na[i] = '\000';
							break;
						}
						if (na[i] == '\000') {
							i = 0;
							break;
						}
					}
					off[j] = atof(na);
					off_i = j;
					if (i == 0)
						break;
					na += i+1;
				}
			}

			/* curve scale */
			else if (argv[fa][1] == 's' || argv[fa][1] == 'S') {
				fa = nfa;
				if (na == NULL) usage(0);
				for (j = 0; j < MAX_CHAN; j++) {
					int i;
					for (i = 0; ; i++) {
						if (na[i] == ','){
							na[i] = '\000';
							break;
						}
						if (na[i] == '\000') {
							i = 0;
							break;
						}
					}
					sca[j] = atof(na);
					sca_i = j;
					if (i == 0)
						break;
					na += i+1;
				}
			}

			/* curve power */
			else if (argv[fa][1] == 'p' || argv[fa][1] == 'P') {
				fa = nfa;
				if (na == NULL) usage(0);
				for (j = 0; j < MAX_CHAN; j++) {
					int i;
					for (i = 0; ; i++) {
						if (na[i] == ','){
							na[i] = '\000';
							break;
						}
						if (na[i] == '\000') {
							i = 0;
							break;
						}
					}
					gam[j] = atof(na);
					gam_i = j;
					if (i == 0)
						break;
					na += i+1;
				}
			}

			/* Profile Description */
			else if (argv[fa][1] == 'E') {
				fa = nfa;
				if (na == NULL) usage(0);
				profDesc = na;
			}


			else 
				usage(0);
		} else
			break;
	}


	/* Get the file name argument */
	if (fa >= argc || argv[fa][0] == '-') usage(0);
	strcpy(outname,argv[fa]);
	if (strlen(outname) < 4 || strcmp(".cal",outname + strlen(outname)-4) != 0)
		strcat(outname,".cal");

	/* Implement defaults */
	if (devtype < 0) {
		devtype = 2;		/* Display */
	}

	if (devmask == 0) {
		if (devtype == 0 || devtype == 2)
			devmask = ICX_RGB;
		else
			devmask = ICX_CMYK;
	}

	ident = icx_inkmask2char(devmask, 1); 
	bident = icx_inkmask2char(devmask, 0); 
	devchan = icx_noofinks(devmask);

	/* If not all params were set, extend them to the rest */
	for (j = 0; j < devchan; j++) {
		if (j > off_i)
			off[j] = off[off_i];
		if (j > sca_i)
			sca[j] = sca[sca_i];
		if (j > gam_i)
			gam[j] = gam[gam_i];
	}

	if (verb) {
		if (devtype == 0)
			printf("Device type: input\n");
		else if (devtype == 1)
			printf("Device type: output\n");
		else
			printf("Device type: display\n");

		printf("Colorspace: %s\n", ident);

		printf("Curve parameters:\n");
		for (j = 0; j < devchan; j++)
			printf("off[%d] = %f, sca[%d] = %f, gam[%d] = %f\n",j,off[j],j,sca[j],j, gam[j]);
	}


	/* Write out the resulting calibration file */
	{
		xcal *xc;
		double **curves;
		int i, j;

		if ((xc = new_xcal()) == NULL)
			error("new_xcal() failed");

		/* All this should really be encapsulated in xcal methods ? */
		xc->originator = strdup("Argyll synthcal");

		if (devtype == 0)
			xc->devclass = icSigInputClass;
		else if (devtype == 1)
			xc->devclass = icSigOutputClass;
		else
			xc->devclass = icSigDisplayClass;

		xc->set_inkmask(xc, devmask);

		curves = dmatrix(0, devchan-1, 0, calres-1);

		for (i = 0; i < calres; i++) {
			double ii = i/(calres-1.0);

			for (j = 0; j < devchan; j++) {
				double vv = off[j] + sca[j] * pow(ii, gam[j]);
				if (vv < 0.0)
					vv = 0.0;
				else if (vv > 1.0)
					vv = 1.0;
				curves[j][i] = vv;
			}
		}

		if (xc->set_curves(xc, calres, curves))
			error("xcal set_curves error : %s",xc->e.m);
			
		if (xc->write(xc, outname))
			error("xcal write error : %s",xc->e.m);

		xc->del(xc);

		free_dmatrix(curves, 0, devchan-1, 0, calres-1);
	}
	return 0;
}


