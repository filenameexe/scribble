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

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>
#include <dirent.h>
#include <sys/stat.h>
#include <errno.h>
#include <limits.h>

#ifdef HAVE_GLOB_H
#include <glob.h>
#endif

#ifdef HAVE_WINDOWS_H
#include <windows.h>
#endif

#include <pulse/mainloop.h>
#include <pulse/channelmap.h>
#include <pulse/timeval.h>
#include <pulse/util.h>
#include <pulse/volume.h>
#include <pulse/xmalloc.h>

#include <pulsecore/sink-input.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/play-memchunk.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sound-file.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/core-error.h>
#include <pulsecore/macro.h>

#include "core-scache.h"

#define UNLOAD_POLL_TIME 2

static void timeout_callback(pa_mainloop_api *m, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    pa_core *c = userdata;
    struct timeval ntv;

    pa_assert(c);
    pa_assert(c->mainloop == m);
    pa_assert(c->scache_auto_unload_event == e);

    pa_scache_unload_unused(c);

    pa_gettimeofday(&ntv);
    ntv.tv_sec += UNLOAD_POLL_TIME;
    m->time_restart(e, &ntv);
}

static void free_entry(pa_scache_entry *e) {
    pa_assert(e);

    pa_namereg_unregister(e->core, e->name);
    pa_subscription_post(e->core, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_REMOVE, e->index);
    pa_xfree(e->name);
    pa_xfree(e->filename);
    if (e->memchunk.memblock)
        pa_memblock_unref(e->memchunk.memblock);
    pa_xfree(e);
}

static pa_scache_entry* scache_add_item(pa_core *c, const char *name) {
    pa_scache_entry *e;

    pa_assert(c);
    pa_assert(name);

    if ((e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0))) {
        if (e->memchunk.memblock)
            pa_memblock_unref(e->memchunk.memblock);

        pa_xfree(e->filename);

        pa_assert(e->core == c);

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
    } else {
        e = pa_xnew(pa_scache_entry, 1);

        if (!pa_namereg_register(c, name, PA_NAMEREG_SAMPLE, e, 1)) {
            pa_xfree(e);
            return NULL;
        }

        e->name = pa_xstrdup(name);
        e->core = c;

        if (!c->scache) {
            c->scache = pa_idxset_new(pa_idxset_trivial_hash_func, pa_idxset_trivial_compare_func);
            pa_assert(c->scache);
        }

        pa_idxset_put(c->scache, e, &e->index);

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_NEW, e->index);
    }

    e->last_used_time = 0;
    e->memchunk.memblock = NULL;
    e->memchunk.index = e->memchunk.length = 0;
    e->filename = NULL;
    e->lazy = 0;
    e->last_used_time = 0;

    memset(&e->sample_spec, 0, sizeof(e->sample_spec));
    pa_channel_map_init(&e->channel_map);
    pa_cvolume_reset(&e->volume, PA_CHANNELS_MAX);

    return e;
}

int pa_scache_add_item(pa_core *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map, const pa_memchunk *chunk, uint32_t *idx) {
    pa_scache_entry *e;
    char st[PA_SAMPLE_SPEC_SNPRINT_MAX];

    pa_assert(c);
    pa_assert(name);

    if (chunk && chunk->length > PA_SCACHE_ENTRY_SIZE_MAX)
        return -1;

    if (!(e = scache_add_item(c, name)))
        return -1;

    if (ss) {
        e->sample_spec = *ss;
        pa_channel_map_init_auto(&e->channel_map, ss->channels, PA_CHANNEL_MAP_DEFAULT);
        e->volume.channels = e->sample_spec.channels;
    }

    if (map)
        e->channel_map = *map;

    if (chunk) {
        e->memchunk = *chunk;
        pa_memblock_ref(e->memchunk.memblock);
    }

    if (idx)
        *idx = e->index;

    pa_log_debug("Created sample \"%s\" (#%d), %lu bytes with sample spec %s",
                 name, e->index, (unsigned long) e->memchunk.length,
                 pa_sample_spec_snprint(st, sizeof(st), &e->sample_spec));

    return 0;
}

int pa_scache_add_file(pa_core *c, const char *name, const char *filename, uint32_t *idx) {
    pa_sample_spec ss;
    pa_channel_map map;
    pa_memchunk chunk;
    int r;

#ifdef OS_IS_WIN32
    char buf[MAX_PATH];

    if (ExpandEnvironmentStrings(filename, buf, MAX_PATH))
        filename = buf;
#endif

    pa_assert(c);
    pa_assert(name);
    pa_assert(filename);

    if (pa_sound_file_load(c->mempool, filename, &ss, &map, &chunk) < 0)
        return -1;

    r = pa_scache_add_item(c, name, &ss, &map, &chunk, idx);
    pa_memblock_unref(chunk.memblock);

    return r;
}

int pa_scache_add_file_lazy(pa_core *c, const char *name, const char *filename, uint32_t *idx) {
    pa_scache_entry *e;

#ifdef OS_IS_WIN32
    char buf[MAX_PATH];

    if (ExpandEnvironmentStrings(filename, buf, MAX_PATH))
        filename = buf;
#endif

    pa_assert(c);
    pa_assert(name);
    pa_assert(filename);

    if (!(e = scache_add_item(c, name)))
        return -1;

    e->lazy = 1;
    e->filename = pa_xstrdup(filename);

    if (!c->scache_auto_unload_event) {
        struct timeval ntv;
        pa_gettimeofday(&ntv);
        ntv.tv_sec += UNLOAD_POLL_TIME;
        c->scache_auto_unload_event = c->mainloop->time_new(c->mainloop, &ntv, timeout_callback, c);
    }

    if (idx)
        *idx = e->index;

    return 0;
}

int pa_scache_remove_item(pa_core *c, const char *name) {
    pa_scache_entry *e;

    pa_assert(c);
    pa_assert(name);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0)))
        return -1;

    if (pa_idxset_remove_by_data(c->scache, e, NULL) != e)
        pa_assert(0);

    pa_log_debug("Removed sample \"%s\"", name);

    free_entry(e);

    return 0;
}

static void free_cb(void *p, PA_GCC_UNUSED void *userdata) {
    pa_scache_entry *e = p;
    pa_assert(e);

    free_entry(e);
}

void pa_scache_free(pa_core *c) {
    pa_assert(c);

    if (c->scache) {
        pa_idxset_free(c->scache, free_cb, NULL);
        c->scache = NULL;
    }

    if (c->scache_auto_unload_event)
        c->mainloop->time_free(c->scache_auto_unload_event);
}

int pa_scache_play_item(pa_core *c, const char *name, pa_sink *sink, pa_volume_t volume) {
    pa_scache_entry *e;
    char *t;
    pa_cvolume r;

    pa_assert(c);
    pa_assert(name);
    pa_assert(sink);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 1)))
        return -1;

    if (e->lazy && !e->memchunk.memblock) {
        if (pa_sound_file_load(c->mempool, e->filename, &e->sample_spec, &e->channel_map, &e->memchunk) < 0)
            return -1;

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);

        if (e->volume.channels > e->sample_spec.channels)
            e->volume.channels = e->sample_spec.channels;
    }

    if (!e->memchunk.memblock)
        return -1;

    pa_log_debug("Playing sample \"%s\" on \"%s\"", name, sink->name);

    t = pa_sprintf_malloc("sample:%s", name);

    pa_cvolume_set(&r, e->volume.channels, volume);
    pa_sw_cvolume_multiply(&r, &r, &e->volume);

    if (pa_play_memchunk(sink, t, &e->sample_spec, &e->channel_map, &e->memchunk, &r) < 0) {
        pa_xfree(t);
        return -1;
    }

    pa_xfree(t);

    if (e->lazy)
        time(&e->last_used_time);

    return 0;
}

int pa_scache_play_item_by_name(pa_core *c, const char *name, const char*sink_name, pa_volume_t volume, int autoload) {
    pa_sink *sink;

    pa_assert(c);
    pa_assert(name);

    if (!(sink = pa_namereg_get(c, sink_name, PA_NAMEREG_SINK, autoload)))
        return -1;

    return pa_scache_play_item(c, name, sink, volume);
}

const char * pa_scache_get_name_by_id(pa_core *c, uint32_t id) {
    pa_scache_entry *e;

    pa_assert(c);
    pa_assert(id != PA_IDXSET_INVALID);

    if (!c->scache || !(e = pa_idxset_get_by_index(c->scache, id)))
        return NULL;

    return e->name;
}

uint32_t pa_scache_get_id_by_name(pa_core *c, const char *name) {
    pa_scache_entry *e;

    pa_assert(c);
    pa_assert(name);

    if (!(e = pa_namereg_get(c, name, PA_NAMEREG_SAMPLE, 0)))
        return PA_IDXSET_INVALID;

    return e->index;
}

uint32_t pa_scache_total_size(pa_core *c) {
    pa_scache_entry *e;
    uint32_t idx, sum = 0;

    pa_assert(c);

    if (!c->scache || !pa_idxset_size(c->scache))
        return 0;

    for (e = pa_idxset_first(c->scache, &idx); e; e = pa_idxset_next(c->scache, &idx))
        if (e->memchunk.memblock)
            sum += e->memchunk.length;

    return sum;
}

void pa_scache_unload_unused(pa_core *c) {
    pa_scache_entry *e;
    time_t now;
    uint32_t idx;

    pa_assert(c);

    if (!c->scache || !pa_idxset_size(c->scache))
        return;

    time(&now);

    for (e = pa_idxset_first(c->scache, &idx); e; e = pa_idxset_next(c->scache, &idx)) {

        if (!e->lazy || !e->memchunk.memblock)
            continue;

        if (e->last_used_time + c->scache_idle_time > now)
            continue;

        pa_memblock_unref(e->memchunk.memblock);
        e->memchunk.memblock = NULL;
        e->memchunk.index = e->memchunk.length = 0;

        pa_subscription_post(c, PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE|PA_SUBSCRIPTION_EVENT_CHANGE, e->index);
    }
}

static void add_file(pa_core *c, const char *pathname) {
    struct stat st;
    const char *e;

    pa_core_assert_ref(c);
    pa_assert(pathname);

    e = pa_path_get_filename(pathname);

    if (stat(pathname, &st) < 0) {
        pa_log("stat('%s'): %s", pathname, pa_cstrerror(errno));
        return;
    }

#if defined(S_ISREG) && defined(S_ISLNK)
    if (S_ISREG(st.st_mode) || S_ISLNK(st.st_mode))
#endif
        pa_scache_add_file_lazy(c, e, pathname, NULL);
}

int pa_scache_add_directory_lazy(pa_core *c, const char *pathname) {
    DIR *dir;

    pa_core_assert_ref(c);
    pa_assert(pathname);

    /* First try to open this as directory */
    if (!(dir = opendir(pathname))) {
#ifdef HAVE_GLOB_H
        glob_t p;
        unsigned int i;
        /* If that fails, try to open it as shell glob */

        if (glob(pathname, GLOB_ERR|GLOB_NOSORT, NULL, &p) < 0) {
            pa_log("failed to open directory '%s': %s", pathname, pa_cstrerror(errno));
            return -1;
        }

        for (i = 0; i < p.gl_pathc; i++)
            add_file(c, p.gl_pathv[i]);

        globfree(&p);
#else
        return -1;
#endif
    } else {
        struct dirent *e;

        while ((e = readdir(dir))) {
            char p[PATH_MAX];

            if (e->d_name[0] == '.')
                continue;

            pa_snprintf(p, sizeof(p), "%s/%s", pathname, e->d_name);
            add_file(c, p);
        }
    }

    closedir(dir);
    return 0;
}
