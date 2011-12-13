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

#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/log.h>
#include <pulsecore/core-subscribe.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/core-util.h>

#include "module-match-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Playback stream expression matching module");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);
PA_MODULE_USAGE("table=<filename>");

#define WHITESPACE "\n\r \t"

#define DEFAULT_MATCH_TABLE_FILE PA_DEFAULT_CONFIG_DIR"/match.table"
#define DEFAULT_MATCH_TABLE_FILE_USER "match.table"

static const char* const valid_modargs[] = {
    "table",
    NULL,
};

struct rule {
    regex_t regex;
    pa_volume_t volume;
    struct rule *next;
};

struct userdata {
    struct rule *rules;
    pa_subscription *subscription;
};

static int load_rules(struct userdata *u, const char *filename) {
    FILE *f;
    int n = 0;
    int ret = -1;
    struct rule *end = NULL;
    char *fn = NULL;

    pa_assert(u);

    f = filename ?
        fopen(fn = pa_xstrdup(filename), "r") :
        pa_open_config_file(DEFAULT_MATCH_TABLE_FILE, DEFAULT_MATCH_TABLE_FILE_USER, NULL, &fn, "r");

    if (!f) {
        pa_log("failed to open file '%s': %s", fn, pa_cstrerror(errno));
        goto finish;
    }

    pa_lock_fd(fileno(f), 1);

    while (!feof(f)) {
        char *d, *v;
        pa_volume_t volume;
        uint32_t k;
        regex_t regex;
        char ln[256];
        struct rule *rule;

        if (!fgets(ln, sizeof(ln), f))
            break;

        n++;

        pa_strip_nl(ln);

        if (ln[0] == '#' || !*ln )
            continue;

        d = ln+strcspn(ln, WHITESPACE);
        v = d+strspn(d, WHITESPACE);


        if (!*v) {
            pa_log(__FILE__ ": [%s:%u] failed to parse line - too few words", filename, n);
            goto finish;
        }

        *d = 0;
        if (pa_atou(v, &k) < 0) {
            pa_log("[%s:%u] failed to parse volume", filename, n);
            goto finish;
        }

        volume = (pa_volume_t) k;


        if (regcomp(&regex, ln, REG_EXTENDED|REG_NOSUB) != 0) {
            pa_log("[%s:%u] invalid regular expression", filename, n);
            goto finish;
        }

        rule = pa_xnew(struct rule, 1);
        rule->regex = regex;
        rule->volume = volume;
        rule->next = NULL;

        if (end)
            end->next = rule;
        else
            u->rules = rule;
        end = rule;

        *d = 0;
    }

    ret = 0;

finish:
    if (f) {
        pa_lock_fd(fileno(f), 0);
        fclose(f);
    }

    if (fn)
        pa_xfree(fn);

    return ret;
}

static void callback(pa_core *c, pa_subscription_event_type_t t, uint32_t idx, void *userdata) {
    struct userdata *u =  userdata;
    pa_sink_input *si;
    struct rule *r;

    pa_assert(c);
    pa_assert(u);

    if (t != (PA_SUBSCRIPTION_EVENT_SINK_INPUT|PA_SUBSCRIPTION_EVENT_NEW))
        return;

    if (!(si = pa_idxset_get_by_index(c->sink_inputs, idx)))
        return;

    if (!si->name)
        return;

    for (r = u->rules; r; r = r->next) {
        if (!regexec(&r->regex, si->name, 0, NULL, 0)) {
            pa_cvolume cv;
            pa_log_debug("changing volume of sink input '%s' to 0x%03x", si->name, r->volume);
            pa_cvolume_set(&cv, si->sample_spec.channels, r->volume);
            pa_sink_input_set_volume(si, &cv);
        }
    }
}

int pa__init(pa_module*m) {
    pa_modargs *ma = NULL;
    struct userdata *u;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments");
        goto fail;
    }

    u = pa_xnew(struct userdata, 1);
    u->rules = NULL;
    u->subscription = NULL;
    m->userdata = u;

    if (load_rules(u, pa_modargs_get_value(ma, "table", NULL)) < 0)
        goto fail;

    u->subscription = pa_subscription_new(m->core, PA_SUBSCRIPTION_MASK_SINK_INPUT, callback, u);

    pa_modargs_free(ma);
    return 0;

fail:
    pa__done(m);

    if (ma)
        pa_modargs_free(ma);
    return  -1;
}

void pa__done(pa_module*m) {
    struct userdata* u;
    struct rule *r, *n;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->subscription)
        pa_subscription_free(u->subscription);

    for (r = u->rules; r; r = n) {
        n = r->next;

        regfree(&r->regex);
        pa_xfree(r);
    }

    pa_xfree(u);
}
