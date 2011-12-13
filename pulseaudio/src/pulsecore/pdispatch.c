/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/native-common.h>
#include <pulsecore/llist.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/flist.h>

#include "pdispatch.h"

/*#define DEBUG_OPCODES */

#ifdef DEBUG_OPCODES

static const char *command_names[PA_COMMAND_MAX] = {
    [PA_COMMAND_ERROR] = "ERROR",
    [PA_COMMAND_TIMEOUT] = "TIMEOUT",
    [PA_COMMAND_REPLY] = "REPLY",
    [PA_COMMAND_CREATE_PLAYBACK_STREAM] = "CREATE_PLAYBACK_STREAM",
    [PA_COMMAND_DELETE_PLAYBACK_STREAM] = "DELETE_PLAYBACK_STREAM",
    [PA_COMMAND_CREATE_RECORD_STREAM] = "CREATE_RECORD_STREAM",
    [PA_COMMAND_DELETE_RECORD_STREAM] = "DELETE_RECORD_STREAM",
    [PA_COMMAND_AUTH] = "AUTH",
    [PA_COMMAND_REQUEST] = "REQUEST",
    [PA_COMMAND_EXIT] = "EXIT",
    [PA_COMMAND_SET_CLIENT_NAME] = "SET_CLIENT_NAME",
    [PA_COMMAND_LOOKUP_SINK] = "LOOKUP_SINK",
    [PA_COMMAND_LOOKUP_SOURCE] = "LOOKUP_SOURCE",
    [PA_COMMAND_DRAIN_PLAYBACK_STREAM] = "DRAIN_PLAYBACK_STREAM",
    [PA_COMMAND_PLAYBACK_STREAM_KILLED] = "PLAYBACK_STREAM_KILLED",
    [PA_COMMAND_RECORD_STREAM_KILLED] = "RECORD_STREAM_KILLED",
    [PA_COMMAND_STAT] = "STAT",
    [PA_COMMAND_GET_PLAYBACK_LATENCY] = "PLAYBACK_LATENCY",
    [PA_COMMAND_CREATE_UPLOAD_STREAM] = "CREATE_UPLOAD_STREAM",
    [PA_COMMAND_DELETE_UPLOAD_STREAM] = "DELETE_UPLOAD_STREAM",
    [PA_COMMAND_FINISH_UPLOAD_STREAM] = "FINISH_UPLOAD_STREAM",
    [PA_COMMAND_PLAY_SAMPLE] = "PLAY_SAMPLE",
    [PA_COMMAND_REMOVE_SAMPLE] = "REMOVE_SAMPLE",
    [PA_COMMAND_GET_SERVER_INFO] = "GET_SERVER_INFO",
    [PA_COMMAND_GET_SINK_INFO] = "GET_SINK_INFO",
    [PA_COMMAND_GET_SINK_INFO_LIST] = "GET_SINK_INFO_LIST",
    [PA_COMMAND_GET_SOURCE_INFO] = "GET_SOURCE_INFO",
    [PA_COMMAND_GET_SOURCE_INFO_LIST] = "GET_SOURCE_INFO_LIST",
    [PA_COMMAND_GET_MODULE_INFO] = "GET_MODULE_INFO",
    [PA_COMMAND_GET_MODULE_INFO_LIST] = "GET_MODULE_INFO_LIST",
    [PA_COMMAND_GET_CLIENT_INFO] = "GET_CLIENT_INFO",
    [PA_COMMAND_GET_CLIENT_INFO_LIST] = "GET_CLIENT_INFO_LIST",
    [PA_COMMAND_GET_SAMPLE_INFO] = "GET_SAMPLE_INFO",
    [PA_COMMAND_GET_SAMPLE_INFO_LIST] = "GET_SAMPLE_INFO_LIST",
    [PA_COMMAND_GET_SINK_INPUT_INFO] = "GET_SINK_INPUT_INFO",
    [PA_COMMAND_GET_SINK_INPUT_INFO_LIST] = "GET_SINK_INPUT_INFO_LIST",
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO] = "GET_SOURCE_OUTPUT_INFO",
    [PA_COMMAND_GET_SOURCE_OUTPUT_INFO_LIST] = "GET_SOURCE_OUTPUT_INFO_LIST",
    [PA_COMMAND_SUBSCRIBE] = "SUBSCRIBE",
    [PA_COMMAND_SUBSCRIBE_EVENT] = "SUBSCRIBE_EVENT",
    [PA_COMMAND_SET_SINK_VOLUME] = "SET_SINK_VOLUME",
    [PA_COMMAND_SET_SINK_INPUT_VOLUME] = "SET_SINK_INPUT_VOLUME",
    [PA_COMMAND_SET_SOURCE_VOLUME] = "SET_SOURCE_VOLME",
    [PA_COMMAND_TRIGGER_PLAYBACK_STREAM] = "TRIGGER_PLAYBACK_STREAM",
    [PA_COMMAND_FLUSH_PLAYBACK_STREAM] = "FLUSH_PLAYBACK_STREAM",
    [PA_COMMAND_CORK_PLAYBACK_STREAM] = "CORK_PLAYBACK_STREAM",
    [PA_COMMAND_GET_AUTOLOAD_INFO] = "GET_AUTOLOAD_INFO",
    [PA_COMMAND_GET_AUTOLOAD_INFO_LIST] = "GET_AUTOLOAD_INFO_LIST",
};

#endif

PA_STATIC_FLIST_DECLARE(reply_infos, 0, pa_xfree);

struct reply_info {
    pa_pdispatch *pdispatch;
    PA_LLIST_FIELDS(struct reply_info);
    pa_pdispatch_cb_t callback;
    void *userdata;
    pa_free_cb_t free_cb;
    uint32_t tag;
    pa_time_event *time_event;
};

struct pa_pdispatch {
    PA_REFCNT_DECLARE;
    pa_mainloop_api *mainloop;
    const pa_pdispatch_cb_t *callback_table;
    unsigned n_commands;
    PA_LLIST_HEAD(struct reply_info, replies);
    pa_pdispatch_drain_callback drain_callback;
    void *drain_userdata;
    const pa_creds *creds;
};

static void reply_info_free(struct reply_info *r) {
    pa_assert(r);
    pa_assert(r->pdispatch);
    pa_assert(r->pdispatch->mainloop);

    if (r->time_event)
        r->pdispatch->mainloop->time_free(r->time_event);

    PA_LLIST_REMOVE(struct reply_info, r->pdispatch->replies, r);

    if (pa_flist_push(PA_STATIC_FLIST_GET(reply_infos), r) < 0)
        pa_xfree(r);
}

pa_pdispatch* pa_pdispatch_new(pa_mainloop_api *mainloop, const pa_pdispatch_cb_t*table, unsigned entries) {
    pa_pdispatch *pd;
    pa_assert(mainloop);

    pa_assert((entries && table) || (!entries && !table));

    pd = pa_xnew(pa_pdispatch, 1);
    PA_REFCNT_INIT(pd);
    pd->mainloop = mainloop;
    pd->callback_table = table;
    pd->n_commands = entries;
    PA_LLIST_HEAD_INIT(struct reply_info, pd->replies);
    pd->drain_callback = NULL;
    pd->drain_userdata = NULL;
    pd->creds = NULL;

    return pd;
}

static void pdispatch_free(pa_pdispatch *pd) {
    pa_assert(pd);

    while (pd->replies) {
        if (pd->replies->free_cb)
            pd->replies->free_cb(pd->replies->userdata);

        reply_info_free(pd->replies);
    }

    pa_xfree(pd);
}

static void run_action(pa_pdispatch *pd, struct reply_info *r, uint32_t command, pa_tagstruct *ts) {
    pa_pdispatch_cb_t callback;
    void *userdata;
    uint32_t tag;
    pa_assert(r);

    pa_pdispatch_ref(pd);

    callback = r->callback;
    userdata = r->userdata;
    tag = r->tag;

    reply_info_free(r);

    callback(pd, command, tag, ts, userdata);

    if (pd->drain_callback && !pa_pdispatch_is_pending(pd))
        pd->drain_callback(pd, pd->drain_userdata);

    pa_pdispatch_unref(pd);
}

int pa_pdispatch_run(pa_pdispatch *pd, pa_packet*packet, const pa_creds *creds, void *userdata) {
    uint32_t tag, command;
    pa_tagstruct *ts = NULL;
    int ret = -1;

    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);
    pa_assert(packet);
    pa_assert(PA_REFCNT_VALUE(packet) >= 1);
    pa_assert(packet->data);

    pa_pdispatch_ref(pd);

    if (packet->length <= 8)
        goto finish;

    ts = pa_tagstruct_new(packet->data, packet->length);

    if (pa_tagstruct_getu32(ts, &command) < 0 ||
        pa_tagstruct_getu32(ts, &tag) < 0)
        goto finish;

#ifdef DEBUG_OPCODES
{
    char t[256];
    char const *p;
    if (!(p = command_names[command]))
        pa_snprintf((char*) (p = t), sizeof(t), "%u", command);

    pa_log("Recieved opcode <%s>", p);
}
#endif

    pd->creds = creds;

    if (command == PA_COMMAND_ERROR || command == PA_COMMAND_REPLY) {
        struct reply_info *r;

        for (r = pd->replies; r; r = r->next)
            if (r->tag == tag)
                break;

        if (r)
            run_action(pd, r, command, ts);

    } else if (pd->callback_table && (command < pd->n_commands) && pd->callback_table[command]) {
        const pa_pdispatch_cb_t *c = pd->callback_table+command;

        (*c)(pd, command, tag, ts, userdata);
    } else {
        pa_log("Recieved unsupported command %u", command);
        goto finish;
    }

    ret = 0;

finish:
    pd->creds = NULL;

    if (ts)
        pa_tagstruct_free(ts);

    pa_pdispatch_unref(pd);

    return ret;
}

static void timeout_callback(pa_mainloop_api*m, pa_time_event*e, PA_GCC_UNUSED const struct timeval *tv, void *userdata) {
    struct reply_info*r = userdata;

    pa_assert(r);
    pa_assert(r->time_event == e);
    pa_assert(r->pdispatch);
    pa_assert(r->pdispatch->mainloop == m);
    pa_assert(r->callback);

    run_action(r->pdispatch, r, PA_COMMAND_TIMEOUT, NULL);
}

void pa_pdispatch_register_reply(pa_pdispatch *pd, uint32_t tag, int timeout, pa_pdispatch_cb_t cb, void *userdata, pa_free_cb_t free_cb) {
    struct reply_info *r;
    struct timeval tv;

    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);
    pa_assert(cb);

    if (!(r = pa_flist_pop(PA_STATIC_FLIST_GET(reply_infos))))
        r = pa_xnew(struct reply_info, 1);

    r->pdispatch = pd;
    r->callback = cb;
    r->userdata = userdata;
    r->free_cb = free_cb;
    r->tag = tag;

    pa_gettimeofday(&tv);
    tv.tv_sec += timeout;

    pa_assert_se(r->time_event = pd->mainloop->time_new(pd->mainloop, &tv, timeout_callback, r));

    PA_LLIST_PREPEND(struct reply_info, pd->replies, r);
}

int pa_pdispatch_is_pending(pa_pdispatch *pd) {
    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);

    return !!pd->replies;
}

void pa_pdispatch_set_drain_callback(pa_pdispatch *pd, void (*cb)(pa_pdispatch *pd, void *userdata), void *userdata) {
    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);
    pa_assert(!cb || pa_pdispatch_is_pending(pd));

    pd->drain_callback = cb;
    pd->drain_userdata = userdata;
}

void pa_pdispatch_unregister_reply(pa_pdispatch *pd, void *userdata) {
    struct reply_info *r, *n;

    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);

    for (r = pd->replies; r; r = n) {
        n = r->next;

        if (r->userdata == userdata)
            reply_info_free(r);
    }
}

void pa_pdispatch_unref(pa_pdispatch *pd) {
    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);

    if (PA_REFCNT_DEC(pd) <= 0)
        pdispatch_free(pd);
}

pa_pdispatch* pa_pdispatch_ref(pa_pdispatch *pd) {
    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);

    PA_REFCNT_INC(pd);
    return pd;
}

const pa_creds * pa_pdispatch_creds(pa_pdispatch *pd) {
    pa_assert(pd);
    pa_assert(PA_REFCNT_VALUE(pd) >= 1);

    return pd->creds;
}
