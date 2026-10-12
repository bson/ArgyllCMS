
/* 
 * Argyll Color Management System
 * Color Printer Device Model Profile checker.
 * Check an mpp profile against a .ti3 file.
 *
 * Author: Graeme W. Gill
 * Date:   19/3/2003
 *
 * Copyright 2003 Graeme W. Gill
 * All rights reserved.
 *
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 */

#undef DEBUG

#define verbo stdout

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <sys/types.h>
#include <time.h>
#include "copyright.h"
#include "aconfig.h"
#include "numlib.h"
#include "cgats.h"
#include "conv.h"
#include "xicc.h"
#include "prof.h"
#include "vrml.h"
#include "sort.h"
#include "ui.h"

static void DE2RGB(double *out, double in);

void
usage(void) {
	fprintf(stderr,"Check Model Printer Profile, Version %s\n",ARGYLL_VERSION_STR);
	fprintf(stderr,"Author: Graeme W. Gill, licensed under the AGPL Version 3\n");
	fprintf(stderr,"usage: %s [-v] [-c] [-s] [-y] values.ti3 profile.mpp\n",error_program);
	fprintf(stderr," -v          Verbose mode\n");
	fprintf(stderr," -c          Show CIE94 delta E values\n");
	fprintf(stderr," -k          Show CIEDE2000 delta E values\n");
	fprintf(stderr," -s          Check spectral model too\n");
	fprintf(stderr," -y          Detail each value\n");
	fprintf(stderr," -w          Create %s visualisation (mppprofile%s)\n",vrml_format(),vrml_ext());
	fprintf(stderr," -e          Color vectors according to delta E\n");
	fprintf(stderr," values.ti3  Test values to check against\n");
	fprintf(stderr," profile.mpp Profile to check\n");
	exit(1);
	}

int main(int argc, char *argv[])
{
	int fa,nfa;							/* current argument we're looking at */
	int verb = 0;
	int cie94 = 0;						/* Display CIE94 delta E */
	int cie2k = 0;						/* Display CIEDE2000 delta E */
	int verify = 0;
	int ospec = 0;						/* Output spectral model flag */
	int dovrml = 0;
	int doaxes = 0;
	int dodecol = 0;
	static char ti3name[MAXNAMEL+1] = { 0 };	/* Input cgats file base name */
	static char mppname[MAXNAMEL+1] = { 0 };	/* Profile file base name */

	int i, j;
	int ti;					/* Temporary index */
	cgats *icg;				/* input cgats structure */
	int devmask;			/* ICX ink mask of device space */
	int devchan;			/* Number of chanels in device space */
	int isLab = 0;			/* Flag indicating whether PCS is XYZ or Lab */
	int isDisplay = 0;		/* Flag indicating that this is a display device, not output */
	double limit = -1.0;	/* Ink limit */
	instType itype = instUnknown;	/* Spectral instrument type */
	int    spec_n = 0;		/* Number of spectral bands, 0 if not valid */
	double spec_wl_short = 0.0;	/* First reading wavelength in nm (shortest) */
	double spec_wl_long = 0.0;	/* Last reading wavelength in nm (longest) */
	double norm = 0.0;		/* Normalising scale value */
	int nodp;				/* Number of test patches */
	mppcol *cols;			/* Test patches */
	mpp *p;					/* Model Printer Profile */
	static char out_name[MAXNAMEL+10+1], *xl;     /* VRML/X3D name */
	vrml *wrl = NULL;

	double merr = 0.0;
	double aerr = 0.0;
	double nsamps = 0.0;


	error_program = argv[0];

	if (argc <= 1)
		usage();

	/* Process the arguments */
	for (fa = 1;fa < argc;fa++) {

		nfa = fa;					/* skip to nfa if next argument is used */
		if (argv[fa][0] == '-') {	/* Look for any flags */
			char *na = NULL;		/* next argument after flag, null if none */

			if (argv[fa][2] != '\000')
				na = &argv[fa][2];		/* next is directly after flag */
			else {
				if ((fa+1) < argc) {
					if (argv[fa+1][0] != '-') {
						nfa = fa + 1;
						na = argv[nfa];		/* next is seperate non-flag argument */
					}
				}
			}

			if (argv[fa][1] == '?')
				usage();

			else if (argv[fa][1] == 'v' || argv[fa][1] == 'V')
				verb = 1;

			else if (argv[fa][1] == 'c' || argv[fa][1] == 'C') {
				cie94 = 1;
				cie2k = 0;
			}

			else if (argv[fa][1] == 'k' || argv[fa][1] == 'K') {
				cie94 = 0;
				cie2k = 1;
			}

			/* Verify model against input points */
			else if (argv[fa][1] == 'y' || argv[fa][1] == 'Y')
				verify = 1;

			/* Check spectral model */
			else if (argv[fa][1] == 's' || argv[fa][1] == 'S')
				ospec = 1;

			/* VRML/X3D */
			else if (argv[fa][1] == 'w')
				dovrml = 1;

			/* Delta E coloring */
			else if (argv[fa][1] == 'e')
				dodecol = 1;

			else 
				usage();
		} else
			break;
	}

	/* Get the file name argument */
	if (fa >= argc || argv[fa][0] == '-') usage();
    strncpy(ti3name,argv[fa++],MAXNAMEL); ti3name[MAXNAMEL] = '\000';

	if (fa >= argc || argv[fa][0] == '-') usage();
    strncpy(mppname,argv[fa++],MAXNAMEL); mppname[MAXNAMEL] = '\000';

	strncpy(out_name,ti3name,MAXNAMEL-4); out_name[MAXNAMEL-4] = '\000';
	if ((xl = strrchr(out_name, '.')) == NULL)	/* Figure where extention is */
		xl = out_name + strlen(out_name);
	xl[0] = '\000';			/* Remove extension */


	/* Open and look at the .ti3 profile patches file */
	icg = new_cgats();				/* Create a CGATS structure */
	icg->add_other(icg, "CTI3"); 	/* our special input type is Calibration Target Information 3 */

	if (icg->read_name(icg, ti3name))
		error("CGATS file read error : %s",icg->e.m);

	if (icg->ntables == 0 || icg->t[0].tt != tt_other || icg->t[0].oi != 0)
		error ("Input file isn't a CTI3 format file");
	if (icg->ntables != 1)
		error ("Input file doesn't contain exactly one table");

	/* If we requested spectral, check that it is available */
	if (ospec) {
		if ((ti = icg->find_kword(icg, 0, "SPECTRAL_BANDS")) < 0) {
			if (ospec) {
				error ("No spectral data, so no spectral model output");
				ospec = 0;		/* Can't output spectral model */
			}
		} else {
			spec_n = atoi(icg->t[0].kdata[ti]);
			if (spec_n > MPP_MXBANDS) {
				error ("MPP can't cope with %d spectral components", spec_n);
				ospec = 0;		/* Can't output spectral model */
				/* Alternative would be to downsample the spectrum to fit */
			}
		}
	}

	/* read the device class, and call function to create profile. */
	if ((ti = icg->find_kword(icg, 0, "DEVICE_CLASS")) < 0)
		error ("Input file doesn't contain keyword DEVICE_CLASS");

	if (strcmp(icg->t[0].kdata[ti],"OUTPUT") == 0) {
		isDisplay = 0;
	} else if (strcmp(icg->t[0].kdata[ti],"DISPLAY") == 0) {
		isDisplay = 1;
	} else {
		error ("Input file must be for an output device");
	}

	/* Deal with color representation of input */
	{
		char *buf;
		char *inc, *outc;

		if ((ti = icg->find_kword(icg, 0, "COLOR_REP")) < 0)
			error("Input file doesn't contain keyword COLOR_REPS");

		if ((buf = strdup(icg->t[0].kdata[ti])) == NULL)
			error("Malloc failed - color rep");

		/* Split COLOR_REP into device and PCS space */
		inc = buf;
		if ((outc = strchr(buf, '_')) == NULL)
			error("COLOR_REP '%s' invalid", icg->t[0].kdata[ti]);
		*outc++ = '\000';

		if (strcmp(outc, "XYZ") == 0)
			isLab = 0;
		else if (strcmp(outc, "LAB") == 0)
			isLab = 1;
		else
			error("COLOR_REP '%s' invalid (Neither XYZ nor LAB)", icg->t[0].kdata[ti]);

		devmask = icx_char2inkmask(inc); 
		devchan = icx_noofinks(devmask);
		
		if (devchan == 0)
			error("COLOR_REP '%s' invalid (No matching devmask)", icg->t[0].kdata[ti]);
		
		if ((nodp = icg->t[0].nsets) <= 0)
			error ("No sets of data");

		free(buf);
	}

	/* Deal with ink limit */
	if ((ti = icg->find_kword(icg, 0, "TOTAL_INK_LIMIT")) >= 0) {
		double imax;
		imax = atof(icg->t[0].kdata[ti]);
		if (imax > 1e-4 && imax <= (ICX_MXINKS * 100.0)) {
			if (limit > 1e-4 && limit <= (ICX_MXINKS * 100.0)) {
											/* User has specified limit as option */
				if (imax < limit) {
					warning("Ink limit greater than original chart! (%f > %f)",limit,imax);
				}
			} else {
				if (imax > 80.0)
					limit = imax - 10.0;	/* Rule of thumb - 10% below chart maximum */
				else
					limit = imax;
			}
		}
	}


	if (limit > 1e-4 && limit <= (ICX_MXINKS * 100.0)) {
		if (verb)
			printf("Total ink limit being used is %f\n",limit);
		limit = limit/100.0;	/* Set a total ink limit */
	} else {
		if (verb)
			printf("No total ink limit being used\n");
		limit = 0.0;			/* Don't use a limit */
	}

	if (ospec && !isDisplay) {

		/* Deal with instrument type */
		if ((ti = icg->find_kword(icg, 0, "TARGET_INSTRUMENT")) < 0)
			error ("Can't find target instrument needed for FWA compensation");
	
		if ((itype = inst_enum(icg->t[0].kdata[ti])) == instUnknown)
			error ("Unrecognised target instrument '%s'", icg->t[0].kdata[ti]);
	}

	if (verb)
		printf("Device has %d colorants, key = '%s', %s\n", devchan, icx_inkmask2char(devmask, 1),
	           devmask & ICX_ADDITIVE ? "Additive" : "Subtractive"); 

	if ((cols = new_mppcols(nodp, devchan, spec_n)) == NULL)
		error("Malloc failed! - cols (%d colors x %d bytes",nodp,sizeof(mppcol));

	/* Read in all the patch values */
	{
		int chix[ICX_MXINKS];
		char *bident;
		int ii, Xi, Yi, Zi;
		xspect sp;
		char buf[100];
		int  spi[XSPECT_MAX_BANDS];	/* CGATS indexes for each wavelength */

		bident = icx_inkmask2char(devmask, 0); 

		/* Find the device value fields */
		for (j = 0; j < devchan; j++) {
			int ii, imask;
			char fname[100];

			imask = icx_index2ink(devmask, j);
			sprintf(fname,"%s_%s",bident,icx_ink2char(imask));

			if ((ii = icg->find_field(icg, 0, fname)) < 0)
				error ("Input file doesn't contain field %s",fname);
			if (icg->t[0].ftype[ii] != r_t)
				error ("Field %s is wrong type - expect float",fname);
	
			chix[j] = ii;
		}

		if (isLab) {		/* Expect Lab */
			if (verb)
				printf("Using the instruments Lab values\n");
			if ((Xi = icg->find_field(icg, 0, "LAB_L")) < 0)
				error("Input file doesn't contain field LAB_L");
			if (icg->t[0].ftype[Xi] != r_t)
				error("Field LAB_L is wrong type - expect float");
			if ((Yi = icg->find_field(icg, 0, "LAB_A")) < 0)
				error("Input file doesn't contain field LAB_A");
			if (icg->t[0].ftype[Yi] != r_t)
				error("Field LAB_A is wrong type - expect float");
			if ((Zi = icg->find_field(icg, 0, "LAB_B")) < 0)
				error("Input file doesn't contain field LAB_B");
			if (icg->t[0].ftype[Zi] != r_t)
				error("Field LAB_B is wrong type - expect float");

		} else { 		/* Expect XYZ */
			if (verb)
				printf("Using the instruments XYZ values\n");
			if ((Xi = icg->find_field(icg, 0, "XYZ_X")) < 0)
				error("Input file doesn't contain field XYZ_X");
			if (icg->t[0].ftype[Xi] != r_t)
				error("Field XYZ_X is wrong type - expect float");
			if ((Yi = icg->find_field(icg, 0, "XYZ_Y")) < 0)
				error("Input file doesn't contain field XYZ_Y");
			if (icg->t[0].ftype[Yi] != r_t)
				error("Field XYZ_Y is wrong type - expect float");
			if ((Zi = icg->find_field(icg, 0, "XYZ_Z")) < 0)
				error("Input file doesn't contain field XYZ_Z");
			if (icg->t[0].ftype[Zi] != r_t)
				error("Field XYZ_Z is wrong type - expect float");
		}

		/* If we need the spectral information, find the fields */
		if (ospec) {
			if ((ii = icg->find_kword(icg, 0, "SPECTRAL_BANDS")) < 0)
				error ("Input file doesn't contain keyword SPECTRAL_BANDS");
			sp.spec_n = atoi(icg->t[0].kdata[ii]);
			if ((ii = icg->find_kword(icg, 0, "SPECTRAL_START_NM")) < 0)
				error ("Input file doesn't contain keyword SPECTRAL_START_NM");
			sp.spec_wl_short = atof(icg->t[0].kdata[ii]);
			if ((ii = icg->find_kword(icg, 0, "SPECTRAL_END_NM")) < 0)
				error ("Input file doesn't contain keyword SPECTRAL_END_NM");
			sp.spec_wl_long = atof(icg->t[0].kdata[ii]);
			sp.norm = 100.0;
	
			/* Find the fields for spectral values */
			for (j = 0; j < sp.spec_n; j++) {
				int nm;
		
				/* Compute nearest integer wavelength */
				nm = (int)(sp.spec_wl_short + ((double)j/(sp.spec_n-1.0))
				            * (sp.spec_wl_long - sp.spec_wl_short) + 0.5);
				
				sprintf(buf,"SPEC_%03d",nm);
	
				if ((spi[j] = icg->find_field(icg, 0, buf)) < 0)
					error("Input file doesn't contain field %s",buf);

				if (icg->t[0].ftype[spi[j]] != r_t)
					error("Field %s is wrong type - expect float",buf);
			}

			/* Record spectral parameters */
			spec_n = sp.spec_n;
			spec_wl_short = sp.spec_wl_short;
			spec_wl_long = sp.spec_wl_long;
			norm = sp.norm;
		} else {
			spec_n = 0;		/* Not using spectral in model */
		}

		/* Load up all the patch values */
		for (i = 0; i < nodp; i++) {

			/* read in device values */
			for (j = 0; j < devchan; j++)
				cols[i].nv[j] = *((double *)icg->t[0].fdata[i][chix[j]])/100.0;

			/* Read the spectral values for this patch */
			if (ospec) {

				/* norm takes care of 100 scale */
				for (j = 0; j < sp.spec_n; j++) {
					sp.spec[j] = *((double *)icg->t[0].fdata[i][spi[j]]);
					if (ospec)
						cols[i].band[3+j] = sp.spec[j];
				}
			}

			/* Use the instrument CIE values */
			if (isLab) {
				cols[i].band[0] = *((double *)icg->t[0].fdata[i][Xi]);
				cols[i].band[1] = *((double *)icg->t[0].fdata[i][Yi]);
				cols[i].band[2] = *((double *)icg->t[0].fdata[i][Zi]);
				icmLab2XYZ(&icmD50, cols[i].band, cols[i].band);
			} else {
				cols[i].band[0] = *((double *)icg->t[0].fdata[i][Xi])/100.0;
				cols[i].band[1] = *((double *)icg->t[0].fdata[i][Yi])/100.0;
				cols[i].band[2] = *((double *)icg->t[0].fdata[i][Zi])/100.0;
			}
		}

		free(bident);

	}	/* End of reading in CGATs file */

	/* Done with inputs to mpp->create() */
	icg->del(icg);

	if (dovrml) {
		wrl = new_vrml(out_name, doaxes, vrml_lab);
		wrl->start_line_set(wrl, 0);
	}

	merr = 0.0;
	aerr = 0.0;
	nsamps = 0.0;

	if ((p = new_mpp()) == NULL)
		error("Failed to create an mpp");

	if (p->read_mpp(p, mppname))
		error("Read error : %s",p->e.m);

	/* Set just PCS and use XYZ model */
	p->set_ilob(p, icxIT_default, NULL, icxOT_default, NULL, icSigLabData, 0);

	for (i = 0; i < nodp; i++) {
		double out[3], ref[3];
		double mxd;

		/* Lookup the profile PCS value for this data point */
		p->lookup(p, out, cols[i].nv);
	
		/* Convert our cols data to Lab */
		icmXYZ2Lab(&icmD50, ref, cols[i].band);

		if (verify && verb) {
			printf("[%f] ", cie2k ? icmCIE2K(ref, out) :
			                        cie94 ? icmCIE94(ref, out) : icmLabDE(ref, out));
			for (j = 0; j < devchan; j++)
				printf("%6.4f ", cols[i].nv[j]);
			printf("-> %5.1f %5.1f %5.1f should be %5.1f %5.1f %5.1f\n",
			       out[0],out[1],out[2], ref[0],ref[1],ref[2]);
		}

		/* Check the result */
		mxd = cie2k ? icmCIE2K(ref, out) : cie94 ? icmCIE94(ref, out) : icmLabDE(ref, out);
		if (mxd > merr)
			merr = mxd;

		aerr += mxd;
		nsamps++;

		if (dovrml && !ospec) {
			int ix[2];
			double p1[3], p2[3];

			/* Add the vertexes */
			icmCpy3(p1, ref);
			icmCpy3(p2, out);
			ix[0] = wrl->add_vertex(wrl, 0, p1);
			ix[1] = wrl->add_vertex(wrl, 0, p2);

			/* Add the line */
			if (dodecol) {		/* Lines with color determined by length */
				double rgb[3];
				DE2RGB(rgb, icmNorm33(p1, p2));
				wrl->add_col_line(wrl, 0, ix, rgb);

			} else {	/* Natural color */
				wrl->add_line(wrl, 0, ix);
			}
		}
	}
	printf("Read profile %s check complete, avg err = %f, max err = %f\n",
		    cie2k ? "CIEDE2000" : cie94 ? "CIE94" : "Lab", aerr/nsamps, merr); fflush(stdout);

	if (ospec) {
		merr = 0.0;
		aerr = 0.0;
		nsamps = 0.0;

		for (i = 0; i < nodp; i++) {
			xspect spout;
			double avd, mxd;

			/* Lookup the profile spectral value for this data point */
			p->lookup_spec(p, &spout, cols[i].nv);
		
			if (spec_n != spout.spec_n)
				error("Mismatch between original spectral and returned");

			avd = mxd = 0.0;
			for (j = 0; j < spec_n; j++) {
				double ded;
				ded = fabs(spout.spec[j]/spout.norm - cols[i].band[3+j]/norm);
				avd += ded;
				if (ded > mxd)
					mxd = ded;
			}
			avd /= (double)spec_n;

			if (verify && verb) {
				printf("[%f %f] ", avd, mxd);
				for (j = 0; j < devchan; j++)
					printf("%6.4f ", cols[i].nv[j]);
				printf("-> ");
				for (j = 0; j < spec_n; j++)
					printf("%2.0f ", spout.spec[j]); 

				printf("should be ");
				for (j = 0; j < spec_n; j++)
					printf("%2.0f ", cols[i].band[3+j]); 
				printf("\n");
			}

			if (mxd > merr)
				merr = mxd;

			aerr += avd;
			nsamps++;
		}
		printf("profile spectral check complete, avg err = %f%%, max err = %f%%\n",
		       aerr * 100.0/nsamps, merr * 100.0); fflush(stdout);

		/* Check spectrally derived Lab values */
		{
			xsp2cie *sc;
			xspect sp, spout;

			if ((sc = new_xsp2cie(icxIT_D50, 0.0, NULL, icxOT_CIE_1931_2, NULL, icSigLabData, icxClamp)) == NULL)
				error("Failed to create xsp2cie object");

			/* Set standard D50 viewer & illum. */
			p->set_ilob(p, icxIT_D50, NULL, icxOT_CIE_1931_2, NULL, icSigLabData, 0);

			merr = 0.0;
			aerr = 0.0;
			nsamps = 0.0;

			for (i = 0; i < nodp; i++) {
				double out[3], ref[3];
				double mxd;

				/* Lookup the profile spectral value for this data point */
				p->lookup_spec(p, &spout, cols[i].nv);
				/* And convert to Lab */
				sc->convert(sc, out, &spout);
			
				/* Convert our cols ref data to Lab */
				sp.spec_n = spec_n;
				sp.spec_wl_short = spec_wl_short;
				sp.spec_wl_long = spec_wl_long;
				sp.norm = norm;
				for (j = 0; j < spec_n; j++)
					sp.spec[j] = cols[i].band[3+j];
				sc->convert(sc, ref, &sp);

				if (verify && verb) {
					printf("[%f] ", cie2k ? icmCIE2K(ref, out) :
					                cie94 ? icmCIE94(ref, out) : icmLabDE(ref, out));
					for (j = 0; j < devchan; j++)
						printf("%6.4f ", cols[i].nv[j]);
					printf("-> %5.1f %5.1f %5.1f should be %5.1f %5.1f %5.1f\n",
					       out[0],out[1],out[2], ref[0],ref[1],ref[2]);
				}

				/* Check the result */
				mxd = cie2k ? icmCIE2K(ref, out) : cie94 ? icmCIE94(ref, out) : icmLabDE(ref, out);
				if (mxd > merr)
					merr = mxd;

				aerr += mxd;
				nsamps++;

				if (dovrml && !ospec) {
					int ix[2];
					double p1[3], p2[3];

					/* Add the vertexes */
					icmCpy3(p1, ref);
					icmCpy3(p2, out);
					ix[0] = wrl->add_vertex(wrl, 0, p1);
					ix[1] = wrl->add_vertex(wrl, 0, p2);

					/* Add the line */
					if (dodecol) {		/* Lines with color determined by length */
						double rgb[3];
						DE2RGB(rgb, icmNorm33(p1, p2));
						wrl->add_col_line(wrl, 0, ix, rgb);

					} else {	/* Natural color */
						wrl->add_line(wrl, 0, ix);
					}
				}
			}
			printf("Read profile spectral %s check complete, avg err = %f, max err = %f\n",
				    cie2k ? "CIEDE2000" : cie94 ? "CIE94" : "Lab", aerr/nsamps, merr);
			fflush(stdout);

			sc->del(sc);
		}
	}

	if (dovrml) {
		wrl->make_lines_vc(wrl, 0, 0.0);
		wrl->del(wrl);
	}

	p->del(p);

	/* Clean up */
	del_mppcols(cols, nodp, devchan, spec_n);

	return 0;
}

/* ------------------------------------------------ */

/* Convert a delta E value into a signal color: */
static void
DE2RGB(double *out, double in) {
	struct {
		double de;
		double r, g, b;
	} range[6] = {
		{ 10.0, 1, 1, 0 },		/* yellow */
		{ 4.0,  1, 0, 0 },		/* red */
		{ 2.0, 1, 0, 1 },		/* magenta */
		{ 1.0, 0, 0, 1 },		/* blue */
		{ 0.5, 0, 1, 1 },		/* cyan */
		{ 0.0, 0, 1, 0 }		/* green */
	};
	int i;
	double bl;

	/* Locate the range we're in */
	if (in > range[0].de) {
		out[0] = range[0].r;
		out[1] = range[0].g;
		out[2] = range[0].b;
	} else {
		for (i = 0; i < 5; i++) {
			if (in <= range[i].de && in >= range[i+1].de)
				break;
		}
		bl = (in - range[i+1].de)/(range[i].de - range[i+1].de);
		out[0] = bl * range[i].r + (1.0 - bl) * range[i+1].r;
		out[1] = bl * range[i].g + (1.0 - bl) * range[i+1].g;
		out[2] = bl * range[i].b + (1.0 - bl) * range[i+1].b;
	}
}
































