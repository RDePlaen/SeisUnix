/* Copyright (c) Colorado School of Mines, 1998.*/
/* All rights reserved.                       */

/* SUTAUP: $Revision: 1.9 $ ; $Date: 1998/03/23 17:38:30 $	*/

#include "su.h"
#include "segy.h"
#include "header.h"
#include <signal.h>
#include "taup.h"
#include "math.h"

/*********************** self documentation **********************/
char *sdoc[] = {
"                                                                       ",
" suslstk - forward and inverse local slant stack for dip filtering	",
"                                                                       ",
"    suslstk <infile >outfile  [optional parameters]                 	",
"                                                                       ",
" Optional Parameters:                                                  ",
"				                                	",
" dt=tr.dt (from header) 	time sampling interval (secs)           ",
" nx=ntr   (counted from data)	number of horizontal samples (traces)	",
" npoints=71			number of points for rho filter		",
" pminf=-0.01			start of moveout at last trace s        ",
" pmaxf=0.01			end of moveout at last trace   s        ",
" np=25			        number of slopes for Tau-P transform	",
" nwin=5		        number of traces in spatial window	",
" fh1=100			high end frequency before taper         ",
" fh2=120			high end frequency                      ",
" prw=0.01                      prewithening                            ",
" w=0			=1	apply semblance weights			",
" s=1			=1	apply smoothing weights			",
"                               1 Gaussian smoothing                    ",
"                               2 DLSQ smoothing                        ",
" sl1=3                     	smoothing coeff in first direction      ",
" sl2=np/2                      smoothing coeff in spatial direction    ",
" smbwin=0.05		        semblance window in seconds		",
" pw = 1.0		        semlance weights raised to this power   ",
"                               before application                      ",
"                                                                       ",
" verbose=0	verbose = 1 echoes information			",
"								",
" tmpdir= 	 if non-empty, use the value as a directory path",
"		 prefix for storing temporary files; else if the",
"	         the CWP_TMPDIR environment variable is set use	",
"	         its value for the path; else use tmpfile()	",
NULL};


static void closefiles(void);
void semb(int sn, float **data, int nx, int nt, float *smb);
void semb_fast(int sni, float **data, int nx, int nti, float *smb);
void c_window(int ntr,int nw,int no,float w[],float ww[],int *rnp,int ws[],int we[]);

/* Globals (so can trap signal) defining temporary disk files */
char tracefile[BUFSIZ];	/* filename for the file of traces	*/
char headerfile[BUFSIZ];/* filename for the file of headers	*/
FILE *tracefp;		/* fp for trace storage file		*/
FILE *headerfp;		/* fp for header storage file		*/


segy tr;

int
main(int argc, char **argv)
{
	int ix,it;		/* loop counters */
	int j,k;
	int ntr;		/* number of input traces */
	int nt;			/* number of time samples */
	float dt;               /* Time sample interval */
        float dx=1;               /* horizontal sample interval */
        float pminf;             /* Minimum slope for Tau-P transform */
        float pmaxf;             /* Maximum slope for Tau-P transform */
	float dpf;		/* slope sampling interval */
	int np;			/* number of slopes for slant stack */
	int nwin;		/* spatial window length */
	int npoints;		/* number of points for rho filter */
	float **twin;		/* array[nwin][nt] of window traces */	
	float **pwin;		/* array[np][nt] of sl traces */
				/* full multiple of nwin */	
	int ntfft;
	float **traces;
	int w;			/* flag to apply semblance weights */
	int s;			/* flag to apply smoothing weights */
	int sl1;		/* length of smoothing window */
	int sl2;		/* length of smoothing window */
	float *smb;		/* semblance weights */
	double pw;
	float smbwin;
	int sn;
	float **out_traces;	/* array[nx][nt] of output traces */	
	int verbose;		/* flag for echoing information */
	char *tmpdir;		/* directory path for tmp files */
	cwp_Bool istmpdir=cwp_false;/* true for user-given path */
	float fh1;		/* maximum frequency before taper */
	float fh2;		/* maximum frequency */
	float prw;		/* prewithening */
	int cwin;		/* counter of the current window */

	float *wg;		/* window weigth array */
	float *ww;		/* window weigth array */
	int *ws;		/* spatial window start values */
	int *we;		/* spatial window end values */ 
	int rnp;		/* number of passes on the = number of windows */
	int no;			/* window overlap length */
	
	
        /* hook up getpar to handle the parameters */
        initargs(argc,argv);
        requestdoc(1);

	if (!getparint("verbose", &verbose))	verbose = 0;

	/* Look for user-supplied tmpdir */
	if (!getparstring("tmpdir",&tmpdir) &&
	    !(tmpdir = getenv("CWP_TMPDIR"))) tmpdir="";
	if (!STREQ(tmpdir, "") && access(tmpdir, WRITE_OK))
		err("you can't write in %s (or it doesn't exist)", tmpdir);

        /* get info from first trace */
        if (!gettr(&tr))  err("can't get first trace");
        nt = tr.ns;
        dt = (float) tr.dt/1000000.0;

        /* Store traces in tmpfile while getting a count */
	if (STREQ(tmpdir,"")) {
		tracefp = etmpfile();
		headerfp = etmpfile();
		if (verbose) warn("using tmpfile() call");
	} else { /* user-supplied tmpdir */
		char directory[BUFSIZ];
		strcpy(directory, tmpdir);
		strcpy(tracefile, temporary_filename(directory));
		strcpy(headerfile, temporary_filename(directory));
		/* Trap signals so can remove temp files */
		signal(SIGINT,  (void (*) (int)) closefiles);
		signal(SIGQUIT, (void (*) (int)) closefiles);
		signal(SIGHUP,  (void (*) (int)) closefiles);
		signal(SIGTERM, (void (*) (int)) closefiles);
		tracefp = efopen(tracefile, "w+");
		headerfp = efopen(headerfile, "w+");
      		istmpdir=cwp_true;		
		if (verbose) warn("putting temporary files in %s", directory);
	}
        ntr = 0;
        do {
                ++ntr;
                efwrite(&tr, 1, HDRBYTES, headerfp);
                efwrite(tr.data, FSIZE, nt, tracefp);
        } while (gettr(&tr));

        /* get general flags and parameters and set defaults */
        if (!getparint("np",&np))             	np = 25;
        if (!getparfloat("pminf",&pminf))	pminf = -0.01;
        if (!getparfloat("pmaxf",&pmaxf))	pmaxf = 0.01;
        if (!getparfloat("fh1",&fh1))		fh1 = 100;
        if (!getparfloat("fh2",&fh2))		fh2 = 120;
        if (!getparfloat("prw",&prw))		prw = 0.01;
	if (!getparfloat("dx",&dx))		dx = 1.0;
	if (!getparint("npoints",&npoints))	npoints = 71;
	if (!getparint("nwin",&nwin))		nwin= 5;
	if (!getparfloat("dt",&dt))		dt = dt;
	if (!getparfloat("smbwin",&smbwin))	smbwin = 0.05;
	if (!getpardouble("pw",&pw))		pw = 1.0;
	if (!getparint("w",&w))			w = 0;
	if (!getparint("s",&s))			s = 0;
	if (!getparint("sl1",&sl1))		sl1 = 3;
	if (!getparint("sl2",&sl2))		sl2 = np/2;

	if (dt == 0.0)
		err("header field dt not set, must be getparred");
	
	if(nwin%2==0) nwin++;

	/* allocate space */
	ntfft=npfar(nt);
	twin = alloc2float(nt, nwin);
	pwin = ealloc2float(ntfft,np);
        traces = alloc2float(nt, ntr);
        out_traces = alloc2float(nt, ntr);
	smb = ealloc1float(nt);
	
	/* Set up some constans*/
	dpf=(pmaxf-pminf)/(np-1);
	sn = NINT(smbwin/dt);
	if(sn%2==0) sn++;
	
	/* load traces into an array and close temp file */
	erewind(headerfp);
        erewind(tracefp);
	
	memset( (void *) traces[0], (int) '\0', (nt*ntr)*FSIZE);
	memset( (void *) out_traces[0], (int) '\0', (nt*ntr)*FSIZE);
	
        for (ix=0; ix<ntr; ix++)
                fread (traces[ix], FSIZE, nt, tracefp);
        efclose (tracefp);
	if (istmpdir) eremove(tracefile);

	
	/* Desing spatial windows */
	/* spatial window weights, window start and end points */
	/* 50 % overlap between windows */
	no=NINT((float)nwin*50.0/100.0);
	wg=ealloc1float(ntr);
	ww=ealloc1float(nwin);
	ws=ealloc1int(ntr);
	we=ealloc1int(ntr);
	
	if(ntr>= nwin) {
		c_window(ntr,nwin,no,wg,ww,&rnp,ws,we);
	
		/* Initialize window counter */
		{ int itr,it;
		for(cwin=0; cwin<rnp; cwin++) {
			for(itr=ws[cwin];itr<=we[cwin];itr++) 
				memcpy( (void *) twin[itr-ws[cwin]] ,
				        (const void *) traces[itr], nt*FSIZE);

			/* compute forward slant stack */
			fwd_tx_sstack (dt, nt, nwin, -nwin/2*dx, dx, np, pminf, dpf,
	       			twin, pwin);
/*			forward_p_transform(nwin,nt,dt,pmaxf*1000.0,pminf*1000.0,dpf*1000.0,
                                    0.0,fh1,fh2,3.0,30.0,400,5,1,0,0,1,prw,
				    0.0,nwin*dx,3,dx,0.0,0.0,0.0,twin,pwin);
*//*			fwd_FK_sstack (dt, nt, nwin, -nwin/2*dx, dx, np, pminf, dpf,0,
	       			twin, pwin);
*/		
			/* compute semplance */
			if(w==1) {
				semb(sn,pwin,np,nt,smb);
				/* apply weights */
				for(j=0;j<nt;j++)
					for(k=0;k<np;k++) pwin[k][j] *=smb[j];
			}
			if(s==1) {
				gaussian2d_smoothing (np,nt,sl2,sl1,pwin);
			}
			if(s==2) {
				dlsq_smoothing (nt,np,0,nt,0,np,sl1,sl2,0,pwin);
			}
			/* compute inverse slant stack */
			inv_tx_sstack (dt, nt, nwin, npoints,-nwin/2*dx, dx, np,pminf,dpf,
				pwin, twin);
/*			inverse_p_transform(nwin,nt,dt,pmaxf*1000.0,pminf*1000.0,dpf*1000.0,
                                    0.0,fh1,fh2,0.0,nwin*dx,3,dx,0.0,
				    pwin,twin);
*//*			inv_FK_sstack (dt, nt, nwin,-nwin/2*dx, dx, np,pminf,dpf,0,
				pwin, twin);
*/			

			for(itr=ws[cwin];itr<=we[cwin];itr++) 
				for(it=0;it<nt;it++) 
					out_traces[itr][it]+=twin[itr-ws[cwin]][it]*ww[itr-ws[cwin]];
		}
		}
/*		fprintf(stderr," Trace #= %5d\n",i); */	
        }
	/* write output traces + add final weights*/
        erewind(headerfp);
	{       register int itr;
		for (itr=0; itr<ntr; itr++) {
			efread(&tr, 1, HDRBYTES, headerfp);
			for (it=0; it<nt; it++) 
				tr.data[it]=out_traces[itr][it]*(1.0/wg[itr]);
			puttr(&tr);
		}
	}
	efclose(headerfp);
	if (istmpdir) eremove(headerfile);

	/* free allocated space */
	free2float(out_traces);
	free1float(wg);
	free1float(ww);
	free1int(ws);
	free1int(we);
	
	return EXIT_SUCCESS;

}

/* for graceful interrupt termination */
static void closefiles(void)
{
	efclose(headerfp);
	efclose(tracefp);
	eremove(headerfile);
	eremove(tracefile);
	exit(EXIT_FAILURE);
}

void semb(int sni, float **data, int nx, int nt, float *smb)
{
     	float nsum;
     	float dsum;
     	float pwr=0.1;
     
     
     /* compute semblance quotients */
/*     for (itout=0; itout<ntout; ++itout) {
     	     it = itout*dtratio;
     	     ismin = it-nsmooth/2;
     	     ismax = it+nsmooth/2;
     	     if (ismin<0) ismin = 0;
     	     if (ismax>nt-1) ismax = nt-1;
     	     nsum = dsum = 0.0;
*/
	{ register int ix,is;
		for (is=0; is<nt; ++is) {
     	    		nsum=0.0;
			dsum=0.0;
		     	for (ix=0; ix<nx; ++ix) {
     			     nsum += data[ix][is];
     		 	     dsum += data[ix][is]*
     			    	     data[ix][is];
			}
     	    	 	dsum*=nx;
			nsum=nsum*nsum;
			
    		     	smb[is] = (dsum!=0.0?nsum/dsum:0.0);
     	 		smb[is] = pow (smb[is], pwr);
     		}
	}
	gaussian1d_smoothing (nt,nt/20,smb);
}

void c_window(int ntr,int nw,int no,float w[],float ww[],int *rnp,int ws[],int we[])
/* Given ntr numbers, nw window length, no overlap
  the routine optimally select the nw window traces out of
  ntr number with no overlap.
  The routine returns the number of windows in rnp
  rnp window start and end values in ws and we, respectively.
  The ntr trace weights are returned in w.
  w has to be declared outside to the size of ntr.
  ws and we have to be at least npr in size 
  The window weights are returned in ww. ww has to be declared
  outside to nw size.  
*/
{
static int iminarg1,iminarg2;
#define IWMIN(a,b) (iminarg1=(a),iminarg2=(b),(iminarg1) < (iminarg2) ?\
        (iminarg1) : (iminarg2))

	int np;		/* number of passes */
	int sw,ew;      /* start and end of spatial window */
	float dw;
	
	memset( (void *) w, (int) '\0', ntr*sizeof(float));
	memset( (void *) ww, (int) '\0', nw*sizeof(float));
		
	{ register int i;		
		ew=nw-1;
		sw=0;
		np=1;
		for(i=sw;i<=ew;i++) ww[i]=(float)nw;
		ww[0]=1.0; ww[nw-1]=1.0;
		dw=(float)(nw-1)/(float)(no-1);
		for(i=1;i<no;i++) ww[i]=1.0+dw*i;
		for(i=1;i<no;i++) ww[nw-1-i]=ww[i];
		for(i=sw;i<=ew;i++) w[i]+=ww[i-sw];
				
		ws[np-1]=sw; we[np-1]=ew;
		while(ew<ntr-1) {
			ew+=nw-no;
			ew=IWMIN(ew,ntr-1);
			sw=ew-nw+1;
			np++;
			for(i=sw;i<=ew;i++) w[i]+=ww[i-sw];
			ws[np-1]=sw; we[np-1]=ew;
		}
	}
	*rnp=np;
}

