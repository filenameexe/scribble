#ifndef foohooklistfoo
#define foohooklistfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulsecore/llist.h>
#include <pulse/xmalloc.h>
#include <pulsecore/gccmacro.h>

typedef struct pa_hook_slot pa_hook_slot;
typedef struct pa_hook pa_hook;

typedef enum pa_hook_result {
    PA_HOOK_OK = 0,
    PA_HOOK_STOP = 1,
    PA_HOOK_CANCEL = -1
} pa_hook_result_t;

typedef pa_hook_result_t (*pa_hook_cb_t)(
        void *hook_data,
        void *call_data,
        void *slot_data);

struct pa_hook_slot {
    int dead;
    pa_hook *hook;
    pa_hook_cb_t callback;
    void *data;
    PA_LLIST_FIELDS(pa_hook_slot);
};

struct pa_hook {
    PA_LLIST_HEAD(pa_hook_slot, slots);
    pa_hook_slot *last;
    int firing, n_dead;

    void *data;
};

void pa_hook_init(pa_hook *hook, void *data);
void pa_hook_free(pa_hook *hook);

pa_hook_slot* pa_hook_connect(pa_hook *hook, pa_hook_cb_t, void *data);
void pa_hook_slot_free(pa_hook_slot *slot);

pa_hook_result_t pa_hook_fire(pa_hook *hook, void *data);

#endif
