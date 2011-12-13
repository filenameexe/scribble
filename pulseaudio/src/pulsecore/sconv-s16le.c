/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/* Despite the name of this file we implement S32 handling here, too. */

#include <inttypes.h>
#include <stdio.h>

#include <liboil/liboilfuncs.h>

#include <pulsecore/sconv.h>
#include <pulsecore/macro.h>
#include <pulsecore/log.h>

#include "endianmacros.h"

#include "sconv-s16le.h"

#ifndef INT16_FROM
#define INT16_FROM PA_INT16_FROM_LE
#endif

#ifndef INT16_TO
#define INT16_TO PA_INT16_TO_LE
#endif

#ifndef INT32_FROM
#define INT32_FROM PA_INT32_FROM_LE
#endif

#ifndef INT32_TO
#define INT32_TO PA_INT32_TO_LE
#endif

#ifndef SWAP_WORDS
#ifdef WORDS_BIGENDIAN
#define SWAP_WORDS 1
#else
#define SWAP_WORDS 0
#endif
#endif

void pa_sconv_s16le_to_float32ne(unsigned n, const int16_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1

    for (; n > 0; n--) {
        int16_t s = *(a++);
        *(b++) = ((float) INT16_FROM(s))/0x7FFF;
    }

#else
{
    static const double add = 0, factor = 1.0/0x7FFF;
    oil_scaleconv_f32_s16(b, a, n, &add, &factor);
}
#endif
}

void pa_sconv_s32le_to_float32ne(unsigned n, const int32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1

    for (; n > 0; n--) {
        int32_t s = *(a++);
        *(b++) = (float) (((double) INT32_FROM(s))/0x7FFFFFFF);
    }

#else
{
    static const double add = 0, factor = 1.0/0x7FFFFFFF;
    oil_scaleconv_f32_s32(b, a, n, &add, &factor);
}
#endif
}

void pa_sconv_s16le_from_float32ne(unsigned n, const float *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1

    for (; n > 0; n--) {
        int16_t s;
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1, 1);
        s = (int16_t) (v * 0x7FFF);
        *(b++) = INT16_TO(s);
    }

#else
{
    static const double add = 0, factor = 0x7FFF;
    oil_scaleconv_s16_f32(b, a, n, &add, &factor);
}
#endif
}

void pa_sconv_s32le_from_float32ne(unsigned n, const float *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

#if SWAP_WORDS == 1

    for (; n > 0; n--) {
        int32_t s;
        float v = *(a++);

        v = PA_CLAMP_UNLIKELY(v, -1, 1);
        s = (int32_t) ((double) v * (double) 0x7FFFFFFF);
        *(b++) = INT32_TO(s);
    }

#else
{
    static const double add = 0, factor = 0x7FFFFFFF;
    oil_scaleconv_s32_f32(b, a, n, &add, &factor);
}
#endif
}

void pa_sconv_s16le_to_float32re(unsigned n, const int16_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = *(a++);
        float k = ((float) INT16_FROM(s))/0x7FFF;
        uint32_t *j = (uint32_t*) &k;
        *j = PA_UINT32_SWAP(*j);
        *(b++) = k;
    }
}

void pa_sconv_s32le_to_float32re(unsigned n, const int32_t *a, float *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = *(a++);
        float k = (float) (((double) INT32_FROM(s))/0x7FFFFFFF);
        uint32_t *j = (uint32_t*) &k;
        *j = PA_UINT32_SWAP(*j);
        *(b++) = k;
    }
}

void pa_sconv_s16le_from_float32re(unsigned n, const float *a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s;
        float v = *(a++);
        uint32_t *j = (uint32_t*) &v;
        *j = PA_UINT32_SWAP(*j);
        v = PA_CLAMP_UNLIKELY(v, -1, 1);
        s = (int16_t) (v * 0x7FFF);
        *(b++) = INT16_TO(s);
    }
}

void pa_sconv_s32le_from_float32re(unsigned n, const float *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s;
        float v = *(a++);
        uint32_t *j = (uint32_t*) &v;
        *j = PA_UINT32_SWAP(*j);
        v = PA_CLAMP_UNLIKELY(v, -1, 1);
        s = (int32_t) ((double) v * 0x7FFFFFFF);
        *(b++) = INT32_TO(s);
    }
}

void pa_sconv_s32le_to_s16ne(unsigned n, const int32_t*a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = (int16_t) (INT32_FROM(*a) >> 16);
        a++;
        b++;
    }
}

void pa_sconv_s32le_to_s16re(unsigned n, const int32_t*a, int16_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int16_t s = (int16_t) (INT32_FROM(*a) >> 16);
        *b = PA_UINT32_SWAP(s);
        a++;
        b++;
    }
}

void pa_sconv_s32le_from_s16ne(unsigned n, const int16_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        *b = INT32_TO(((int32_t) *a) << 16);
        a++;
        b++;
    }
}

void pa_sconv_s32le_from_s16re(unsigned n, const int16_t *a, int32_t *b) {
    pa_assert(a);
    pa_assert(b);

    for (; n > 0; n--) {
        int32_t s = ((int32_t) PA_UINT16_SWAP(*a)) << 16;
        *b = INT32_TO(s);
        a++;
        b++;
    }
}
