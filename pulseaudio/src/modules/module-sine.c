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

#include <stdio.h>
#include <math.h>

#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/module.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>

#include "module-sine-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Sine wave generator");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE("sink=<sink to connect to> frequency=<frequency in Hz>");

struct userdata {
    pa_core *core;
    pa_module *module;
    pa_sink_input *sink_input;
    pa_memblock *memblock;
    size_t peek_index;
};

static const char* const valid_modargs[] = {
    "sink",
    "frequency",
    NULL,
};

static int sink_input_peek_cb(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct userdata *u;

    pa_assert(i);
    u = i->userdata;
    pa_assert(u);
    pa_assert(chunk);

    chunk->memblock = pa_memblock_ref(u->memblock);
    chunk->index = u->peek_index;
    chunk->length = pa_memblock_get_length(u->memblock) - u->peek_index;

    return 0;
}

static void sink_input_drop_cb(pa_sink_input *i, size_t length) {
    struct userdata *u;
    size_t l;

    pa_assert(i);
    u = i->userdata;
    pa_assert(u);
    pa_assert(length > 0);

    u->peek_index += length;

    l = pa_memblock_get_length(u->memblock);

    while (u->peek_index >= l)
        u->peek_index -= l;
}

static void sink_input_kill_cb(pa_sink_input *i) {
    struct userdata *u;

    pa_assert(i);
    u = i->userdata;
    pa_assert(u);

    pa_sink_input_unlink(u->sink_input);
    pa_sink_input_unref(u->sink_input);
    u->sink_input = NULL;

    pa_module_unload_request(u->module);
}

static void calc_sine(float *f, size_t l, float freq) {
    size_t i;

    l /= sizeof(float);

    for (i = 0; i < l; i++)
        f[i] = (float) sin((double) i/l*M_PI*2*freq)/2;
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;
    pa_sink *sink;
    pa_sample_spec ss;
    uint32_t frequency;
    char t[256];
    void *p;
    pa_sink_input_new_data data;

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    m->userdata = u = pa_xnew0(struct userdata, 1);
    u->core = m->core;
    u->module = m;
    u->sink_input = NULL;
    u->memblock = NULL;
    u->peek_index = 0;

    if (!(sink = pa_namereg_get(m->core, pa_modargs_get_value(ma, "sink", NULL), PA_NAMEREG_SINK, 1))) {
        pa_log("No such sink.");
        goto fail;
    }

    ss.format = PA_SAMPLE_FLOAT32;
    ss.rate = sink->sample_spec.rate;
    ss.channels = 1;

    frequency = 440;
    if (pa_modargs_get_value_u32(ma, "frequency", &frequency) < 0 || frequency < 1 || frequency > ss.rate/2) {
        pa_log("Invalid frequency specification");
        goto fail;
    }

    u->memblock = pa_memblock_new(m->core->mempool, pa_bytes_per_second(&ss));
    p = pa_memblock_acquire(u->memblock);
    calc_sine(p, pa_memblock_get_length(u->memblock), frequency);
    pa_memblock_release(u->memblock);

    pa_snprintf(t, sizeof(t), "Sine Generator at %u Hz", frequency);

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.driver = __FILE__;
    data.name = t;
    pa_sink_input_new_data_set_sample_spec(&data, &ss);
    data.module = m;

    if (!(u->sink_input = pa_sink_input_new(m->core, &data, 0)))
        goto fail;

    u->sink_input->peek = sink_input_peek_cb;
    u->sink_input->drop = sink_input_drop_cb;
    u->sink_input->kill = sink_input_kill_cb;
    u->sink_input->userdata = u;

    pa_sink_input_put(u->sink_input);

    pa_modargs_free(ma);
    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    pa__done(m);
    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sink_input) {
        pa_sink_input_unlink(u->sink_input);
        pa_sink_input_unref(u->sink_input);
    }

    if (u->memblock)
        pa_memblock_unref(u->memblock);

    pa_xfree(u);
}
