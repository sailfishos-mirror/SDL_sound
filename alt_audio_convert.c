/*
 *  Extended Audio Converter for SDL (Simple DirectMedia Layer)
 *  Copyright (C) 2002  Frank Ranostaj
 *                      Institute of Applied Physik
 *                      Johann Wolfgang Goethe-Universit�t
 *                      Frankfurt am Main, Germany
 *
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Library General Public
 *   License as published by the Free Software Foundation; either
 *   version 2 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Library General Public License for more details.
 *
 *  You should have received a copy of the GNU Library General Public
 *  License along with this library; if not, write to the Free
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 *  Frank Ranostaj
 *  ranostaj@stud.uni-frankfurt.de
 *
 * (This code blatantly abducted for SDL_sound. Thanks, Frank! --ryan.)
 */

#include "alt_audio_convert.h"
#include <math.h>

/* just to make sure this is defined... */

#ifndef min
#define min(x, y) ( ((x) < (y)) ? (x) : (y) )
#endif

#ifndef max
#define max(x, y) ( ((x) > (y)) ? (x) : (y) )
#endif

#ifndef abs
#define abs(x) ( ((x) > (0)) ? (x) : -(x) )
#endif


/* some macros for "parsing" format */

#define IS_8BIT(x)    ((x).format & 0x0008)
#define IS_16BIT(x)   ((x).format & 0x0010)
#define IS_FLOAT(x)   ((x).format & 0x0020)
#define IS_SIGNED(x)  ((x).format & 0x8000)
#define IS_SYSENDIAN(x) ((~AUDIO_U16SYS ^ (x).format) & 0x1000)


/*-------------------------------------------------------------------------*/
/* the purpose of the RateConverterBuffer is to provide a continous storage
   for head and tail of the (sample)-buffer. This allows a simple and
   perfomant implemantation of the sample rate converters. Depending of the
   operation mode, two layouts for the RateConverterBuffer.inbuffer are
   possible:

   in the Loop Mode:
   ... T-4 T-3 T-2 T-1 H+0 H+1 H+2 H+3 H+4 ...
                       |
                       linp, finp

   in the Single Mode (non Loop):
   ... T-4 T-3 T-2 T-1 0   0   0 ... 0   0   0   H+0 H+1 H+2 H+3 H+4 ...
                       |                          |
                       linp                       finp

   The RateConverterBuffer allows an accurate attack and decay of the
   filters in the rate Converters.

   The pointer finp are actually shifted against the depicted position so
   that on the first invocation of the rate converter the input of the
   filter is nearly complete in the zero region, only one input value is
   used. After the calculation of the first output value, the pointer are
   incremented or decremented depending on down or up conversion and the
   first two input value are taken into account. This procedure repeats
   until the filter has processed all zeroes. The distance of the pointer
   movement is stored in flength, always positive.

   Further a pointer cinp to the sample buffer itself is stored. The pointer
   to the sample buffer is shifted too, so that on the first use of this
   pointer the filter is complete in the sample buffer. The pointer moves
   over the sample buffer until it reaches the other end. The distance of
   the movement is stored in clength.

   Finally the decay of the filter is done by linp and llength like finp,
   flength, but in reverse order.

   buffer denotes the start or the end of the output buffer, depending
   on direction of the rate conversion.

   All pointer and length referring the buffer as Sint16. All length
   are refering to the input buffer */

typedef struct
{
    Sint16 inbuffer[24*_fsize];
    Sint16 *finp, *cinp, *linp;
    Sint16 *buffer;
    int flength, clength, llength;
} RateConverterBuffer;


/* Mono (1 channel ) */
#define Suffix(x) x##1
#include "filter_templates.h"
#undef Suffix

/* Stereo (2 channel ) */
#define Suffix(x) x##2
#include "filter_templates.h"
#undef Suffix


/*-------------------------------------------------------------------------*/
static int ConvertAudio( Sound_AudioCVT *Data,
                         Uint8* buffer, int length, int mode )
{
    AdapterC Temp;
    int i;

    /* Make sure there's a converter */
    if( Data == NULL ) {
        SDL_SetError("No converter given");
        return(-1);
    }

    /* Make sure there's data to convert */
    if( buffer == NULL ) {
        SDL_SetError("No buffer allocated for conversion");
        return(-1);
    }

    /* Set up the conversion and go! */
    Temp.buffer = buffer;
    Temp.mode = mode;
    Temp.filter = &Data->filter;

    for( i = 0; Data->adapter[i] != NULL; i++ )
	length = (*Data->adapter[i])( Temp, length);

    return length;
}

int Sound_ConvertAudio( Sound_AudioCVT *Data )
{
    int length;
    /* !!! FIXME: Try the looping stuff under certain circumstances? --ryan. */
    length = ConvertAudio( Data, Data->buf, Data->len, 12 );
    Data->len_cvt = length;
    return length;
}


/*-------------------------------------------------------------------------*/
static int expand8BitTo16BitSys( AdapterC Data, int length )
{
    int i;
    Uint8* inp = Data.buffer;
    Uint16* buffer = (Uint16*)Data.buffer;
    for( i = length; i--; )
         buffer[i] = inp[i]<<8;
    return 2*length;
}

static int expand8BitTo16BitWrong( AdapterC Data, int length )
{
    int i;
    Uint8* inp = Data.buffer;
    Uint16* buffer = (Uint16*)Data.buffer;
    for( i = length; i--; )
         buffer[i] = inp[i];
    return 2*length;
}

/*-------------------------------------------------------------------------*/
static int expand16BitToFloat( AdapterC Data, int length )
{
    int i;
    Sint16* inp = (Sint16*)Data.buffer;
    float* buffer = (float*)Data.buffer;
    for( i = length>>1; i--; )
         buffer[i] = inp[i]*(1./32767);
    return 2*length;
}

/*-------------------------------------------------------------------------*/
static int swapBytes( AdapterC Data, int length )
{
   /*
    * !!! FIXME !!!
    *
    *
    * Use the faster SDL-Macros to swap
    * - Frank
    */

    int i;
    Uint16 a,b;
    Uint16* buffer = (Uint16*) Data.buffer;
    for( i = length>>1; i --; )
    {
         a = b = buffer[i];
         buffer[i] = ( a << 8 ) | ( b >> 8 );
    }
    return length;
}

/*-------------------------------------------------------------------------*/
static int cutFloatTo16Bit( AdapterC Data, int length )
{
    int i;
    float* inp = (float*) Data.buffer;
    Sint16* buffer = (Sint16*) Data.buffer;
    length>>=2;
    for( i = 0; i < length; i++ )
    {
         if( inp[i] > 1. )
             buffer[i] = 32767;
         else if( inp[i] < -1. )
             buffer[i] = -32768;
         else
             buffer[i] = 32767 * inp[i];
    }
    return 2*length;
}

/*-------------------------------------------------------------------------*/
static int cut16BitSysTo8Bit( AdapterC Data, int length )
{
    int i;
    Uint16* inp = (Uint16*) Data.buffer;
    Uint8* buffer = Data.buffer;
    length >>= 1;
    for( i = 0; i < length; i++ )
         buffer[i] = inp[i]>>8;
    return length;
}

static int cut16BitWrongTo8Bit( AdapterC Data, int length )
{
    int i;
    Uint16* inp = (Uint16*) Data.buffer;
    Uint8* buffer = Data.buffer;
    length >>= 1;
    for( i = 0; i < length; i++ )
         buffer[i] = inp[i] & 0xff;
    return length;
}

/*-------------------------------------------------------------------------*/
/* poor mans mmx :-)                                                       */
static int changeSigned( AdapterC Data, int length, Uint32 XOR )
{
    int i;
    Uint32* buffer = (Uint32*) Data.buffer;
    for( i = length>>2; i--;  )
         buffer[i] ^= XOR;
    for( i = 4*(length>>2); i < length; i++)
         ((Uint8*)buffer)[i] ^= ((Uint8*)&XOR)[i&3];
    return length;
}

static int changeSigned16BitSys( AdapterC Data, int length )
{
    return changeSigned( Data, length, 0x80008000 );
}

static int changeSigned16BitWrong( AdapterC Data, int length )
{
    return changeSigned( Data, length, 0x00800080 );
}

static int changeSigned8Bit( AdapterC Data, int length )
{
    return changeSigned( Data, length, 0x80808080 );
}

/*-------------------------------------------------------------------------*/
static int convertStereoToMonoS16Bit( AdapterC Data, int length )
{
    int i;
    Sint16* buffer = (Sint16*) Data.buffer;
    Sint16* src = (Sint16*) Data.buffer;
    length >>= 2;
    for( i = 0; i < length;  i++, src+=2 )
         buffer[i] = ((int) src[0] + src[1] ) >> 1;
    return 2*length;
}

static int convertStereoToMonoU16Bit( AdapterC Data, int length )
{
    int i;
    Uint16* buffer = (Uint16*) Data.buffer;
    Uint16* src = (Uint16*) Data.buffer;
    length >>= 2;
    for( i = 0; i < length;  i++, src+=2 )
         buffer[i] = ((int) src[0] + src[1] ) >> 1;
    return 2*length;
}

static int convertStereoToMonoS8Bit( AdapterC Data, int length )
{
    int i;
    Sint8* buffer = (Sint8*) Data.buffer;
    Sint8* src = (Sint8*) Data.buffer;
    length >>= 1;
    for( i = 0; i < length;  i++, src+=2 )
         buffer[i] = ((int) src[0] + src[1] ) >> 1;
    return length;
}

static int convertStereoToMonoU8Bit( AdapterC Data, int length )
{
    int i;
    Uint8* buffer = (Uint8*) Data.buffer;
    Uint8* src = (Uint8*) Data.buffer;
    length >>= 1;
    for( i = 0; i < length;  i++, src+=2 )
         buffer[i] = ((int) src[0] + src[1] ) >> 1;
    return length;
}

/*-------------------------------------------------------------------------*/
static int convertMonoToStereo16Bit( AdapterC Data, int length )
{
    int i;
    Uint16* buffer = (Uint16*) Data.buffer;
    Uint16* dst = (Uint16*)Data.buffer + length - 2;
    for( i = length>>1; i--; dst-=2 )
         dst[0] = dst[1] = buffer[i];
    return 2*length;
}

static int convertMonoToStereo8Bit( AdapterC Data, int length )
{
    int i;
    Uint8* buffer = Data.buffer;
    Uint8* buffer1 = Data.buffer + 1;
    for( i = length-1; i >= 0; i-- )
         buffer[2*i] = buffer1[2*i] = buffer[i];
    return 2*length;
}

/*-------------------------------------------------------------------------*/
static int minus5dB( AdapterC Data, int length )
{
    int i;
    Sint16* buffer = (Sint16*) Data.buffer;
    for(i = length>>1; i--; )
        buffer[i]= (38084 * (int)buffer[i]) >> 16;
    return length;
}

/*-------------------------------------------------------------------------*/
enum RateConverterType{
    dcrsRate = -1,
    incrsRate = 0,
    hlfRate = 1,
    dblRate = 2
};

static void initRateConverterBuffer( RateConverterBuffer *rcb,
    AdapterC* Data, int length, enum RateConverterType typ )
{
    int size, minsize, dir;
    int den[] = { 0, 1, 2};
    int num[] = { 0, 2, 1};
    int i;

    den[incrsRate] = Data->filter->denominator;
    num[incrsRate] = Data->filter->numerator;

    size = 8 * _fsize;
    dir = ~typ&1;
    length >>= 1;
    minsize = min( length, size );

    rcb->buffer = (Sint16*)( Data->buffer );

    if( Data->mode & SDL_SOUND_Loop )
    {
        // !!!FIXME: modulo length, take scale into account,
        // check against the 'else' part
        for( i = 0; i < size; i++ )
        {
            rcb->inbuffer[i] = rcb->buffer[length-size+i];
            rcb->inbuffer[i+size] = rcb->buffer[i];
        }
        rcb->finp = rcb->linp = rcb->inbuffer + size;
        if( size < 0 )
            rcb->buffer += num[typ] * ( length + 2 * size ) / den[typ];
    }
    else
    {
        for( i = 0; i < minsize; i++ )
        {
            rcb->inbuffer[i] = rcb->buffer[length-size+i];
            rcb->inbuffer[i+size] = 0;
            rcb->inbuffer[i+2*size] = rcb->buffer[i];
        }
        for( ; i < size; i++ )
        {
            rcb->inbuffer[i] = 0;
            rcb->inbuffer[i+size] = 0;
            rcb->inbuffer[i+2*size] = 0;
        }
        rcb->flength = rcb->llength = size/2 + minsize/2;
        rcb->clength = length - minsize;

        if( dir )
        {
            rcb->finp = rcb->inbuffer + 2 * size + minsize/2;
            rcb->cinp = rcb->buffer + length - minsize/2;
            rcb->linp = rcb->inbuffer + size + minsize/2;
            rcb->buffer += den[typ] * ( length + minsize ) / num[typ];
        }
        else
        {
            rcb->finp = rcb->inbuffer + size/2;
            rcb->cinp = rcb->buffer + size/2;
            rcb->linp = rcb->inbuffer + 3*size/2;
        }
    }
}

static void nextRateConverterBuffer( RateConverterBuffer *rcb )
{
    rcb->buffer++;
    rcb->finp++;
    rcb->cinp++;
    rcb->linp++;
}

typedef Sint16* (*RateConverter)( Sint16*, Sint16*, int, VarFilter*, int*);
static int doRateConversion( RateConverterBuffer* rcb,
                             RateConverter ffp, VarFilter* filter )
{
    int pos = 0;
    Sint16 *outp;
    outp = rcb->buffer;

    outp = (*ffp)( outp, rcb->finp, rcb->flength, filter, &pos );
    outp = (*ffp)( outp, rcb->cinp, rcb->clength, filter, &pos );
    outp = (*ffp)( outp, rcb->linp, rcb->llength, filter, &pos );
    return 2 * abs( rcb->buffer - outp );
}


/*-------------------------------------------------------------------------*/
   /*
    * !!! FIXME !!!
    *
    * The doubleRate filter is half as large as the halfRate one!  Frank
    */


static int doubleRateMono( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, dblRate );
    return doRateConversion( &rcb, doubleRate1, NULL );
}

static int doubleRateStereo( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, dblRate );
    doRateConversion( &rcb, doubleRate2, NULL );
    nextRateConverterBuffer( &rcb );
    return 2 + doRateConversion( &rcb, doubleRate2, NULL );
}

/*-------------------------------------------------------------------------*/
static int halfRateMono( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, hlfRate );
    return doRateConversion( &rcb, halfRate1, NULL );
}

static int halfRateStereo( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, hlfRate );
    doRateConversion( &rcb, halfRate2, NULL );
    nextRateConverterBuffer( &rcb );
    return 2 + doRateConversion( &rcb, halfRate2, NULL );
}

/*-------------------------------------------------------------------------*/
static int increaseRateMono( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, incrsRate );
    return doRateConversion( &rcb, increaseRate1, Data.filter );
}

static int increaseRateStereo( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, incrsRate );
    doRateConversion( &rcb, increaseRate2, Data.filter );
    nextRateConverterBuffer( &rcb );
    return 2 + doRateConversion( &rcb, increaseRate2, Data.filter );
}

/*-------------------------------------------------------------------------*/
static int decreaseRateMono( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, dcrsRate );
    return doRateConversion( &rcb, decreaseRate1, Data.filter );
}

static int decreaseRateStereo( AdapterC Data, int length )
{
    RateConverterBuffer rcb;
    initRateConverterBuffer( &rcb, &Data, length, dcrsRate );
    doRateConversion( &rcb, decreaseRate2, Data.filter );
    nextRateConverterBuffer( &rcb );
    return doRateConversion( &rcb, decreaseRate2, Data.filter );
}

/*-------------------------------------------------------------------------*/
static int padSilence( AdapterC Data, int length )
{
    Uint32 zero, *buffer;
    int i, mask = 0;

    buffer = (Uint32*) ( Data.buffer + length );
    if( Data.mode != SDL_SOUND_Loop )
        mask = Data.filter->mask;
    length = mask - ( ( length - 1 ) & mask );
    zero = Data.filter->zero;

    for( i = length>>2; i--;  )
         buffer[i] = zero;
    for( i = 4*(length>>2); i < length; i++)
         ((Uint8*)buffer)[i] ^= ((Uint8*)&zero)[i&3];

    return length + ((Uint8*)buffer - Data.buffer);
}

/*-------------------------------------------------------------------------*/
typedef struct{
    Sint16 numerator;
    Sint16 denominator;
} Fraction;

const Fraction Half = {1, 2};
const Fraction Double = {2, 1};

/* gives a maximal error of 3% and typical less than 0.2% */
static Fraction findFraction( float Value )
{
    const Sint8 frac[95]={
         2,                                                  -1, /*  /1 */
      1,    3,                                               -1, /*  /2 */
         2,    4, 5,                                         -1, /*  /3 */
            3,    5,    7,                                   -1, /*  /4 */
            3, 4,    6, 7, 8, 9,                             -1, /*  /5 */
                  5,    7,           11,                     -1, /*  /6 */
               4, 5, 6,    8, 9, 10, 11, 12, 13,             -1, /*  /7 */
                  5,    7,    9,     11,     13,     15,     -1, /*  /8 */
                  5,    7, 8,    10, 11,     13, 14,     16, -1, /*  /9 */
                        7,    9,     11,     13,             -1, /* /10 */
                     6, 7, 8, 9, 10,     12, 13, 14, 15, 16, -1, /* /11 */
                        7,           11,     13,             -1, /* /12 */
                        7, 8, 9, 10, 11, 12,     14, 15, 16, -1, /* /13 */
                              9,     11,     13,     15,     -1, /* /14 */
                           8,        11,     13, 14,     16, -1, /* /15 */
                              9,     11,     13,     15       }; /* /16 */


    Fraction Result = {0,0};
    int i,num,den=1;

    float RelErr, BestErr = 0;
    if( Value < 31/64. || Value > 64/31. ) return Result;

    for( i = 0; i < SDL_TABLESIZE(frac); i++ )
    {
         num = frac[i];
         if( num < 0 ) den++;
         RelErr = Value * num / den;
         RelErr = min( RelErr, 1/RelErr );
         if( RelErr > BestErr )
         {
             BestErr = RelErr;
             Result.denominator = den;
             Result.numerator = num;
         }
    }
    return Result;
}

/*-------------------------------------------------------------------------*/
static float sinc( float x )
{
    if( x > -1e-24 && x < 1e-24 ) return 1.;
    else return sin(x)/x;
}

static float calculateVarFilter( Sint16* dst,
                                 float Ratio, float phase, float scale )
{
    const Uint16 KaiserWindow7[]= {
        22930, 16292, 14648, 14288, 14470, 14945, 15608, 16404,
        17304, 18289, 19347, 20467, 21644, 22872, 24145, 25460,
        26812, 28198, 29612, 31052, 32513, 33991, 35482, 36983,
        38487, 39993, 41494, 42986, 44466, 45928, 47368, 48782,
        50165, 51513, 52821, 54086, 55302, 56466, 57575, 58624,
        59610, 60529, 61379, 62156, 62858, 63483, 64027, 64490,
        64870, 65165, 65375, 65498, 65535, 65484, 65347, 65124,
        64815, 64422, 63946, 63389, 62753, 62039, 61251, 60391 };
    int i;
    float w;
    const float fg = -.018 + .5 * Ratio;
    const float omega = 2 * M_PI * fg;
    fprintf( stderr, "    phase: %6g \n", phase );
    phase -= 63;
    for( i = 0; i < 64; i++)
    {
        w = scale * ( KaiserWindow7[i] * ( i + 1 ));
        dst[i] = w * sinc( omega * (i+phase) );
        dst[127-i] = w * sinc( omega * (127-i+phase) );
    }
    return fg;
}

static Fraction setupVarFilter( VarFilter* filter, float Ratio )
{
    int pos,n,d, incr, phase = 0;
    float Scale, rd, fg;
    Fraction IRatio;

    IRatio = findFraction( Ratio );
//    Scale = Ratio < 1. ? 0.0364733 : 0.0211952;
    Scale = 0.0084778;
    Ratio = min( Ratio, 0.97 );

    n = IRatio.numerator;
    d = IRatio.denominator;
    filter->denominator = d;
    filter->numerator = n;
    rd = 1. / d;

    fprintf( stderr, "Filter:\n" );

    for( pos = 0; pos < d; pos++ )
    {
        fg = calculateVarFilter( filter->c[pos], Ratio, phase*rd, Scale );
        phase += n;
        filter->incr[pos] = phase / d;
        phase %= d;
    }
    fprintf( stderr, "    fg:  %6g\n\n", fg );
/* !!!FIXME: get rid of the inversion -Frank*/
    IRatio.numerator = d;
    IRatio.denominator = n;
    return IRatio;
}
/*-------------------------------------------------------------------------*/
static void adjustSize( Sound_AudioCVT *Data, int add, Fraction f )
{

    double ratio = f.numerator / (double) f.denominator;
    Data->len_ratio *= ratio;
    Data->len_mult = max( Data->len_mult, ceil(Data->len_ratio) );
    Data->add = ratio * (Data->add + add);
    Data->len_add = max( Data->len_add, ceil(Data->add) );
}

static void initSize( Sound_AudioCVT *Data )
{
    Data->len_ratio = 1.;
    Data->len_mult = 1;
    Data->add = 0;
    Data->len_add = 0;
}

/*-------------------------------------------------------------------------*/
static void createRateConverter( Sound_AudioCVT *Data, int* fip,
                                 int SrcRate, int DestRate, int Channel )
{
    Fraction f;
    int filter_index = *fip;
    int VarPos = 0;
    int Mono = 2 - Channel;
    float Ratio = DestRate;
    *fip = -1;


    if( SrcRate < 1 || SrcRate > 1<<18 ||
        DestRate < 1 || DestRate > 1<<18 ) return;
    Ratio /= SrcRate;

    if( Ratio > 1.)
        VarPos = filter_index++;
    else
        Data->adapter[filter_index++] = minus5dB;

    while( Ratio > 64./31.)
    {
        Data->adapter[filter_index++] =
            Mono ? doubleRateMono : doubleRateStereo;
        Ratio /= 2.;
        adjustSize( Data, _fsize, Double );
    }

    while( Ratio < 31./64. )
    {
        Data->adapter[filter_index++] =
            Mono ? halfRateMono : halfRateStereo;
        Ratio *= 2;
        adjustSize( Data, _fsize, Half );
    }

    if( Ratio > 1. )
    {
        f = setupVarFilter( &Data->filter, Ratio );
        Data->adapter[VarPos] =
            Mono ? increaseRateMono : increaseRateStereo;
        adjustSize( Data, _fsize, f );
    }
    else
    {
        f = setupVarFilter( &Data->filter, Ratio );
        Data->adapter[filter_index++] =
            Mono ? decreaseRateMono : decreaseRateStereo;
        adjustSize( Data, _fsize, f );
    }
    *fip = filter_index;
}

/*-------------------------------------------------------------------------*/
static void createFormatConverter16Bit(Sound_AudioCVT *Data, int* fip,
    SDL_AudioSpec src, SDL_AudioSpec dst )
{
    int filter_index = *fip;

    if( src.channels == 2 && dst.channels == 1 )
    {
        adjustSize( Data, 0, Half );

        if( !IS_SYSENDIAN(src) )
            Data->adapter[filter_index++] = swapBytes;

        if( IS_SIGNED(src) )
            Data->adapter[filter_index++] = convertStereoToMonoS16Bit;
        else
            Data->adapter[filter_index++] = convertStereoToMonoU16Bit;

        if( !IS_SYSENDIAN(dst) )
            Data->adapter[filter_index++] = swapBytes;
    }
    else if( IS_SYSENDIAN(src) != IS_SYSENDIAN(dst) )
        Data->adapter[filter_index++] = swapBytes;

    if( IS_SIGNED(src) != IS_SIGNED(dst) )
    {
        if( IS_SYSENDIAN(dst) )
            Data->adapter[filter_index++] = changeSigned16BitSys;
        else
            Data->adapter[filter_index++] = changeSigned16BitWrong;
    }

    if( src.channels == 1 && dst.channels == 2 )
    {
        adjustSize( Data, 0, Double );
        Data->adapter[filter_index++] = convertMonoToStereo16Bit;
    }

    *fip = filter_index;
}

/*-------------------------------------------------------------------------*/
static void createFormatConverter8Bit(Sound_AudioCVT *Data, int *fip,
    SDL_AudioSpec src, SDL_AudioSpec dst )
{
    int filter_index = *fip;
    if( IS_16BIT(src) )
    {
        adjustSize( Data, 0, Half );

        if( IS_SYSENDIAN(src) )
            Data->adapter[filter_index++] = cut16BitSysTo8Bit;
        else
            Data->adapter[filter_index++] = cut16BitWrongTo8Bit;
    }

    if( src.channels == 2 && dst.channels == 1 )
    {
        adjustSize( Data, 0, Half );

        if( IS_SIGNED(src) )
            Data->adapter[filter_index++] = convertStereoToMonoS8Bit;
        else
            Data->adapter[filter_index++] = convertStereoToMonoU8Bit;
    }

    if( IS_SIGNED(src) != IS_SIGNED(dst) )
        Data->adapter[filter_index++] = changeSigned8Bit;

    if( src.channels == 1 && dst.channels == 2  )
    {
        adjustSize( Data, 0, Double );
        Data->adapter[filter_index++] = convertMonoToStereo8Bit;
    }

    if( !IS_8BIT(dst) )
    {
        adjustSize( Data, 0, Double );
        if( IS_SYSENDIAN(dst) )
            Data->adapter[filter_index++] = expand8BitTo16BitSys;
        else
            Data->adapter[filter_index++] = expand8BitTo16BitWrong;
    }

    *fip = filter_index;
}

/*-------------------------------------------------------------------------*/
static void createFormatConverter(Sound_AudioCVT *Data, int *fip,
    SDL_AudioSpec src, SDL_AudioSpec dst )
{
    int filter_index = *fip;

    if( IS_FLOAT(src) )
    {
        Data->adapter[filter_index++] = cutFloatTo16Bit;
        adjustSize( Data, 0, Half );
    }

    if( IS_8BIT(src) || IS_8BIT(dst) )
         createFormatConverter8Bit( Data, &filter_index, src, dst);
    else
         createFormatConverter16Bit( Data, &filter_index, src, dst);

    if( IS_FLOAT(dst) )
    {
        Data->adapter[filter_index++] = expand16BitToFloat;
        adjustSize( Data, 0, Double );
    }

    *fip = filter_index;
}

/*-------------------------------------------------------------------------*/
Uint32 getSilenceValue( Uint16 format )
{
    const static float fzero[] = {0.0000001};
    switch( format )
    {
    case 0x0020: return *(Uint32*) fzero;
    default: ;
    }
    return 0;
}

/*-------------------------------------------------------------------------*/
int BuildAudioCVT( Sound_AudioCVT *Data,
                   SDL_AudioSpec src, SDL_AudioSpec dst )
{
    SDL_AudioSpec intrm;
    int filter_index = 0;

    if( Data == NULL ) return -1;
    initSize( Data );
    Data->filter.denominator = 0;
    Data->filter.zero = getSilenceValue( dst.format );
    Data->filter.mask = dst.size - 1;

    /* Check channels */
    if( src.channels < 1 || src.channels > 2 ||
        dst.channels < 1 || dst.channels > 2 ) goto error_exit;

    /* If no frequency conversion is needed, go straight to dst format */
    if( src.freq == dst.freq )
    {
        createFormatConverter( Data, &filter_index, src, dst );
        goto sucess_exit;
    }

    /* Convert to signed 16Bit System-Endian */
    intrm.format = AUDIO_S16SYS;
    intrm.channels = min( src.channels, dst.channels );
    createFormatConverter( Data, &filter_index, src, intrm );

    /* Do rate conversion */
    if( src.channels == 2 && dst.channels == 2 )
        createRateConverter( Data, &filter_index, src.freq, dst.freq, 2 );
    else
        createRateConverter( Data, &filter_index, src.freq, dst.freq, 1 );
    /* propagate error */
    if( filter_index < 0 ) goto error_exit;

    /* Convert to final format */
    createFormatConverter( Data, &filter_index, intrm, dst );

    /* Set up the filter information */
sucess_exit:
    Data->adapter[filter_index++] = padSilence;
    Data->adapter[filter_index] = NULL;
/* !!! FIXME: Is it okay to assign NULL to a function pointer?
              Borland says no. -frank */
    return 0;

error_exit:
/* !!! FIXME: Is it okay to assign NULL to a function pointer?
              Borland says no. -frank    */
    Data->adapter[0] = NULL;
    return -1;
}

/*-------------------------------------------------------------------------*/
static char *fmt_to_str(Uint16 fmt)
{
    switch (fmt)
    {
        case AUDIO_U8:     return "    U8";
        case AUDIO_S8:     return "    S8";
        case AUDIO_U16MSB: return "U16MSB";
        case AUDIO_S16MSB: return "S16MSB";
        case AUDIO_U16LSB: return "U16LSB";
        case AUDIO_S16LSB: return "S16LSB";
    }
    return "??????";
}

#define AdapterDesc(x) { x, #x }

static void show_AudioCVT( Sound_AudioCVT *Data )
{
    int i,j;
    const struct{ int (*adapter) ( AdapterC, int); Sint8 *name; }
    AdapterDescription[] = {
        AdapterDesc(expand8BitTo16BitSys),
        AdapterDesc(expand8BitTo16BitWrong),
        AdapterDesc(expand16BitToFloat),
        AdapterDesc(swapBytes),
        AdapterDesc(cut16BitSysTo8Bit),
        AdapterDesc(cut16BitWrongTo8Bit),
        AdapterDesc(cutFloatTo16Bit),
        AdapterDesc(changeSigned16BitSys),
        AdapterDesc(changeSigned16BitWrong),
        AdapterDesc(changeSigned8Bit),
        AdapterDesc(convertStereoToMonoS16Bit),
        AdapterDesc(convertStereoToMonoU16Bit),
        AdapterDesc(convertStereoToMonoS8Bit),
        AdapterDesc(convertStereoToMonoU8Bit),
        AdapterDesc(convertMonoToStereo16Bit),
        AdapterDesc(convertMonoToStereo8Bit),
        AdapterDesc(minus5dB),
        AdapterDesc(doubleRateMono),
        AdapterDesc(doubleRateStereo),
        AdapterDesc(halfRateMono),
        AdapterDesc(halfRateStereo),
        AdapterDesc(increaseRateMono),
        AdapterDesc(increaseRateStereo),
        AdapterDesc(decreaseRateMono),
        AdapterDesc(decreaseRateStereo),
        AdapterDesc(padSilence),
        { NULL,    "----------NULL-----------\n" }
    };

    fprintf( stderr, "Sound_AudioCVT:\n" );
    fprintf( stderr, "    needed:      %8d\n", Data->needed );
    fprintf( stderr, "    add:         %8g\n", Data->add );
    fprintf( stderr, "    len_add:     %8d\n", Data->len_add );
    fprintf( stderr, "    len_ratio:   %8g\n", Data->len_ratio );
    fprintf( stderr, "    len_mult:    %8d\n", Data->len_mult );
    fprintf( stderr, "    filter->mask: %#7x\n", Data->filter.mask );
    fprintf( stderr, "\n" );

    fprintf( stderr, "Adapter List:    \n" );
    for( i = 0; i < 32; i++ )
    {
        for( j = 0; j < SDL_TABLESIZE(AdapterDescription); j++ )
        {
            if( Data->adapter[i] == AdapterDescription[j].adapter )
            {
                fprintf( stderr, "    %s \n", AdapterDescription[j].name );
                if( Data->adapter[i] == NULL ) goto sucess_exit;
                goto cont;
            }
        }
        fprintf( stderr, "    Error: unknown adapter\n" );

        cont:
    }
    fprintf( stderr, "    Error: NULL adapter missing\n" );
    sucess_exit:
    if( Data->filter.denominator )
    {
        fprintf( stderr, "Variable Rate Converter:\n"
                         "    numerator:   %3d\n"
                         "    denominator: %3d\n",
                         Data->filter.denominator, Data->filter.numerator );

        fprintf( stderr, "    increment sequence:\n"
                         "    " );
        for( i = 0; i < Data->filter.denominator; i++ )
             fprintf( stderr, "%1d ", Data->filter.incr[i] );

        fprintf( stderr, "\n" );
    }
    else
    {
        fprintf( stderr, "No Variable Rate Converter\n" );
    }
}


int Sound_BuildAudioCVT(Sound_AudioCVT *Data,
    Uint16 src_format, Uint8 src_channels, int src_rate,
    Uint16 dst_format, Uint8 dst_channels, int dst_rate, Uint32 dst_size )
{
    SDL_AudioSpec src, dst;
    int ret;

    fprintf (stderr,
             "Sound_BuildAudioCVT():\n"
             "-----------------------------\n"
             "format:    %s ->  %s\n"
             "channels:  %6d ->  %6d\n"
             "rate:      %6d ->  %6d\n"
             "size:  don't care -> %#7x\n\n",
             fmt_to_str (src_format), fmt_to_str (dst_format),
             src_channels,            dst_channels,
             src_rate,                dst_rate,
                                      dst_size );

    src.format = src_format;
    src.channels = src_channels;
    src.freq = src_rate;

    dst.format = dst_format;
    dst.channels = dst_channels;
    dst.freq = dst_rate;
    dst.size = dst_size;

    ret = BuildAudioCVT( Data, src, dst );
    Data->needed = 1;

    show_AudioCVT( Data );
    fprintf (stderr, "\n"
                     "return value: %d \n\n\n", ret );

    return ret;
}