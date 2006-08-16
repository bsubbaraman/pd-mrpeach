/* sqosc.c Martin Peach 20060613 based on d_osc.c */
/* 20060707 using x-x^3/3 to smooth the ramp */
/* Copyright (c) 1997-1999 Miller Puckette.
* For information on usage and redistribution, and for a DISCLAIMER OF ALL
* WARRANTIES, see the file, "LICENSE.txt," in this distribution.  */

/* sinusoidal oscillator and table lookup; see also tabosc4~ in d_array.c.
*/

#include "m_pd.h"
#include "math.h"
#include <stdio.h> /* for file io */

#define UNITBIT32 1572864.  /* 3*2^19; bit 32 has place value 1 */

    /* machine-dependent definitions.  These ifdefs really
    should have been by CPU type and not by operating system! */
#ifdef IRIX
    /* big-endian.  Most significant byte is at low address in memory */
#define HIOFFSET 0    /* word offset to find MSB */
#define LOWOFFSET 1    /* word offset to find LSB */
#define int32 long  /* a data type that has 32 bits */
#endif /* IRIX */

#ifdef MSW
    /* little-endian; most significant byte is at highest address */
#define HIOFFSET 1
#define LOWOFFSET 0
#define int32 long
#endif

#if defined(__FreeBSD__) || defined(__APPLE__)
#include <machine/endian.h>
#endif

#ifdef __APPLE__
#define __BYTE_ORDER BYTE_ORDER
#define __LITTLE_ENDIAN LITTLE_ENDIAN
#endif                                                                          

#ifdef __linux__
#include <endian.h>
#endif

#if defined(__unix__) || defined(__APPLE__)
#if !defined(__BYTE_ORDER) || !defined(__LITTLE_ENDIAN)                         
#error No byte order defined                                                    
#endif                                                                          

#if __BYTE_ORDER == __LITTLE_ENDIAN                                             
#define HIOFFSET 1                                                              
#define LOWOFFSET 0                                                             
#else                                                                           
#define HIOFFSET 0    /* word offset to find MSB */                             
#define LOWOFFSET 1    /* word offset to find LSB */                            
#endif /* __BYTE_ORDER */                                                       
#include <sys/types.h>
#define int32 int32_t
#endif /* __unix__ or __APPLE__*/

union tabfudge
{
    double tf_d;
    int32 tf_i[2];
};

static t_class *sqosc_class, *scalarsqosc_class;

static float *sqosc_table;
/* COSTABSIZE is 512 in m_pd.h, we start with that... */
#define LOGSQOSCTABSIZE 9
#define SQOSCTABSIZE 512
#define HALFSQOSCTABSIZE 256

typedef struct _sqosc
{
    t_object  x_obj;
    double    x_phase;
    float     x_conv;
    float     x_f; /* frequency if scalar */
    float     x_pw; /* pulse width 0-1, default 0.5 */
    float     x_bw; /* bandwidth */
    float     x_slew; /* slew time in samples */
    double    x_dpw; /* pulse width in this pulse */
    int       x_pulse_ended; /* nonzero if pulse has finished */
//    FILE      *x_logfp;
//    int       x_logcount;
} t_sqosc;

static void sqosc_maketable(void);
void sqosc_tilde_setup(void);
static void sqosc_ft1(t_sqosc *x, t_float f);
static void sqosc_pw(t_sqosc *x, t_float pw);
static void sqosc_dsp(t_sqosc *x, t_signal **sp);
static t_int *sqosc_perform(t_int *w);
static void *sqosc_new(t_floatarg f, t_floatarg pw, t_float bw);

static void sqosc_maketable(void)
{
    int    i;
    float  *fp, phase, phsinc = (2. * 3.14159) / SQOSCTABSIZE; 
    union  tabfudge tf;
    //FILE   *cosfp;

    if (sqosc_table) return;
    //cosfp = fopen("sqosctable.txt", "wb");
    sqosc_table = (float *)getbytes(sizeof(float) * (SQOSCTABSIZE+1));
    for (i = SQOSCTABSIZE + 1, fp = sqosc_table, phase = 0; i--;
        fp++, phase += phsinc)
    {
        *fp = cos(phase);
        //fprintf(cosfp, "%f: %f\n", phase, *fp);
    }
    //fclose(cosfp);
    /* here we check at startup whether the byte alignment
        is as we declared it.  If not, the code has to be
        recompiled the other way. */
    tf.tf_d = UNITBIT32 + 0.5;
    if ((unsigned)tf.tf_i[LOWOFFSET] != 0x80000000)
        bug("cos~: unexpected machine alignment");
}

static void *sqosc_new(t_floatarg f, t_floatarg pw, t_floatarg bw)
{
    t_sqosc *x = (t_sqosc *)pd_new(sqosc_class);

    x->x_f = f; /* the initial frequency in Hz */
    outlet_new(&x->x_obj, gensym("signal"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("ft1"));
    inlet_new(&x->x_obj, &x->x_obj.ob_pd, &s_float, gensym("pw"));
    post("sqosc_new frequency %f, pulsewidth %f bandwidth %f", f, pw, bw);
    x->x_phase = 0;
    x->x_conv = 0;
    if ((pw <= 0)||(pw >= 1))
    {
        post("sqosc: second argument (pulse width) must be greater than 0 and less than 1, using 0.5");
        x->x_pw = 0.5 * SQOSCTABSIZE;
    }
    else x->x_pw = pw * SQOSCTABSIZE;
    if (bw < 0)
    {
        post("sqosc: third argument (bandwidth) must be greater than 0, using 10000");
        x->x_bw = 10000;
    }
    else x->x_bw = bw;
    x->x_slew = HALFSQOSCTABSIZE/x->x_bw;/* slew = time of half bandwidth cycle */
    x->x_dpw = x->x_pw; /* pulse width in this pulse */
    x->x_pulse_ended = 1; /* nonzero if pulse has finished */
//    x->x_logfp = fopen("sqosclog.txt", "wb");
//    x->x_logcount = 0;
    return (x);
}

/*
This is corrected from Chun Lee's explanation in an email at:
http://music.columbia.edu/pipermail/music-dsp/2004-November/028814.html

1-> a double variable UNITBIT32 is assigned to 1572864. This is what it
looks like in memory in a little endian machine:
byte 7   byte 6   byte 5   byte 4   byte 3   byte 2   byte 1    byte 0
00000000 00000000 00000000 00000000 00000000 00000000 0001 1100 1000001 0
|<------fraction------------------>|<----1572864--------->|exponent    |sign|

The reason for 1572864 (1.5 X 2^20) is that its representation in IEEE754 format (double)
has 51 zeros to the right of a placeholding 1 bit, which can then be used as
a wide fixed-point register with the binary point at bit 32, so the lower 32 bits
can be used as a fractional accumulator with 32-bit precision.
Adding 1 to INITBIT32 adds a 1 at bit 32, with no change in exponent,
so the lower 32 bits act as a fraction of one. The 31 zero bits in the integer part
can be set as high as 2147483647 (31 1s) without affecting the precision.

The upper 32 bits are 1094189056, or 01000001 00111000 00000000 00000000 in binary.
The sign bit is zero, or positive. The upper bit of the exponent is one, meaning
the number is normalized, or full precision.The exponent is 1043 minus the bias
of 1023, or 20.

This is the representation of the number 1.5 X 2^20 = 1572864:

IEEE Standard 754 Double Precision Storage Format (double):
63        62            52 51               32 31 0
+--------+----------------+-------------------+------------------------------+
| s 1bit | e[62:52] 11bit | f[51:32] 20bit    | f[31:0] 32bit                |
+--------+----------------+-------------------+------------------------------+
B0-------------->B1---------->B2----->B3----->B4----->B5----->B6----->B7----->
0         10000010011     1000000000000000000000000000000000000000000000000000

2-> there is the tabfudge like:

Union tabfudge
{
 double tf_d;
 int32 tf_i[2]; 
}

In dsp_perfrom:

Union tabfudge tf;

tf.tf_d = UNITBIT32;

3-> the phase is accumulated into tf_d plus the UNITBIT32

Double dphase = x_phase + UNITBIT32;

4-> byte 0 to byte 3 is stored as a separate variable as:

int normhipart = tf.tf_I[HIOFFSET]; //where HIOFFSET is used to find MSB

5-> in the actual dsp block, although the phase is accumulated into
tf.tf_d, bytes 0 to 3 of tf.tf_d are constantly forced to be a
constant by:

tf.tf_i[HIOFFSET] = normhipart;

6-> because of this, to get the fraction part out of the tf.tf_d, simply do:

tf.tf_d - UNITBIT32;

7-> mission accomplished!!!!!

*/

static t_int *sqosc_perform(t_int *w)
{
    t_sqosc         *x = (t_sqosc *)(w[1]);
    t_float         *in = (t_float *)(w[2]);
    t_float         *out = (t_float *)(w[3]);
    t_float         sample;
    int             n = (int)(w[4]);
    float           *tab = sqosc_table, *addr, f1, f2, frac;
    int             index;  
    double          dphase = x->x_phase + UNITBIT32;
    int             normhipart;
    union           tabfudge tf;
    float           conv = x->x_conv;
    double          lastin, findex, slewindex;
    static double   twothirds = 2.0/3.0;
    static double   onethird = 1.0/3.0;

    tf.tf_d = UNITBIT32; /* set the phase accumulator to (1.5 X 2^20) making it effectively fixed-point */
    normhipart = tf.tf_i[HIOFFSET]; /* save the sign, exponent, and integer part of a fixed accumulator */
    tf.tf_d = dphase; /* the current phase plus the "frame" */
    lastin = *in++; /* latest frequency */
    if (lastin < 0) lastin = -lastin;/* negative frequency is the same as positive here */
    if (lastin > x->x_bw) lastin = x->x_bw;// limit frequency to bandwidth
    slewindex = x->x_slew*lastin;
    dphase += lastin * conv; /* new phase is old phase + (frequency * table period) */
    //addr = tab + (tf.tf_i[HIOFFSET] & (SQOSCTABSIZE-1)); /* point to the current sample in the table */
    index = tf.tf_i[HIOFFSET] & (SQOSCTABSIZE-1);
    tf.tf_i[HIOFFSET] = normhipart; /* zero the non-fractional part of the phase */
    frac = tf.tf_d - UNITBIT32; /* extract the fractional part of the phase  */
    while (--n)
    {
        tf.tf_d = dphase;
        //f1 = addr[0]; /* first sample */
        if (index <= slewindex)
        { /* rising phase */
            if(x->x_pulse_ended)
            {/* set pw for this pulse once only*/
                if(x->x_pw < slewindex)x->x_dpw = slewindex;
                else if (x->x_pw > SQOSCTABSIZE-slewindex)x->x_dpw = SQOSCTABSIZE-slewindex;
                else x->x_dpw = x->x_pw;
                x->x_pulse_ended = 0;
            }
            //findex = (index/(x->x_slew*lastin))*HALFSQOSCTABSIZE;
            //addr = tab + HALFSQOSCTABSIZE + (int)findex; 
            f1 = 1.0-2.0*(slewindex-index)/slewindex;// a ramp from -1 to +1 // addr[0];
            f1 = f1 - pow(f1, 3.0)*onethird;// smooth the ramp
//            if (x->x_logcount < 1000)
//            {
//                fprintf(x->x_logfp, "rise index %d slewindex %f f1 %f frac %f\n", index, slewindex, f1, frac);
//                ++x->x_logcount;
//                if (x->x_logcount >= 1000) fclose(x->x_logfp);
//            }
        }
        else if (index < x->x_dpw) f1 = twothirds; /* risen */
        else if (index <= slewindex+x->x_dpw)
        { /* falling phase */
//            findex = ((index-HALFSQOSCTABSIZE)/(x->x_slew*lastin))*HALFSQOSCTABSIZE;
//            addr = tab + (int)findex; 
            f1 = -1.0+2.0*(slewindex-index+x->x_dpw)/slewindex;// a ramp from +1 to -1 // addr[0];
            f1 = f1 - pow(f1, 3.0)*onethird;// smooth the ramp
            x->x_pulse_ended = 1;
//            if (x->x_logcount < 1000)
//            {
//                fprintf(x->x_logfp, "fall index %d slewindex %f f1 %f frac %f\n", index, slewindex, f1, frac);
//                ++x->x_logcount;
//                if (x->x_logcount >= 1000) fclose(x->x_logfp);
//            }
        }
        else
        { /* fallen */
            f1 = -twothirds;
        }
        lastin = *in++;
        if (lastin < 0) lastin = -lastin;/* negative frequency is the same as positive here */
        if (lastin > x->x_bw) lastin = x->x_bw;// limit frequency to bandwidth
        slewindex = x->x_slew*lastin;
        dphase += lastin * conv; /* next phase */
        //f2 = addr[1]; /* second sample */
        if (index+1 <= slewindex)
        {
            f2 = 1.0-2.0*(slewindex-index-1)/slewindex;// addr[1];
            f2 = f2 - pow(f2, 3.0)*onethird;
//            if (x->x_logcount < 1000)
//            {
//                fprintf(x->x_logfp, "rise index %d slewindex %f f2 %f frac %f\n", index+1, slewindex, f2, frac);
//                ++x->x_logcount;
//                if (x->x_logcount >= 1000) fclose(x->x_logfp);
//            }
        }
        else if (index+1 < x->x_dpw) f2 = twothirds;
        else if (index+1 <= slewindex+x->x_dpw)
        {
            f2 = -1.0+2.0*(slewindex-index-1+x->x_dpw)/slewindex;// addr[1];
            f2 = f2 - pow(f2, 3.0)*onethird;
//            if (x->x_logcount < 1000)
//            {
//                fprintf(x->x_logfp, "fall index %d slewindex %f f2 %f frac %f\n", index+1, slewindex, f2, frac);
//                ++x->x_logcount;
//                if (x->x_logcount >= 1000) fclose(x->x_logfp);
//            }
        }
        else f2 = -twothirds;

        sample = f1 + frac * (f2 - f1); /* output first sample plus fraction of second sample (linear interpolation) */
        *out++ = sample;
//        if (x->x_logcount < 1000)
//        {
//            fprintf(x->x_logfp, "index %ld f1 %f f2 %f frac %f out %f\n", index, f1, f2, frac, sample);
//            ++x->x_logcount;
//            if (x->x_logcount >= 1000) fclose(x->x_logfp);
//        }
        //addr = tab + (tf.tf_i[HIOFFSET] & (SQOSCTABSIZE-1)); /* point to the next sample */
        index = tf.tf_i[HIOFFSET] & (SQOSCTABSIZE-1);
        tf.tf_i[HIOFFSET] = normhipart; /* zero the non-fractional part */

        frac = tf.tf_d - UNITBIT32; /* get next fractional part */
    }
    //f1 = addr[0];
    if (index <= slewindex)
    {
        if(x->x_pulse_ended)
        {/* set pw for this pulse once only*/
            if(x->x_pw < slewindex)x->x_dpw = slewindex;
            else if (x->x_pw > SQOSCTABSIZE-slewindex)x->x_dpw = SQOSCTABSIZE-slewindex;
            else x->x_dpw = x->x_pw;
            x->x_pulse_ended = 0;
        }
        //findex = (index/(x->x_slew*lastin))*HALFSQOSCTABSIZE;
        //addr = tab + HALFSQOSCTABSIZE + (int)findex; 
        f1 = 1.0-2.0*(slewindex-index)/slewindex;// addr[0];
        f1 = f1 - pow(f1, 3.0)*onethird;
//        if (x->x_logcount < 1000)
//        {
//            fprintf(x->x_logfp, "rise2 index %d slewindex %f f1 %f frac %f\n", index, slewindex, f1, frac);
//            ++x->x_logcount;
//            if (x->x_logcount >= 1000) fclose(x->x_logfp);
//        }
    }
    else if (index < x->x_dpw) f1 = twothirds; /* risen */
    else if (index <= slewindex+x->x_dpw)
    { /* falling phase */
//        findex = ((index-HALFSQOSCTABSIZE)/(x->x_slew*lastin))*HALFSQOSCTABSIZE;
//        addr = tab + (int)findex; 
        f1 = -1.0+2.0*(slewindex-index+x->x_dpw)/slewindex;// addr[0];
        f1 = f1 - pow(f1, 3.0)*onethird;
        x->x_pulse_ended = 1;
//        if (x->x_logcount < 1000)
//        {
//            fprintf(x->x_logfp, "fall2 index %d slewindex %f f1 %f frac %f\n", index, slewindex, f1, frac);
//            ++x->x_logcount;
///            if (x->x_logcount >= 1000) fclose(x->x_logfp);
//        }
    }
    else
    { /* fallen */
        f1 = -twothirds;
    }
    //f2 = addr[1]; /* second sample */
    if (index+1 <= slewindex)
    {
        f2 = 1.0-2.0*(slewindex-index-1)/slewindex;// addr[1];
        f2 = f2 - pow(f2, 3.0)*onethird;
//        if (x->x_logcount < 1000)
//        {
//            fprintf(x->x_logfp, "rise2 index %d slewindex %f f2 %f frac %f\n", index+1, slewindex, f2, frac);
//            ++x->x_logcount;
//            if (x->x_logcount >= 1000) fclose(x->x_logfp);
//        }
    }
    else if (index+1 < x->x_dpw) f2 = twothirds;
    else if (index+1 <= slewindex+x->x_dpw)
    {
        f2 = -1.0+2.0*(slewindex-index-1+x->x_dpw)/slewindex;// addr[1];
        f2 = f2 - pow(f2, 3.0)*onethird;
//        if (x->x_logcount < 1000)
//        {
//            fprintf(x->x_logfp, "fall2 index %d slewindex %f f2 %f frac %f\n", index+1, slewindex, f2, frac);
//            ++x->x_logcount;
//            if (x->x_logcount >= 1000) fclose(x->x_logfp);
//        }
    }
    else f2 = -twothirds;
    sample = f1 + frac * (f2 - f1); /* the final sample */
    *out++ = sample;
//    if (x->x_logcount < 1000)
//    {
//        fprintf(x->x_logfp, "*index %ld f1 %f f2 %f frac %f out %f\n", index, f1, f2, frac, sample);
//        ++x->x_logcount;
//        if (x->x_logcount >= 1000) fclose(x->x_logfp);
//    }

    tf.tf_d = UNITBIT32 * SQOSCTABSIZE; /* this just changes the exponent if the table size is a power of 2 */
    normhipart = tf.tf_i[HIOFFSET]; /* ...so we get more integer digits but fewer fractional ones */
    tf.tf_d = dphase + (UNITBIT32 * SQOSCTABSIZE - UNITBIT32); /* subtract one UNITBIT32 we added at the beginning */
    tf.tf_i[HIOFFSET] = normhipart; /* wrap any overflow to the table size */
    x->x_phase = tf.tf_d - UNITBIT32 * SQOSCTABSIZE; /* extract just the phase */
    return (w+5);
}
static void sqosc_dsp(t_sqosc *x, t_signal **sp)
{
//    static int once = 0;

    x->x_conv = SQOSCTABSIZE/sp[0]->s_sr;
/* conv = table period = (samples/cycle)/(samples/sec) = sec/cycle = 0.011610sec for 512/44100 */

//    if (once == 0)
//    {
//        ++once;
//        post ("x->x_slew = %f, x->x_bw = %f, sp[0]->s_sr = %f", x->x_slew, x->x_bw, sp[0]->s_sr);
//    }
    dsp_add(sqosc_perform, 4, x, sp[0]->s_vec, sp[1]->s_vec, sp[0]->s_n);
}

static void sqosc_ft1(t_sqosc *x, t_float f)
{
    x->x_phase = SQOSCTABSIZE * f;
}

static void sqosc_pw(t_sqosc *x, t_float pw)
{
    if ((pw <= 0)||(pw >= 1)) return;
        //post("sqosc: pulse width must be greater than 0 and less than 1");// this is an annoying message...
    x->x_pw = pw * SQOSCTABSIZE;
}

void sqosc_tilde_setup(void)
{    
    sqosc_class = class_new(gensym("sqosc~"), (t_newmethod)sqosc_new, 0,
        sizeof(t_sqosc), 0, A_DEFFLOAT, A_DEFFLOAT, A_DEFFLOAT, 0);
    CLASS_MAINSIGNALIN(sqosc_class, t_sqosc, x_f);/* x_f is used when no signal is input */
    class_addmethod(sqosc_class, (t_method)sqosc_dsp, gensym("dsp"), 0);
    class_addmethod(sqosc_class, (t_method)sqosc_ft1, gensym("ft1"), A_FLOAT, 0);
    class_addmethod(sqosc_class, (t_method)sqosc_pw, gensym("pw"), A_FLOAT, 0);

    sqosc_maketable(); /* make the same table as cos_table */
}


/* end of sqosc.c */
