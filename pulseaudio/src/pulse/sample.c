/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include <stdio.h>
#include <math.h>
#include <string.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "sample.h"

size_t pa_sample_size(const pa_sample_spec *spec) {

    static const size_t table[] = {
        [PA_SAMPLE_U8] = 1,
        [PA_SAMPLE_ULAW] = 1,
        [PA_SAMPLE_ALAW] = 1,
        [PA_SAMPLE_S16LE] = 2,
        [PA_SAMPLE_S16BE] = 2,
        [PA_SAMPLE_FLOAT32LE] = 4,
        [PA_SAMPLE_FLOAT32BE] = 4,
        [PA_SAMPLE_S32LE] = 4,
        [PA_SAMPLE_S32BE] = 4,
    };

    pa_assert(spec);
    pa_assert(spec->format >= 0);
    pa_assert(spec->format < PA_SAMPLE_MAX);

    return table[spec->format];
}

size_t pa_frame_size(const pa_sample_spec *spec) {
    pa_assert(spec);

    return pa_sample_size(spec) * spec->channels;
}

size_t pa_bytes_per_second(const pa_sample_spec *spec) {
    pa_assert(spec);
    return spec->rate*pa_frame_size(spec);
}

pa_usec_t pa_bytes_to_usec(uint64_t length, const pa_sample_spec *spec) {
    pa_assert(spec);

    return (pa_usec_t) (((double) length/pa_frame_size(spec)*1000000)/spec->rate);
}

size_t pa_usec_to_bytes(pa_usec_t t, const pa_sample_spec *spec) {
    pa_assert(spec);

    return (size_t) (((double) t * spec->rate / 1000000))*pa_frame_size(spec);
}

int pa_sample_spec_valid(const pa_sample_spec *spec) {
    pa_assert(spec);

    if (spec->rate <= 0 ||
        spec->rate > PA_RATE_MAX ||
        spec->channels <= 0 ||
        spec->channels > PA_CHANNELS_MAX ||
        spec->format >= PA_SAMPLE_MAX ||
        spec->format < 0)
        return 0;

    return 1;
}

int pa_sample_spec_equal(const pa_sample_spec*a, const pa_sample_spec*b) {
    pa_assert(a);
    pa_assert(b);

    return (a->format == b->format) && (a->rate == b->rate) && (a->channels == b->channels);
}

const char *pa_sample_format_to_string(pa_sample_format_t f) {
    static const char* const table[]= {
        [PA_SAMPLE_U8] = "u8",
        [PA_SAMPLE_ALAW] = "aLaw",
        [PA_SAMPLE_ULAW] = "uLaw",
        [PA_SAMPLE_S16LE] = "s16le",
        [PA_SAMPLE_S16BE] = "s16be",
        [PA_SAMPLE_FLOAT32LE] = "float32le",
        [PA_SAMPLE_FLOAT32BE] = "float32be",
        [PA_SAMPLE_S32LE] = "s32le",
        [PA_SAMPLE_S32BE] = "s32be",
    };

    if (f < 0 || f >= PA_SAMPLE_MAX)
        return NULL;

    return table[f];
}

char *pa_sample_spec_snprint(char *s, size_t l, const pa_sample_spec *spec) {
    pa_assert(s);
    pa_assert(l);
    pa_assert(spec);

    if (!pa_sample_spec_valid(spec))
        pa_snprintf(s, l, "Invalid");
    else
        pa_snprintf(s, l, "%s %uch %uHz", pa_sample_format_to_string(spec->format), spec->channels, spec->rate);

    return s;
}

char* pa_bytes_snprint(char *s, size_t l, unsigned v) {
    pa_assert(s);

    if (v >= ((unsigned) 1024)*1024*1024)
        pa_snprintf(s, l, "%0.1f GiB", ((double) v)/1024/1024/1024);
    else if (v >= ((unsigned) 1024)*1024)
        pa_snprintf(s, l, "%0.1f MiB", ((double) v)/1024/1024);
    else if (v >= (unsigned) 1024)
        pa_snprintf(s, l, "%0.1f KiB", ((double) v)/1024);
    else
        pa_snprintf(s, l, "%u B", (unsigned) v);

    return s;
}

pa_sample_format_t pa_parse_sample_format(const char *format) {
    pa_assert(format);

    if (strcasecmp(format, "s16le") == 0)
        return PA_SAMPLE_S16LE;
    else if (strcasecmp(format, "s16be") == 0)
        return PA_SAMPLE_S16BE;
    else if (strcasecmp(format, "s16ne") == 0 || strcasecmp(format, "s16") == 0 || strcasecmp(format, "16") == 0)
        return PA_SAMPLE_S16NE;
    else if (strcasecmp(format, "s16re") == 0)
        return PA_SAMPLE_S16RE;
    else if (strcasecmp(format, "u8") == 0 || strcasecmp(format, "8") == 0)
        return PA_SAMPLE_U8;
    else if (strcasecmp(format, "float32") == 0 || strcasecmp(format, "float32ne") == 0 || strcasecmp(format, "float") == 0)
        return PA_SAMPLE_FLOAT32NE;
    else if (strcasecmp(format, "float32re") == 0)
        return PA_SAMPLE_FLOAT32RE;
    else if (strcasecmp(format, "float32le") == 0)
        return PA_SAMPLE_FLOAT32LE;
    else if (strcasecmp(format, "float32be") == 0)
        return PA_SAMPLE_FLOAT32BE;
    else if (strcasecmp(format, "ulaw") == 0 || strcasecmp(format, "mulaw") == 0)
        return PA_SAMPLE_ULAW;
    else if (strcasecmp(format, "alaw") == 0)
        return PA_SAMPLE_ALAW;
    else if (strcasecmp(format, "s32le") == 0)
        return PA_SAMPLE_S32LE;
    else if (strcasecmp(format, "s32be") == 0)
        return PA_SAMPLE_S32BE;
    else if (strcasecmp(format, "s32ne") == 0 || strcasecmp(format, "s32") == 0 || strcasecmp(format, "32") == 0)
        return PA_SAMPLE_S32NE;
    else if (strcasecmp(format, "s32re") == 0)
        return PA_SAMPLE_S32RE;

    return -1;
}
