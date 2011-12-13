
/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

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
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

#include <pulse/timeval.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-error.h>
#include <pulsecore/module.h>
#include <pulsecore/llist.h>
#include <pulsecore/sink.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/memblockq.h>
#include <pulsecore/log.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>
#include <pulsecore/namereg.h>
#include <pulsecore/sample-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/atomic.h>
#include <pulsecore/rtclock.h>
#include <pulsecore/atomic.h>

#include "module-rtp-recv-symdef.h"

#include "rtp.h"
#include "sdp.h"
#include "sap.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Recieve data from a network via RTP/SAP/SDP");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(FALSE);
PA_MODULE_USAGE(
        "sink=<name of the sink> "
        "sap_address=<multicast address to listen on> "
);

#define SAP_PORT 9875
#define DEFAULT_SAP_ADDRESS "224.0.0.56"
#define MEMBLOCKQ_MAXLENGTH (1024*170)
#define MAX_SESSIONS 16
#define DEATH_TIMEOUT 20

static const char* const valid_modargs[] = {
    "sink",
    "sap_address",
    NULL
};

struct session {
    struct userdata *userdata;
    PA_LLIST_FIELDS(struct session);

    pa_sink_input *sink_input;
    pa_memblockq *memblockq;

    pa_bool_t first_packet;
    uint32_t ssrc;
    uint32_t offset;

    struct pa_sdp_info sdp_info;

    pa_rtp_context rtp_context;

    pa_rtpoll_item *rtpoll_item;

    pa_atomic_t timestamp;
};

struct userdata {
    pa_module *module;

    pa_sap_context sap_context;
    pa_io_event* sap_event;

    pa_time_event *check_death_event;

    char *sink_name;

    PA_LLIST_HEAD(struct session, sessions);
    pa_hashmap *by_origin;
    int n_sessions;
};

static void session_free(struct session *s);

/* Called from I/O thread context */
static int sink_input_process_msg(pa_msgobject *o, int code, void *data, int64_t offset, pa_memchunk *chunk) {
    struct session *s = PA_SINK_INPUT(o)->userdata;

    switch (code) {
        case PA_SINK_INPUT_MESSAGE_GET_LATENCY:
            *((pa_usec_t*) data) = pa_bytes_to_usec(pa_memblockq_get_length(s->memblockq), &s->sink_input->sample_spec);

            /* Fall through, the default handler will add in the extra
             * latency added by the resampler */
            break;
    }

    return pa_sink_input_process_msg(o, code, data, offset, chunk);
}

/* Called from I/O thread context */
static int sink_input_peek(pa_sink_input *i, size_t length, pa_memchunk *chunk) {
    struct session *s;
    pa_sink_input_assert_ref(i);
    pa_assert_se(s = i->userdata);

    return pa_memblockq_peek(s->memblockq, chunk);
}

/* Called from I/O thread context */
static void sink_input_drop(pa_sink_input *i, size_t length) {
    struct session *s;
    pa_sink_input_assert_ref(i);
    pa_assert_se(s = i->userdata);

    pa_memblockq_drop(s->memblockq, length);
}

/* Called from main context */
static void sink_input_kill(pa_sink_input* i) {
    struct session *s;
    pa_sink_input_assert_ref(i);
    pa_assert_se(s = i->userdata);

    session_free(s);
}

/* Called from I/O thread context */
static int rtpoll_work_cb(pa_rtpoll_item *i) {
    pa_memchunk chunk;
    int64_t k, j, delta;
    struct timeval now;
    struct session *s;
    struct pollfd *p;

    pa_assert_se(s = pa_rtpoll_item_get_userdata(i));

    p = pa_rtpoll_item_get_pollfd(i, NULL);

    if (p->revents & (POLLERR|POLLNVAL|POLLHUP|POLLOUT)) {
        pa_log("poll() signalled bad revents.");
        return -1;
    }

    if ((p->revents & POLLIN) == 0)
        return 0;

    p->revents = 0;

    if (pa_rtp_recv(&s->rtp_context, &chunk, s->userdata->module->core->mempool) < 0)
        return 0;

    if (s->sdp_info.payload != s->rtp_context.payload) {
        pa_memblock_unref(chunk.memblock);
        return 0;
    }

    if (!s->first_packet) {
        s->first_packet = TRUE;

        s->ssrc = s->rtp_context.ssrc;
        s->offset = s->rtp_context.timestamp;

        if (s->ssrc == s->userdata->module->core->cookie)
            pa_log_warn("Detected RTP packet loop!");
    } else {
        if (s->ssrc != s->rtp_context.ssrc) {
            pa_memblock_unref(chunk.memblock);
            return 0;
        }
    }

    /* Check wheter there was a timestamp overflow */
    k = (int64_t) s->rtp_context.timestamp - (int64_t) s->offset;
    j = (int64_t) 0x100000000LL - (int64_t) s->offset + (int64_t) s->rtp_context.timestamp;

    if ((k < 0 ? -k : k) < (j < 0 ? -j : j))
        delta = k;
    else
        delta = j;

    pa_memblockq_seek(s->memblockq, delta * s->rtp_context.frame_size, PA_SEEK_RELATIVE);

    if (pa_memblockq_push(s->memblockq, &chunk) < 0) {
        /* queue overflow, let's flush it and try again */
        pa_memblockq_flush(s->memblockq);
        pa_memblockq_push(s->memblockq, &chunk);
    }

    /* The next timestamp we expect */
    s->offset = s->rtp_context.timestamp + (chunk.length / s->rtp_context.frame_size);

    pa_memblock_unref(chunk.memblock);

    pa_rtclock_get(&now);
    pa_atomic_store(&s->timestamp, now.tv_sec);

    return 1;
}

/* Called from I/O thread context */
static void sink_input_attach(pa_sink_input *i) {
    struct session *s;
    struct pollfd *p;

    pa_sink_input_assert_ref(i);
    pa_assert_se(s = i->userdata);

    pa_assert(!s->rtpoll_item);
    s->rtpoll_item = pa_rtpoll_item_new(i->sink->rtpoll, PA_RTPOLL_LATE, 1);

    p = pa_rtpoll_item_get_pollfd(s->rtpoll_item, NULL);
    p->fd = s->rtp_context.fd;
    p->events = POLLIN;
    p->revents = 0;

    pa_rtpoll_item_set_work_callback(s->rtpoll_item, rtpoll_work_cb);
    pa_rtpoll_item_set_userdata(s->rtpoll_item, s);
}

/* Called from I/O thread context */
static void sink_input_detach(pa_sink_input *i) {
    struct session *s;
    pa_sink_input_assert_ref(i);
    pa_assert_se(s = i->userdata);

    pa_assert(s->rtpoll_item);
    pa_rtpoll_item_free(s->rtpoll_item);
    s->rtpoll_item = NULL;
}

static int mcast_socket(const struct sockaddr* sa, socklen_t salen) {
    int af, fd = -1, r, one;

    pa_assert(sa);
    pa_assert(salen > 0);

    af = sa->sa_family;
    if ((fd = socket(af, SOCK_DGRAM, 0)) < 0) {
        pa_log("Failed to create socket: %s", pa_cstrerror(errno));
        goto fail;
    }

    one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) < 0) {
        pa_log("SO_REUSEADDR failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (af == AF_INET) {
        struct ip_mreq mr4;
        memset(&mr4, 0, sizeof(mr4));
        mr4.imr_multiaddr = ((const struct sockaddr_in*) sa)->sin_addr;
        r = setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mr4, sizeof(mr4));
    } else {
        struct ipv6_mreq mr6;
        memset(&mr6, 0, sizeof(mr6));
        mr6.ipv6mr_multiaddr = ((const struct sockaddr_in6*) sa)->sin6_addr;
        r = setsockopt(fd, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mr6, sizeof(mr6));
    }

    if (r < 0) {
        pa_log_info("Joining mcast group failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (bind(fd, sa, salen) < 0) {
        pa_log("bind() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    return fd;

fail:
    if (fd >= 0)
        close(fd);

    return -1;
}

static struct session *session_new(struct userdata *u, const pa_sdp_info *sdp_info) {
    struct session *s = NULL;
    char *c;
    pa_sink *sink;
    int fd = -1;
    pa_memblock *silence;
    pa_sink_input_new_data data;
    struct timeval now;

    pa_assert(u);
    pa_assert(sdp_info);

    if (u->n_sessions >= MAX_SESSIONS) {
        pa_log("Session limit reached.");
        goto fail;
    }

    if (!(sink = pa_namereg_get(u->module->core, u->sink_name, PA_NAMEREG_SINK, 1))) {
        pa_log("Sink does not exist.");
        goto fail;
    }

    s = pa_xnew0(struct session, 1);
    s->userdata = u;
    s->first_packet = FALSE;
    s->sdp_info = *sdp_info;
    s->rtpoll_item = NULL;

    pa_rtclock_get(&now);
    pa_atomic_store(&s->timestamp, now.tv_sec);

    if ((fd = mcast_socket((const struct sockaddr*) &sdp_info->sa, sdp_info->salen)) < 0)
        goto fail;

    c = pa_sprintf_malloc("RTP Stream%s%s%s",
                          sdp_info->session_name ? " (" : "",
                          sdp_info->session_name ? sdp_info->session_name : "",
                          sdp_info->session_name ? ")" : "");

    pa_sink_input_new_data_init(&data);
    data.sink = sink;
    data.driver = __FILE__;
    data.name = c;
    data.module = u->module;
    pa_sink_input_new_data_set_sample_spec(&data, &sdp_info->sample_spec);

    s->sink_input = pa_sink_input_new(u->module->core, &data, 0);
    pa_xfree(c);

    if (!s->sink_input) {
        pa_log("Failed to create sink input.");
        goto fail;
    }

    s->sink_input->userdata = s;

    s->sink_input->parent.process_msg = sink_input_process_msg;
    s->sink_input->peek = sink_input_peek;
    s->sink_input->drop = sink_input_drop;
    s->sink_input->kill = sink_input_kill;
    s->sink_input->attach = sink_input_attach;
    s->sink_input->detach = sink_input_detach;

    silence = pa_silence_memblock_new(
            s->userdata->module->core->mempool,
            &s->sink_input->sample_spec,
            pa_frame_align(pa_bytes_per_second(&s->sink_input->sample_spec)/128, &s->sink_input->sample_spec));

    s->memblockq = pa_memblockq_new(
            0,
            MEMBLOCKQ_MAXLENGTH,
            MEMBLOCKQ_MAXLENGTH,
            pa_frame_size(&s->sink_input->sample_spec),
            pa_bytes_per_second(&s->sink_input->sample_spec)/10+1,
            0,
            silence);

    pa_memblock_unref(silence);

    pa_rtp_context_init_recv(&s->rtp_context, fd, pa_frame_size(&s->sdp_info.sample_spec));

    pa_hashmap_put(s->userdata->by_origin, s->sdp_info.origin, s);
    u->n_sessions++;
    PA_LLIST_PREPEND(struct session, s->userdata->sessions, s);

    pa_sink_input_put(s->sink_input);

    pa_log_info("New session '%s'", s->sdp_info.session_name);

    return s;

fail:
    pa_xfree(s);

    if (fd >= 0)
        pa_close(fd);

    return NULL;
}

static void session_free(struct session *s) {
    pa_assert(s);

    pa_log_info("Freeing session '%s'", s->sdp_info.session_name);

    pa_sink_input_unlink(s->sink_input);
    pa_sink_input_unref(s->sink_input);

    PA_LLIST_REMOVE(struct session, s->userdata->sessions, s);
    pa_assert(s->userdata->n_sessions >= 1);
    s->userdata->n_sessions--;
    pa_hashmap_remove(s->userdata->by_origin, s->sdp_info.origin);

    pa_memblockq_free(s->memblockq);
    pa_sdp_info_destroy(&s->sdp_info);
    pa_rtp_context_destroy(&s->rtp_context);

    pa_xfree(s);
}

static void sap_event_cb(pa_mainloop_api *m, pa_io_event *e, int fd, pa_io_event_flags_t flags, void *userdata) {
    struct userdata *u = userdata;
    int goodbye;
    pa_sdp_info info;
    struct session *s;

    pa_assert(m);
    pa_assert(e);
    pa_assert(u);
    pa_assert(fd == u->sap_context.fd);
    pa_assert(flags == PA_IO_EVENT_INPUT);

    if (pa_sap_recv(&u->sap_context, &goodbye) < 0)
        return;

    if (!pa_sdp_parse(u->sap_context.sdp_data, &info, goodbye))
        return;

    if (goodbye) {

        if ((s = pa_hashmap_get(u->by_origin, info.origin)))
            session_free(s);

        pa_sdp_info_destroy(&info);
    } else {

        if (!(s = pa_hashmap_get(u->by_origin, info.origin))) {
            if (!(s = session_new(u, &info)))
                pa_sdp_info_destroy(&info);

        } else {
            struct timeval now;
            pa_rtclock_get(&now);
            pa_atomic_store(&s->timestamp, now.tv_sec);

            pa_sdp_info_destroy(&info);
        }
    }
}

static void check_death_event_cb(pa_mainloop_api *m, pa_time_event *t, const struct timeval *ptv, void *userdata) {
    struct session *s, *n;
    struct userdata *u = userdata;
    struct timeval now;
    struct timeval tv;

    pa_assert(m);
    pa_assert(t);
    pa_assert(ptv);
    pa_assert(u);

    pa_rtclock_get(&now);

    pa_log_debug("Checking for dead streams ...");

    for (s = u->sessions; s; s = n) {
        int k;
        n = s->next;

        k = pa_atomic_load(&s->timestamp);

        if (k + DEATH_TIMEOUT < now.tv_sec)
            session_free(s);
    }

    /* Restart timer */
    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, DEATH_TIMEOUT*PA_USEC_PER_SEC);
    m->time_restart(t, &tv);
}

int pa__init(pa_module*m) {
    struct userdata *u;
    pa_modargs *ma = NULL;
    struct sockaddr_in sa4;
    struct sockaddr_in6 sa6;
    struct sockaddr *sa;
    socklen_t salen;
    const char *sap_address;
    int fd = -1;
    struct timeval tv;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("failed to parse module arguments");
        goto fail;
    }

    sap_address = pa_modargs_get_value(ma, "sap_address", DEFAULT_SAP_ADDRESS);

    if (inet_pton(AF_INET6, sap_address, &sa6.sin6_addr) > 0) {
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(SAP_PORT);
        sa = (struct sockaddr*) &sa6;
        salen = sizeof(sa6);
    } else if (inet_pton(AF_INET, sap_address, &sa4.sin_addr) > 0) {
        sa4.sin_family = AF_INET;
        sa4.sin_port = htons(SAP_PORT);
        sa = (struct sockaddr*) &sa4;
        salen = sizeof(sa4);
    } else {
        pa_log("Invalid SAP address '%s'", sap_address);
        goto fail;
    }

    if ((fd = mcast_socket(sa, salen)) < 0)
        goto fail;

    u = pa_xnew(struct userdata, 1);
    m->userdata = u;
    u->module = m;
    u->sink_name = pa_xstrdup(pa_modargs_get_value(ma, "sink", NULL));

    u->sap_event = m->core->mainloop->io_new(m->core->mainloop, fd, PA_IO_EVENT_INPUT, sap_event_cb, u);
    pa_sap_context_init_recv(&u->sap_context, fd);

    PA_LLIST_HEAD_INIT(struct session, u->sessions);
    u->n_sessions = 0;
    u->by_origin = pa_hashmap_new(pa_idxset_string_hash_func, pa_idxset_string_compare_func);

    pa_gettimeofday(&tv);
    pa_timeval_add(&tv, DEATH_TIMEOUT * PA_USEC_PER_SEC);
    u->check_death_event = m->core->mainloop->time_new(m->core->mainloop, &tv, check_death_event_cb, u);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);

    if (fd >= 0)
        pa_close(fd);

    return -1;
}

void pa__done(pa_module*m) {
    struct userdata *u;
    struct session *s;

    pa_assert(m);

    if (!(u = m->userdata))
        return;

    if (u->sap_event)
        m->core->mainloop->io_free(u->sap_event);

    if (u->check_death_event)
        m->core->mainloop->time_free(u->check_death_event);

    pa_sap_context_destroy(&u->sap_context);

    if (u->by_origin) {
        while ((s = pa_hashmap_get_first(u->by_origin)))
            session_free(s);

        pa_hashmap_free(u->by_origin, NULL, NULL);
    }

    pa_xfree(u->sink_name);
    pa_xfree(u);
}
