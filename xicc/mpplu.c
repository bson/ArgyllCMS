
/* 
 * Model Printer Profile Lookup test utility.
 *
 * Author:  Graeme W. Gill
 * Date:    2002/12/30
 * Version: 1.00
 *
 * Copyright 2002, 2003, 2025 Graeme W. Gill
 * All rights reserved.
 * This material is licenced under the GNU AFFERO GENERAL PUBLIC LICENSE Version 3 :-
 * see the License.txt file for licencing details.
 *
 */

/* TTBD:
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <fcntl.h>
#include <string.h>
#include <math.h>
#include "aconfig.h"
#include "numlib.h"
#include "conv.h"
#include "xicc.h"
#include "counters.h"
#include "plot.h"
#include "vrml.h"
#include "ui.h"

void usage(void) {
#ifdef COMMPLUS
	fprintf(stderr,"Translate colors through an MPP or MPP2 profile, V1.00\n");
#else
	fprintf(stderr,"Translate colors through an MPP profile, V1.00\n");
#endif
	fprintf(stderr,"Author: Graeme W. Gill, licensed under the AGPL Version 3\n");
	fprintf(stderr,"usage: mpplu [-v] [-f func] [-i intent] [-o order] profile\n");
	fprintf(stderr," -v            Verbose\n");
	fprintf(stderr," -f function   f = forward, b = backwards\n");
	fprintf(stderr," -p oride      x = XYZ_PCS, l = Lab_PCS, y = Yxy\n");
	fprintf(stderr," -l limit   override default ink limit, 1 - N00%%\n");
	fprintf(stderr," -i illum   Choose illuminant for print/transparency spectral data:\n");
	fprintf(stderr,"            A, C, D50 (def.), D50M2, D65, F5, F8, F10 or file.sp\n");
	fprintf(stderr," -o observ  Choose CIE Observer for spectral data:\n");
	fprintf(stderr,"            1931_2 (def), 1964_10, 2015_2, 2015_10, S&B 1955_2, shaw, J&V 1978_2 or file.cmf\n");
	fprintf(stderr," -u         Use Fluorescent Whitening Agent compensation\n");
	fprintf(stderr," -s         Print spectrum for each lookup\n");
	fprintf(stderr," -S         Plot spectrum for each lookup\n");
	fprintf(stderr," -g         Create gamut output\n");
	fprintf(stderr," -w         Create gamut %s as well\n",vrml_format());
	fprintf(stderr," -n         Don't add %s axes\n",vrml_format());
	fprintf(stderr," -a n       Gamut transparency level\n");
	fprintf(stderr," -d n       Gamut surface detail level\n");
	fprintf(stderr," -e         Create %s edge plot\n",vrml_format());
	fprintf(stderr," -E         Create %s all possible edge plot\n",vrml_format());
	fprintf(stderr," -F         Create %s face plot\n",vrml_format());
#ifdef COMMPLUS
	fprintf(stderr," -D level   dump parameters of the mpp2\n");
	fprintf(stderr," -P level   plot parameters of the mpp2\n");
#endif
	fprintf(stderr," -t num     Invoke debugging test code \"num\" 1..n\n");
	fprintf(stderr,"            1 - check partial derivative for device input\n");
	fprintf(stderr,"\n");
	fprintf(stderr,"    The colors to be translated should be fed into stdin,\n");
	fprintf(stderr,"    one input color per line, white space separated.\n");
	fprintf(stderr,"    A line starting with a # will be ignored.\n");
	fprintf(stderr,"    A line not starting with a number will terminate the program.\n");
	exit(1);
}

static void face_plot(mpp *p, double gamres, int doaxes, double trans, char *outname);
static void mpp_rev(mpp *mppo, double limit, double *out, double *in);

int
main(int argc, char *argv[]) {
	int fa,nfa;				/* argument we're looking at */
	static char prof_name[MAXNAMEL+1];
	mpp *mppo = NULL;
#ifdef COMMPLUS
	mpp2 *mppo2 = NULL;		/* mppo aslo set to this, use ->version to cast */
#endif
	int verb = 0;
	int test = 0;			/* special test code */
	int dogam = 0;			/* Create gamut */
	int dodump = 0;			/* Dump the profiles contents */
	int doplot = 0;			/* Plot the profiles contents */
	int dowrl = 0;			/* Create VRML/X3D gamut */
	int doaxes = 1;			/* Create VRML/X3D axes */
	double trans = 0.0;		/* Transparency */
	double gamres = 0.0;	/* Gamut resolution */
	int doedgepl = 0;		/* Create edge plot */
	int dofacepl = 0;		/* Create face plot */
	int repYxy = 0;			/* Report Yxy */
	int repspec = 0;		/* Report Spectral */
	int plotspec = 0;		/* Plot a spectrum on each lookup */
	int bwd = 0;			/* Do reverse lookup */
	double dlimit;			/* Device ink limit */
	double limit = -1.0;	/* Used ink limit */
	int display = 0;		/* NZ if display type */
	int spec = 0;			/* Use spectral data flag */
	int    spec_n;			/* Number of spectral bands, 0 if not valid */
	double spec_wl_short;	/* First reading wavelength in nm (shortest) */
	double spec_wl_long;	/* Last reading wavelength in nm (longest) */
	int fwacomp = 0;		/* FWA compensation */
	icxIllumeType illum = icxIT_default;	/* Spectral defaults */
	xspect cust_illum;		/* Custom illumination spectrum */
	icxObserverType obType = icxOT_default;
	xspect custObserver[3];	/* If obType = icxOT_custom */
	char buf[200];
	double in[MAX_CHAN], out[MAX_CHAN];
	int rv = 0;

	inkmask imask;						/* Device Ink mask */
	char *ident = NULL;					/* Device colorspec description */
	icColorSpaceSignature pcss;			/* Type of PCS space */
	int devn, pcsn;						/* Number of components */

	icColorSpaceSignature pcsor = icSigLabData;	/* Default */
	

	error_program = argv[0];

	if (argc < 2)
		usage();

	/* Process the arguments */
	for (fa = 1;fa < argc;fa++) {
		nfa = fa;					/* skip to nfa if next argument is used */
		if (argv[fa][0] == '-')	{	/* Look for any flags */
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

			/* Verbosity */
			else if (argv[fa][1] == 'v') {
				verb = 1;
			}
			/* function */
			else if (argv[fa][1] == 'f') {
				fa = nfa;
				if (na == NULL) usage();
    			switch (na[0]) {
					case 'f':
					case 'F':
						bwd = 0;
						break;
					case 'b':
					case 'B':
						bwd = 1;
						break;
					default:
						usage();
				}
			}

			/* PCS override */
			else if (argv[fa][1] == 'p') {
				fa = nfa;
				if (na == NULL) usage();
    			switch (na[0]) {
					case 'x':
					case 'X':
						pcsor = icSigXYZData;
						repYxy = 0;
						break;
					case 'l':
					case 'L':
						pcsor = icSigLabData;
						repYxy = 0;
						break;
					case 'y':
					case 'Y':
						pcsor = icSigXYZData;
						repYxy = 1;
						break;
					default:
						usage();
				}
			}

			/* Ink Limit */
			else if (argv[fa][1] == 'l') {
				fa = nfa;
				if (na == NULL) usage();
				limit = atof(na);
			}

			/* Spectral Illuminant type */
			else if (argv[fa][1] == 'i') {
				fa = nfa;
				if (na == NULL) usage();
				if (strcmp(na, "A") == 0) {
					spec = 1;
					illum = icxIT_A;
				} else if (strcmp(na, "C") == 0) {
					spec = 1;
					illum = icxIT_C;
				} else if (strcmp(na, "D50") == 0) {
					spec = 1;
					illum = icxIT_D50;
				} else if (strcmp(na, "D50M2") == 0) {
					spec = 1;
					illum = icxIT_D50M2;
				} else if (strcmp(na, "D65") == 0) {
					spec = 1;
					illum = icxIT_D65;
				} else if (strcmp(na, "F5") == 0) {
					spec = 1;
					illum = icxIT_F5;
				} else if (strcmp(na, "F8") == 0) {
					spec = 1;
					illum = icxIT_F8;
				} else if (strcmp(na, "F10") == 0) {
					spec = 1;
					illum = icxIT_F10;
				} else {	/* Assume it's a filename */
					inst_meas_type mt;

					spec = 1;
					illum = icxIT_custom;
					if (read_xspect(&cust_illum, &mt, NULL, na) != 0)
						usage();

					if (mt != inst_mrt_none
					 && mt != inst_mrt_emission
					 && mt != inst_mrt_ambient
					 && mt != inst_mrt_emission_flash
					 && mt != inst_mrt_ambient_flash) {
						error("Custom illuminant '%s' is wrong measurement type",na);
					}
				}
			}

			/* Spectral Observer type */
			else if (argv[fa][1] == 'o') {
				fa = nfa;
				if (na == NULL) usage();
				if (strcmp(na, "1931_2") == 0) {			/* Classic 2 degree */
					spec = 1;
					obType = icxOT_CIE_1931_2;
				} else if (strcmp(na, "1964_10") == 0) {	/* Classic 10 degree */
					spec = 1;
					obType = icxOT_CIE_1964_10;
				} else if (strcmp(na, "2015_2") == 0) {		/* Latest 2 degree */
					spec = 1;
					obType = icxOT_CIE_2015_2;
				} else if (strcmp(na, "2015_10") == 0) {	/* Latest 10 degree */
					spec = 1;
					obType = icxOT_CIE_2015_10;
				} else if (strcmp(na, "1955_2") == 0) {		/* Stiles and Burch 1955 2 degree */
					spec = 1;
					obType = icxOT_Stiles_Burch_2;
				} else if (strcmp(na, "1978_2") == 0) {		/* Judd and Voss 1978 2 degree */
					spec = 1;
					obType = icxOT_Judd_Voss_2;
				} else if (strcmp(na, "shaw") == 0) {		/* Shaw and Fairchilds 1997 2 degree */
					spec = 1;
					obType = icxOT_Shaw_Fairchild_2;
				} else {	/* Assume it's a filename */
					obType = icxOT_custom;
					if (read_cmf(custObserver, na) != 0)
						usage();
				}
			}

			/* Fluerescent Whitner compensation */
			else if (argv[fa][1] == 'u')
				fwacomp = 1;

			/* Report spectrum on each lookup */
			else if (argv[fa][1] == 's')
				repspec = 1;

			/* Plot spectrum on each lookup */
			else if (argv[fa][1] == 'S')
				plotspec = 1;

			/* Gamut plot */
			else if (argv[fa][1] == 'g')
				dogam = 1;

			/* VRML/X3D plot */
			else if (argv[fa][1] == 'w') {
				dogam = 1;
				dowrl = 1;
			}

			/* No VRML/X3D axes */
			else if (argv[fa][1] == 'n') {
				doaxes = 0;
			}

			/* Transparency */
			else if (argv[fa][1] == 'a') {
				fa = nfa;
				if (na == NULL) usage();
				trans = atof(na);
			}

			/* Surface Detail */
			else if (argv[fa][1] == 'd') {
				fa = nfa;
				if (na == NULL) usage();
				gamres = atof(na);
			}

			/* Edge plot */
			else if (argv[fa][1] == 'e')
				doedgepl = 1;

			else if (argv[fa][1] == 'E')
				doedgepl = 2;

			/* Face plot */
			else if (argv[fa][1] == 'F')
				dofacepl = 1;

#ifdef COMMPLUS
			/* Dump */
			else if (argv[fa][1] == 'D') {
				fa = nfa;
				if (na == NULL) usage();
				dodump = atoi(na);
			}

			/* Plot */
			else if (argv[fa][1] == 'P') {
				fa = nfa;
				if (na == NULL) usage();
				doplot = atoi(na);
			}
#endif
			/* Test code */
			else if (argv[fa][1] == 't') {
				fa = nfa;
				if (na == NULL) usage();
				test = 1;
			}

			else 
				usage();
		} else
			break;
	}

	if (fa >= argc || argv[fa][0] == '-') usage();
	strcpy(prof_name,argv[fa]);


	if ((mppo = new_mpp()) == NULL)
		error("Creation of MPP object failed");

	if ((rv = mppo->read_mpp(mppo,prof_name)) != 0) {
#ifdef COMMPLUS
		mppo->del(mppo);
		mppo = NULL;
		if ((mppo2 = new_mpp2()) == NULL)
			error("Creation of MPP2 object failed");
		if ((rv = mppo2->read(mppo2,prof_name)) != 0)
			error ("%d, %s",rv,mppo2->e.m);
		mppo = (mpp *)mppo2;
#else
		error ("%d, %s",rv,mppo->e.m);
#endif
	}

#ifdef COMMPLUS
	if (mppo2 != NULL) {
		mppo2->get_info(mppo2, &imask, &devn, &dlimit, &spec_n, &spec_wl_short, &spec_wl_long, NULL);
	} else
#endif
	mppo->get_info(mppo, &imask, &devn, &dlimit, &spec_n, &spec_wl_short, &spec_wl_long, NULL, &display);
	ident = icx_inkmask2char(imask, 1); 

	if (verb) {
#ifdef COMMPLUS
		if (mppo2 != NULL)
			printf("MPP2 profile with %d colorants, type %s, TAC %f\n",devn,ident, dlimit);
		else
#endif
		printf("MPP profile with %d colorants, type %s, TAC %f\n",devn,ident, dlimit);
		if (display)
			printf("MPP profile is for a display type device\n");
	}

	if (limit <= 0.0 || dlimit < limit)
		limit = dlimit;

	pcss = pcsor;
	pcsn = 3;

	if (spec && spec_n == 0) {
		error("Spectral profile needed for spectral result, custom illuminant, observer or FWA");
	}

	/* Select CIE return value details */
#ifdef COMMPLUS
	if (mppo2 != NULL) {
		if ((rv = mppo2->set_ilob(mppo2, illum, &cust_illum, obType, custObserver, pcss)) != 0) {
			error("Error setting illuminant, observer for MPP2");
		}
	} else
#endif
	if ((rv = mppo->set_ilob(mppo, illum, &cust_illum, obType, custObserver, pcss, fwacomp)) != 0) {
		if (rv == 1)
			error("Spectral profile needed for custom illuminant, observer or FWA");
		error("Error setting illuminant, observer, or FWA");
	}

#ifdef NEVER
# pragma message("!!!!!!!!!!!!!!!!!! zero'ing itrap & dadj values !!!!!!!!!!!!!!!")
	if (mppo2 != NULL) {
		int i;
		/* Zero out ink combo values */
		for (i = 0; i < mppo2->np; i++) {
			mppo2->m.sep[i] = 0.0;
			mppo2->m.itrap[i] = 0.0;
			mppo2->m.dadj[i] = 0.0;
		}
	}
#endif


#ifdef COMMPLUS
	if (dodump) {
		int ph = 1;

		if (mppo2 == NULL)
			error("Need MPP2 to do dump");

		if (dodump > 1)
			ph |= 2;

		if (dodump > 2)
			ph |= 4;

		mppo2->dump_plot(mppo2, ph, 1, 3);
		exit(0);
	}

	if (doplot) {
		int ph = 1;

		if (mppo2 == NULL)
			error("Need MPP2 to do plot");

		if (doplot > 1)
			ph |= 2;

		if (doplot > 2)
			ph |= 4;

		mppo2->dump_plot(mppo2, ph, 2, 3);
		exit(0);
	}
#endif

	if (test != 0) {
#ifdef COMMPLUS
		if (mppo2 != NULL)
			error("Can't run partial derivative test code on MPP2");
#endif
		printf("!!!!! Running special test code no %d !!!!!\n",test);

		if (test == 1) {
			double **dv, **rdv;

			dv = dmatrix(0, pcsn-1, 0, devn-1);
			rdv = dmatrix(0, pcsn-1, 0, devn-1);

			printf("Checking partial derivative at each input value\n");
			/* Process colors to translate */
			for (;;) {
				int i,j;
				char *bp, *nbp;
				double tout[MAX_CHAN];

				/* Read in the next line */
				if (fgets(buf, 200, stdin) == NULL)
					break;
				if (buf[0] == '#') {
					fprintf(stdout,"%s\n",buf);
					continue;
				}
				/* For each input number */
				for (nbp = buf, i = 0; i < MAX_CHAN; i++) {
					bp = nbp;
					in[i] = strtod(bp, &nbp);
					if (nbp == bp)
						break;			/* Failed */
				}
				if (i == 0)
					break;

				/* Do conversion */
				mppo->lookup(mppo, out, in);
				mppo->dlookup(mppo, out, dv, in);
		
				for (j = 0; j < devn; j++) {
					double del = 1e-9;

					if (in[j] > 0.5)
						del = -del;

					in[j] += del;
					mppo->lookup(mppo, tout, in);
					in[j] -= del;
					
					for (i = 0; i < pcsn; i++) {
						rdv[i][j] = (tout[i] - out[i])/del;
					}
				}

				/* Output the results */
				for (j = 0; j < devn; j++) {
					if (j > 0)
						fprintf(stdout," %f",in[j]);
					else
						fprintf(stdout,"%f",in[j]);
				}
				printf(" [%s] -> ", ident);
		
				for (j = 0; j < pcsn; j++) {
					if (j > 0)
						fprintf(stdout," %f",out[j]);
					else
						fprintf(stdout,"%f",out[j]);
				}
				printf(" [%s]\n", icm2str(icmColorSpaceSig, pcss));

				/* Print the derivatives */
				for (i = 0; i < pcsn; i++) {
					
					printf("Output chan %d: ",i);
					for (j = 0; j < devn; j++) {
						if (j < (devn-1))
							fprintf(stdout,"%f ref %f, ",dv[i][j], rdv[i][j]);
						else
							fprintf(stdout,"%f ref %f\n",dv[i][j], rdv[i][j]);
					}
				}
			}

			free_dmatrix(dv, 0, pcsn-1, 0, devn-1);
			free_dmatrix(rdv, 0, pcsn-1, 0, devn-1);

		} else {
			printf("Unknown test!\n");
		}

	}

	if (dogam) {
		gamut *gam;
		char *xl, gam_name[MAXNAMEL+1];
		int docusps = 1;

#ifdef COMMPLUS
		if (mppo2 != NULL) {
			if ((gam = mppo2->get_gamut(mppo2, gamres)) == NULL)
				error("get_gamut failed\n");
		} else
#endif
		if ((gam = mppo->get_gamut(mppo, gamres)) == NULL)
			error("get_gamut failed\n");

		strcpy(gam_name, prof_name);
		if ((xl = strrchr(gam_name, '.')) == NULL)	/* Figure where extention is */
			xl = gam_name + strlen(gam_name);
		strcpy(xl,".gam");
		if (gam->write_gam(gam, gam_name))
			error ("write gamut failed on '%s'",gam_name);
	
		if (dowrl) {
			xl[0] = '\000';
			if (gam->write_vrml(gam,gam_name, doaxes, docusps))
				error ("write vrml failed on '%s%s'",gam_name,vrml_ext());
		}

		gam->del(gam);

	/* Cube edges */
	} else if (doedgepl == 1) {
		vrml *wrl = NULL;
		char *xl, edge_name[MAXNAMEL+1];
		int docusps = 1;

		int lres;
		int cc, i, j;

		if (gamres <= 0.0)
			gamres = 5.0;

		lres = (int)(100.0/gamres + 0.5);
		if (lres < 2)
			lres = 2;

		strcpy(edge_name, prof_name);
		if ((xl = strrchr(edge_name, '.')) == NULL)	/* Figure where extention is */
			xl = edge_name + strlen(edge_name);
		strcpy(xl,"_e");

		wrl = new_vrml(edge_name, doaxes, vrml_lab);

		/* For all verticies minus one dimension */
		for (cc = 0; cc < (1 << (devn-1)); cc++) {

			/* For the other dimension being each channel */
			for (i = 0; i < devn; i++) {
				int ii, mm;

				/* Set static input values */
				for (mm = 1, ii = 0; ii < devn; ii++) {
					if (i == ii)
						continue;	/* Skip channel we're going to step along */
					if (cc & mm)
						in[ii] = 1.0;
					else
						in[ii] = 0.0;
					mm <<= 1;
				}

				wrl->start_line_set(wrl, 0);

				/* Step along the edge */
				for (j = 0; j < lres; j++) {
					in[i] = j/(lres-1.0);
#ifdef COMMPLUS
					if (mppo2 != NULL)
						mppo2->lookup(mppo2, out, in);
					else
#endif
					mppo->lookup(mppo, out, in);
//printf("~1 dev %s -> Lab %s\n",debPdv(devn,in),debPdv(3,out));

					wrl->add_vertex(wrl, 0, out);
				}
				wrl->make_last_vertex(wrl, 0);
				wrl->make_lines(wrl, 0, 0);
			}
		}

#ifdef NEVER		/* Put labels on the veticies using DCOUNT */
# pragma message("!!!!!!!!!!!!!!!!!! Vertex label code enabled !!!!!!!!!!!!!!!")
		{
			DCOUNT(coa, MAX_CHAN, 0, 0, 0, 2);
			coa_di = devn;

			DC_INIT(coa);
			while(!DC_DONE(coa)) {
				int e;
				double in[MAX_CHAN];
				double out[3];
				int j, ii;
				char text[100];

				for (e = 0; e < devn; e++)
					in[e] = (double)coa[e];		/* vertex value */

#ifdef COMMPLUS
				if (mppo2 != NULL)
					mppo2->lookup(mppo2, out, in);
				else
#endif
				mppo->lookup(mppo, out, in);

				j = 0;
				text[j++] = ' ';
				text[j++] = 'P';
				for (ii = 0; ii < devn; ii++) {
					if (in[ii] != 0.0)
						text[j++] = '1';				
					else
						text[j++] = '0';				
				}
				text[j++] = '\000';
//printf("~1 dev %s -> Lab %s at '%s'\n",debPdv(devn,in),debPdv(3,out),text);
				wrl->add_text(wrl, text, out, NULL, 1.0);
			/* Increment index within block */
			DC_INC(coa);
		}
	}
#endif	// NEVER (labels)

		wrl->del(wrl);

#ifdef NEVER	/* Plot a specific segment in L*a*b* */
# pragma message("!!!!!!!!!!!!!!!!!! PLOTING SEGMENT code enabled !!!!!!!!!!!!!!!")
	{
#define GRES 256
//		double p1[MAX_CHAN] = { 1.0, 1.0, 0.0, 0.0 };
//		double p2[MAX_CHAN] = { 1.0, 1.0, 1.0, 0.0 };
//		double p1[MAX_CHAN] = { 0.0, 1.0, 0.0, 1.0 };
//		double p2[MAX_CHAN] = { 0.0, 1.0, 1.0, 1.0 };
		double p1[MAX_CHAN] = { 1.0, 1.0, 1.0, 0.0 };
		double p2[MAX_CHAN] = { 1.0, 1.0, 1.0, 1.0 };
		double xx[GRES];
		double y0[GRES];
		double y1[GRES];
		double y2[GRES];
		double *yy[MXGPHS];

#ifdef NEVER
# pragma message("!!!!!!!!!!!!!!!!!! DEBUG_NINK set !!!!!!!!!!!!!!!")
		extern int nink_db;		/* = 1 .. 4 */
		nink_db = 4;
#endif

		for (j = 0; j < GRES; j++) {
			double bl = j/(GRES-1.0);

//			bl = 0.09233 + 0.00002 * bl;

			vect_blend(in, p1, p2, (bl), devn);
#ifdef COMMPLUS
			if (mppo2 != NULL)
				mppo2->lookup(mppo2, out, in);
			else
#endif
			mppo->lookup(mppo, out, in);
printf("~1 dev %s -> Lab %s\n",debPdv(devn,in),debPdv(3,out));
			xx[j] = bl;
			y0[j] = out[0];
			y1[j] = out[1];
			y2[j] = out[2];
		}
#ifdef NEVER
for (j = GRES-1; j >= 0; j--) {
	y0[j] -= y0[0];
	y1[j] -= y1[0];
	y2[j] -= y2[0];
}
#endif
		yy[0] = y0;
		yy[1] = y1;
		yy[2] = y2;
		for (i = 3; i < MXGPHS; i++)
			yy[i] = NULL;
		printf("Vector %s -> %s Lab\n",debPdv(devn,p1),debPdv(devn,p2));
		do_plotNpwz(xx, yy, GRES, NULL, NULL, 0, 1, 0);
	}
#undef GRES
#endif /* NEVER */

	/* All possible edges */
	} else if (doedgepl == 2) {
		vrml *wrl = NULL;
		char *xl, edge_name[MAXNAMEL+1];
		int docusps = 1;

		int lres;
		int c1, c2, i, j;

		if (gamres <= 0.0)
			gamres = 5.0;

		lres = (int)(100.0/gamres + 0.5);
		if (lres < 2)
			lres = 2;

		strcpy(edge_name, prof_name);
		if ((xl = strrchr(edge_name, '.')) == NULL)	/* Figure where extention is */
			xl = edge_name + strlen(edge_name);
		strcpy(xl,"_e");

		wrl = new_vrml(edge_name, doaxes, vrml_lab);

		/* For all verticies */
		for (c1 = 0; c1 < (1 << devn)-1; c1++) {

			/* For all other verticies */
			for (c2 = c1 + 1; c2 < (1 << devn); c2++) {
				double p1[MAX_CHAN], p2[MAX_CHAN];
				int ii, mm;

				/* Set start and end device values */
				for (mm = 1, ii = 0; ii < devn; ii++) {
					if (c1 & mm)
						p1[ii] = 1.0;
					else
						p1[ii] = 0.0;
					if (c2 & mm)
						p2[ii] = 1.0;
					else
						p2[ii] = 0.0;
					mm <<= 1;
				}

				wrl->start_line_set(wrl, 0);

				/* Step along the edge */
				for (j = 0; j < lres; j++) {
					double bl = j/(lres-1.0);

					vect_blend(in, p1, p2, bl, devn);
#ifdef COMMPLUS
					if (mppo2 != NULL)
						mppo2->lookup(mppo2, out, in);
					else
#endif
					mppo->lookup(mppo, out, in);
//printf("~1 dev %s -> Lab %s\n",debPdv(devn,in),debPdv(3,out));

					wrl->add_vertex(wrl, 0, out);
				}
				wrl->make_last_vertex(wrl, 0);
				wrl->make_lines(wrl, 0, 0);
			}
		}

		wrl->del(wrl);
	}

	/* Plot cube faces */
	if (dofacepl) {
		vrml *wrl = NULL;
		char *xl, face_name[MAXNAMEL+1];

		if (gamres <= 0.0)
			gamres = 20.0;

		strcpy(face_name, prof_name);
		if ((xl = strrchr(face_name, '.')) == NULL)	/* Figure where extention is */
			xl = face_name + strlen(face_name);
		strcpy(xl,"_f");

		face_plot(mppo, gamres, doaxes, trans, face_name);

	}
	if (!doedgepl && !dofacepl) {
		/* Normal color lookup */

#ifdef NEVER
# pragma message("!!!!!!!!!!!!!!!!!! DEBUG_NINK set !!!!!!!!!!!!!!!")
		extern int nink_db;		/* = 1 .. 4 */
		nink_db = 4;
#endif

//printf("~1 line %d\n",__LINE__);
		if (repYxy) {	/* report Yxy rather than XYZ */
			if (pcss == icSigXYZData)
				pcss = icSigYxyData; 
		}

		/* Process colors to translate */
		for (;;) {
			int i,j;
			char *bp, *nbp;

			/* Read in the next line */
			if (fgets(buf, 200, stdin) == NULL)
				break;
			if (buf[0] == '#') {
				fprintf(stdout,"%s\n",buf);
				continue;
			}
			/* For each input number */
			for (nbp = buf, i = 0; i < MAX_CHAN; i++) {
				bp = nbp;
				in[i] = strtod(bp, &nbp);
				if (nbp == bp)
					break;			/* Failed */
			}
			if (i == 0)
				break;

			if (!bwd) {
				xspect ospec;

				if (repspec || plotspec) {

					/* Do lookup of spectrum */
#ifdef COMMPLUS
					if (mppo2 != NULL)
						mppo2->lookup_spec(mppo2, &ospec, in);
					else
#endif
					mppo->lookup_spec(mppo, &ospec, in);
			
					if (repspec) {
						/* Output the results */
						for (j = 0; j < devn; j++) {
							if (j > 0)
								fprintf(stdout," %f",in[j]);
							else
								fprintf(stdout,"%f",in[j]);
						}
						printf(" [%s] -> ", ident);
				
						for (j = 0; j < spec_n; j++) {
							if (j > 0)
								fprintf(stdout," %f",ospec.spec[j]);
							else
								fprintf(stdout,"%f",ospec.spec[j]);
						}

						printf(" [%3.0f .. %3.0f nm]\n", spec_wl_short, spec_wl_long);
					}
				}

				/* Do conversion */
#ifdef COMMPLUS
				if (mppo2 != NULL)
					mppo2->lookup(mppo2, out, in);
				else
#endif
				mppo->lookup(mppo, out, in);
		
				/* Output the results */
				for (j = 0; j < devn; j++) {
					if (j > 0)
						fprintf(stdout," %f",in[j]);
					else
						fprintf(stdout,"%f",in[j]);
				}
				printf(" [%s] -> ", ident);
		
				if (repYxy && pcss == icSigYxyData) {
					double X = out[0];
					double Y = out[1];
					double Z = out[2];
					double sum = X + Y + Z;
					if (sum < 1e-6) {
						out[0] = out[1] = out[2] = 0.0;
					} else {
						out[0] = Y;
						out[1] = X/sum;
						out[2] = Y/sum;
					}
				}
				for (j = 0; j < pcsn; j++) {
					if (j > 0)
						fprintf(stdout," %f",out[j]);
					else
						fprintf(stdout,"%f",out[j]);
				}

				printf(" [%s]\n", icm2str(icmColorSpaceSig, pcss));

				if (plotspec) {
					xspect_plot_w(&ospec, NULL, NULL, 0);
				}

			} else {	/* Do a reverse lookup */

				if (repYxy && pcss == icSigYxyData) {
					double Y = in[0];
					double x = in[1];
					double y = in[2];
					double z = 1.0 - x - y;
					double sum;
					if (y < 1e-6) {
						in[0] = in[1] = in[2] = 0.0;
					} else {
						sum = Y/y;
						in[0] = x * sum;
						in[1] = Y;
						in[2] = z * sum;
					}
				}

				/* Do conversion */
#ifdef COMMPLUS
				if (mppo2 != NULL)
					mpp_rev((mpp *)mppo2, limit, out, in);
				else
#endif
				mpp_rev(mppo, limit, out, in);

				/* Output the results */
				for (j = 0; j < pcsn; j++) {
					if (j > 0)
						fprintf(stdout," %f",in[j]);
					else
						fprintf(stdout,"%f",in[j]);
				}
				printf(" [%s] -> ", icm2str(icmColorSpaceSig, pcss));
		
				for (j = 0; j < devn; j++) {
					if (j > 0)
						fprintf(stdout," %f",out[j]);
					else
						fprintf(stdout,"%f",out[j]);
				}

				printf(" [%s]\n", ident);
			}
		}
	}

	free(ident);
#ifdef COMMPLUS
	if (mppo2 != NULL)
		mppo2->del(mppo2);
	else
#endif
	mppo->del(mppo);

	return 0;
}


/* -------------------------------------------- */
/* Code for special gamut surface plot */

#define GAMUT_LCENT 50

/* Create a face gamut, illustrating */
/* device space "fold-over" */
/* (This will be in the current PCS, but assumed to be Lab) */
static void face_plot(
mpp *p,				/* This */
double detail,		/* Gamut resolution detail */
int doaxes,
double trans,		/* Transparency */
char *outname		/* Output VRML/X3D file (no extension) */
) {
#ifdef COMMPLUS
	mpp2 *p2 = NULL; 
#endif 
	int i, j;
	vrml *wrl;
	int vix;						/* Vertex index */
	int nix;						/* Node index */
	int fix;
	double col[MPP_MXCCOMB][3];		/* Color asigned to each major vertex */
	int n, nn;
	int res;
	DCOUNT(coa, MAX_CHAN, 0, 0, 0, 2);

#ifdef COMMPLUS
	if (p->version >= 2) {
		p2 = (mpp2 *)p;
		n = p2->n;
		nn = p2->nn;
	} else
#endif
	{
		n = p->n;
		nn = p->nn;
	}

	coa_di = n;

	/* Asign some colors to the combination nodes */
	for (i = 0; i < nn; i++) {
		int a, b, c, j;
		double h;

		j = (i ^ 0x5a5a5a5a) % nn;
		h = (double)j/(nn-1);

		/* Make fully saturated with chosen hue */
		if (h < 1.0/3.0) {
			a = 0;
			b = 1;
			c = 2;
		} else if (h < 2.0/3.0) {
			a = 1;
			b = 2;
			c = 0;
			h -= 1.0/3.0;
		} else {
			a = 2;
			b = 0;
			c = 1;
			h -= 2.0/3.0;
		}
		h *= 3.0;

		col[i][a] = (1.0 - h);
		col[i][b] = h;
		col[i][c] = d_rand(0.0, 1.0);
	}

	if (detail > 0.0)
		res = (int)(100.0/detail);	/* Establish an appropriate sampling density */
	else
		res = 4;

	if (res < 2)
		res = 2;

	if ((wrl = new_vrml(outname, doaxes, vrml_lab)) == NULL)
		error("new_vrml faile for file '%s%s'",outname,vrml_ext());

	wrl->start_line_set(wrl, 0);

	/* Itterate over all the faces in the device space */
	/* generating the vertex positions. */
	DC_INIT(coa);
	vix = 0;
	nix = 0;
	fix = 0;
	while(!DC_DONE(coa)) {
		int e, m1, m2;
		double in[MAX_CHAN];
		double out[3];
		double sum;
		double xb, yb;

		/* Scan only device surface */
		for (m1 = 0; m1 < n; m1++) {
			if (coa[m1] != 0)
				continue;

			for (m2 = m1 + 1; m2 < n; m2++) {
				int x, y;
				double fcol[3];

				if (coa[m2] != 0)
					continue;

				for (sum = 0.0, e = 0; e < n; e++)
					in[e] = (double)coa[e];		/* Base value */

				fcol[0] = d_rand(0.0, 1.0);
				fcol[1] = d_rand(0.0, 1.0);
				fcol[2] = d_rand(0.0, 1.0);

				fix++;

				/* Scan over 2D device space face */
				for (x = 0; x < res; x++) {				/* step over surface */
					xb = in[m1] = x/(res - 1.0);
					for (y = 0; y < res; y++) {
						int v0, v1, v2, v3;
						double rgb[3];
						yb = in[m2] = y/(res - 1.0);

						/* Lookup PCS value */
#ifdef COMMPLUS
						if (p2 != NULL)
							p2->lookup(p2, out, in);
						else
#endif
						p->lookup(p, out, in);
//printf("  ~1 dev1 %s, Lab %s\n",debPdvf(n,"%.2f",in),debPdv(3,out));

#ifdef NEVER
						/* Create a color */
						for (v0 = 0, e = 0; e < n; e++)
							v0 |= coa[e] ? (1 << e) : 0;		/* Binary index */

						v1 = v0 | (1 << m2);				/* Y offset */
						v2 = v0 | (1 << m2) | (1 << m1);	/* X+Y offset */
						v3 = v0 | (1 << m1);				/* Y offset */

						/* Linear interp between the main vertices */
						for (j = 0; j < 3; j++) {
							rgb[j] = (1.0 - yb) * (1.0 - xb) * col[v0][j]
							       +        yb  * (1.0 - xb) * col[v1][j]
							       + (1.0 - yb) *        xb  * col[v3][j]
							       +        yb  *        xb  * col[v2][j];
						}
						wrl->add_col_vertex(wrl, 0, out, rgb);
#else
						wrl->add_col_vertex(wrl, 0, out, fcol);
#endif

						vix++;
					}
				}
			}
		}
		/* Increment index within block */
		DC_INC(coa);
		nix++;
	}

	/* Itterate over all the faces in the device space */
	/* generating the quadrilateral indexes. */
	DC_INIT(coa);
	vix = 0;
	while(!DC_DONE(coa)) {
		int e, m1, m2;
		double in[MAX_CHAN];
		double sum;

		/* Scan only device surface */
		for (m1 = 0; m1 < n; m1++) {
			if (coa[m1] != 0)
				continue;

			for (m2 = m1 + 1; m2 < n; m2++) {
				int x, y;

				if (coa[m2] != 0)
					continue;

				for (sum = 0.0, e = 0; e < n; e++)
					in[e] = (double)coa[e];		/* Base value */

				/* Scan over 2D device space face */
				for (x = 0; x < res; x++) {				/* step over surface */
					for (y = 0; y < res; y++) {

						if (x < (res-1) && y < (res-1)) {
							int ix[4];
							/* Hmm. add both directions to avoid figuring orientation... */
							ix[0] = vix;
							ix[1] = vix + 1;
							ix[2] = vix + 1 + res;
							ix[3] = vix + res;
							wrl->add_quad(wrl, 0, ix);
							ix[0] = vix;
							ix[1] = vix + res;
							ix[2] = vix + 1 + res;
							ix[3] = vix + 1;
							wrl->add_quad(wrl, 0, ix);
						}
						vix++;
					}
				}
			}
		}
		/* Increment index within block */
		DC_INC(coa);
	}
	wrl->make_quads_vc(wrl, 0, trans);

	if (wrl->flush(wrl) != 0)
		error("Error closing output file '%s%s'",outname,vrml_ext());

	wrl->del(wrl);
}

/* -------------------------------------------- */
/* Reverse lookup support */
/* This is for developing the appropriate reverse lookup */
/* code for xsep.c */

/*
 * TTBD:
 * 		Not handlink rule or separate black
 *      Not handling linearisation callback for ink limit.
 *      Not allowing for other possible secondary limits/goals/tunings.
 */

/*
 * Descriptionr:
 *
 *	The primary reverse lookup optimisation goals are to remain within
 *	the device gamut, and to match the target PCS.
 *
 *	The secondary optimisation goals for solving this under constrained
 *	problem can be chosen to a achieve a wide variety of possible aims.
 *
 *   1) One that is applicable to screened devices might be to use the extra
 *	 inks to minimise the visiblility of screening. For screening resolutions
 *   above 50 lpi (equivalent to 100 dpi stocastic screens) only luminance
 *	 contrast will be relavant, so priority to the inks closest to paper white
 *   measured purely by L distance is appropriate. (In practice I've used
 *	 a measure that adds a small color distance component.)
 *
 *	 2) Another possible goal would be to optimise fit to a desired spectral
 *	 profile, if spectral information is avaiable as an aim. The goal would
 *	 be best fit weighted to the visual sensitivity curve.
 *
 *   3) Another possible secondary goal might be to minimise the total
 *   amount of ink used, to minimise cost, drying time, media ink loading.
 *
 *   4) A fourth possible secondary goal might be to choose ink combinations
 *	 that are the most robust given an error in device values.
 *
 *  In practice these secondary goals need not be entirely exclusive,
 *	but the overall secondary goal could be a weighted blending between
 *  these goals. Overall the ink limit (TAC) is extremely important,
 *  since this will be the primary thing that stops large amounds of
 *  light ink being used at all times.
 *
 *	Numerous other tweaks, limits or goals (such as secondary combination
 *  ink limits, exclusions such as Cyan/Oraange, Magenta/Green) 
 *  could also be applied in the reverse optimisation routine.
 *
 */


#ifdef NEVER
/* These weights are the "theoreticaly correct" weightings, since */
/* at 50 lpi or higher, the color contrast sensitivity should be close to 0 */
# define L_WEIGHT 1.0
# define a_WEIGHT 0.0
# define b_WEIGHT 0.0
#else
/* These weights give us our "expected" ink ordering of */
/* Yellow, light Cyan/Magenta, Orange/Green, Cyan/Magenta, Black. */
# define L_WEIGHT 1.0
# define a_WEIGHT 0.4
# define b_WEIGHT 0.2
#endif

/* Start array entry */
typedef struct {
	double dev[MAX_CHAN];	/* Device value */
	double Lab[3];			/* Lab value */
	double oerr;			/* Order error */
} saent;

/* Context for reverse lookup */
typedef struct {
	int pass;
	int di;				/* Number of device channels */
	double Lab[3];		/* Lab target value */
	void (*dev2lab) (void *d2lcntx, double *out, double *in);	/* Device to Lab callback */
	void *d2lcntx;		/* dev2lab callback context */
	double ilimit;		/* Total limit */
	int sord[MAX_CHAN];	/* Sorted order index */
	double oweight[MAX_CHAN];	/* Order weighting (not used ?) */
} revlus;

/* Return the largest distance of the point outside the device gamut. */
/* This will be 0 if inside the gamut, and > 0 if outside.  */
static double
dist2gamut(revlus *s, double *d) {
	int e;
	int di = s->di;
	double tt, dd = 0.0;
	double ss = 0.0;

	for (e = 0; e < di; e++) {
		ss += d[e];

		tt = 0.0 - d[e];
		if (tt > 0.0) {
			if (tt > dd)
				dd = tt;
		}
		tt = d[e] - 1.0; 
		if (tt > 0.0) {
			if (tt > dd)
				dd = tt;
		}
	}
	tt = ss - s->ilimit;
	if (tt > 0.0) {
		if (tt > dd)
			dd = tt;
	}
	return dd;
}

/* Snap a point to the device gamut boundary. */
/* Return nz if it has been snapped. */
static int snap2gamut(revlus *s, double *d) {
	int e;
	int di = s->di;
	double dd;			/* Smallest distance */
	double ss;			/* Sum */
	int rv = 0;

	/* Snap to ink limit first */
	for (ss = 0.0, e = 0; e < di; e++)
		ss += d[e];
	dd = fabs(ss - s->ilimit);

	if (dd < 0.0) {
		int j;
		for (j = 0; j < di; j++) 
			d[j] *= s->ilimit/ss;	/* Snap to ink limit */
		rv = 1;
	}

	/* Now snap to any other dimension */
	for (e = 0; e < di; e++) {

		dd = fabs(d[e] - 0.0);
		if (dd < 0.0) {
			d[e] = 0.0;		/* Snap to orthogonal boundary */
			rv = 1;
		}
		dd = fabs(1.0 - d[e]); 
		if (dd < 0.0) {
			d[e] = 1.0;		/* Snap to orthogonal boundary */
			rv = 1;
		}
	}

	return rv;
}

/* Reverse optimisation function handed to powell() */
static double revoptfunc(void *edata, double *v) {
	revlus *s = (revlus *)edata;
	double rv;

//printf("~1 target %f %f %f\n",s->Lab[0],s->Lab[1],s->Lab[2]);

	if ((rv = (dist2gamut(s, v))) > 0.0) {
//		rv = rv * 1000.0 + 45000.0;		/* Discourage being out of gamut */
		rv = rv * 5e6;		/* Discourage being out of gamut */

	}
//printf("~1 out of gamut error = %f\n",rv);
	{
		int j;
		double oerr;
		double Lab[3];
		double tot;

		/* Convert device to Lab */
		s->dev2lab(s->d2lcntx, Lab, v);

		/* Accumulate total delta E squared */
		for (j = 0; j < 3; j++) {
			double tt = s->Lab[j] - Lab[j];
			rv += tt * tt;
		}

//printf("~1 Delta E squared = %f\n",rv);

		/* Skip first 3 colorants */
		oerr = tot = 0.0;
//printf("oerr = %f\n",oerr);
		for (j = 3; j < s->di; j++) {
			int ix = s->sord[j];		/* Sorted order index */
			double vv = v[ix];
			double we = (double)j - 2.0;	/* Increasing weight */
			
//printf("Comp %d value %f\n",ix,vv);
			if (vv > 0.0001) {
				oerr += tot + we * vv;
//printf("Added %f + %f to give oerr %f\n",tot,we * vv,oerr);
			}
			tot += we;
		}
		oerr /= tot;
		if (s->pass == 0)
			oerr *= 2000.0;
		else
			oerr *= 1.0;
//printf("Final after div by %f oerr = %f\n",tot,oerr);

//printf("~1 Order error %f\n",oerr);
		rv += oerr;
	}

//printf("~1 Returning total error %f\n",rv);
	return rv;
}


/* Do a reverse lookup on the mpp */
static void mpp_rev(
mpp *mppo,
double limit,		/* Ink limit */
double *out,		/* Device value */
double *in			/* Lab target */
) {
	int i, j;
	inkmask imask;	/* Device Ink mask */
	int inn;
	revlus rs;		/* Reverse lookup structure */
	double sr[MAX_CHAN];	/* Search radius */
	double tt;
	/* !!! This needs to be cached elsewhere !!!! */
	static saent *start = NULL;	/* Start array */
	static int nisay = 0;		/* Number in start array */
#ifdef COMMPLUS
	mpp2 *mppo2 = NULL;

	if (mppo->version == 2) {
		mppo2 = (mpp2 *)mppo;
		mppo = NULL;
	}
#endif

#ifdef COMMPLUS
	if (mppo2 != NULL)
		mppo2->get_info(mppo2, &imask, &inn, NULL, NULL, NULL, NULL, NULL);
	else
#endif
	mppo->get_info(mppo, &imask, &inn, NULL, NULL, NULL, NULL, NULL, NULL);

	rs.di = inn;				/* Number of device channels */

	rs.Lab[0] = in[0];		/* Target PCS value */
	rs.Lab[1] = in[1];
	rs.Lab[2] = in[2];

#ifdef COMMPLUS
	if (mppo2 != NULL) {
		/* Dev->PCS Lookup function and context */
		rs.dev2lab = (void (*)(void *, double *, double *))mppo2->lookup;
		rs.d2lcntx = (void *)mppo2;
	} else
#endif
	{
		/* Dev->PCS Lookup function and context */
		rs.dev2lab = (void (*)(void *, double *, double *))mppo->lookup;
		rs.d2lcntx = (void *)mppo;
	}

	rs.ilimit = limit;		/* Total ink limit */

	{
		double Labw[3];				/* Lab value of white */
		double Lab[MAX_CHAN][3];	/* Lab value of device primaries */
		double min, max;
		
		/* Lookup the L value of all the device primaries */
		for (j = 0; j < inn; j++)
			out[j] = 0.0;
	
#ifdef COMMPLUS
		if (mppo2 != NULL)
			mppo2->lookup(mppo2, Labw, out);
		else
#endif
		mppo->lookup(mppo, Labw, out);

		for (i = 0; i < inn; i++) {
			double tt;
			double de;

			out[i] = 1.0;
#ifdef COMMPLUS
			if (mppo2 != NULL)
				mppo2->lookup(mppo2, Lab[i], out);
			else
#endif

			/* Use DE measure heavily weighted towards L only */
			tt = L_WEIGHT * (Labw[0] - Lab[i][0]);
			de = tt * tt;
			tt = 0.4 * (Labw[1] - Lab[i][1]);
			de += tt * tt;
			tt = 0.2 * (Labw[2] - Lab[i][2]);
			de += tt * tt;
			rs.oweight[i] = sqrt(de);
			out[i] = 0.0;
		}

		/* Normalise weights from 0 .. 1.0 */
		min = 1e6, max = 0.0;
		for (j = 0; j < inn; j++) {
			if (rs.oweight[j] < min)
				min = rs.oweight[j];
			if (rs.oweight[j] > max)
				max = rs.oweight[j];
		}
		for (j = 0; j < inn; j++)
			rs.oweight[j] = (rs.oweight[j] - min)/(max - min);

		{
			for (j = 0; j < inn; j++)
				rs.sord[j] = j;

			for (i = 0; i < (inn-1); i++) {
				for (j = i+1; j < inn; j++) {
					if (rs.oweight[rs.sord[i]] > rs.oweight[rs.sord[j]]) {
						int xx;
						xx = rs.sord[i];
						rs.sord[i] = rs.sord[j];
						rs.sord[j] = xx;
					}
				}
			}
		}

for (j = 0; j < inn; j++)
printf("~1 oweight[%d] = %f\n",j,rs.oweight[j]);
for (j = 0; j < inn; j++)
printf("~1 sorted oweight[%d] = %f\n",j,rs.oweight[rs.sord[j]]);
	}

	/* Initialise the start point array */
	if (start == NULL) {
		int mxstart;
		int steps = 4;

		DCOUNT(dix, MAX_CHAN, inn, 0, 0, steps);

printf("~1 initing start point array\n");
		for (mxstart = 1, j = 0; j < inn; j++) 		/* Compute maximum entries */
			mxstart *= steps;

printf("~1 mxstart = %d\n",mxstart);
		if ((start = malloc(sizeof(saent) * mxstart)) == NULL)
			error("mpp_rev malloc of start array failed\n");

		nisay = 0;
		DC_INIT(dix);

		while(!DC_DONE(dix)) {
			double sum = 0.0;

			/* Figure device values */
			for (j = 0; j < inn; j++) {
				sum += start[nisay].dev[j] = dix[j]/(steps-1.0);
			}

			if (sum <= limit) {		/* Within ink limit */
				double oerr;
				double tot;

				/* Compute Lab */
				mppo->lookup(mppo, start[nisay].Lab, start[nisay].dev);

				/* Compute order error */
				/* Skip first 3 colorants */
				oerr = tot = 0.0;
				for (j = 3; j < inn; j++) {
					int ix = rs.sord[j];		/* Sorted order index */
					double vv = start[nisay].dev[ix];
					double we = (double)j - 2.0;	/* Increasing weight */
			
					if (vv > 0.0001) {
						oerr += tot + we * vv;
					}
					tot += we;
				}
				oerr /= tot;
				start[nisay].oerr = oerr;

				nisay++;
			}

			DC_INC(dix);
		}
printf("~1 start point array done, %d out of %d valid\n",nisay,mxstart);
	}

	/* Search the start array for closest matching point */
	{
		double bde = 1e38;
		int bix = 0;

		for (i = 0; i < nisay; i++) {
			double de;
	
			/* Compute delta E */
			for (de = 0.0, j = 0; j < 3; j++) {
				double tt = rs.Lab[j] - start[i].Lab[j];
				de += tt * tt;
			}
			de += 0.0 * start[i].oerr;
			if (de < bde) {
				bde = de;
				bix = i;
			}
		}
	
printf("Start point at index %d, bde = %f, dev = ",bix,bde);
for (j = 0; j < inn; j++) {
printf("%f",start[bix].dev[j]);
if (j < (inn-1))
printf(" ");
}
printf("\n");

		for (j = 0; j < inn; j++) {
			out[j] = start[bix].dev[j];
			sr[j] = 0.1;
		}
	}

#ifdef NEVER
	out[0] = 0.0;
	out[1] = 0.0;
	out[2] = 0.45;
	out[3] = 0.0;
	out[4] = 0.0;
	out[5] = 0.0;
	out[6] = 0.6;
	out[7] = 1.0;
#endif

#ifdef NEVER
	out[0] = 1.0;
	out[1] = 0.0;
	out[2] = 0.0;
	out[3] = 0.0;
	out[4] = 0.0;
	out[5] = 0.0;
	out[6] = 0.0;
	out[7] = 0.0;
#endif

#ifdef NEVER
	rs.pass = 0;
//	if (powell(&tt, inn, out, sr,  0.001, 5000, revoptfunc, (void *)&rs) != 0)
	if (conjgrad(&tt, inn, out, sr,  0.001, 5000, revoptfunc, NULL, (void *)&rs) != 0)
	{
		error("Powell failed inside mpp_rev()");
	}
printf("\n\n\n\n\n\n#############################################\n");
printf("~1 after first pass got ");
for (j = 0; j < inn; j++) {
printf("%f",out[j]);
if (j < (inn-1))
printf(" ");
}
printf("\n");
printf("#############################################\n\n\n\n\n\n\n\n");
#endif 

#ifndef NEVER
	out[0] = 0.0;
	out[1] = 0.0;
	out[2] = 0.0;
	out[3] = 0.0;
	out[4] = 0.0;
	out[5] = 1.0;
	out[6] = 0.0;
	out[7] = 0.0;
#endif
#ifndef NEVER
	rs.pass = 1;
	if (powell(&tt, inn, out, sr,  0.00001, 5000, revoptfunc, (void *)&rs, NULL, NULL) != 0) {
		error("Powell failed inside mpp_rev()");
	}
#endif

	snap2gamut(&rs, out);
}



















