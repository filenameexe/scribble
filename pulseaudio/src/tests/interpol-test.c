/* $Id$ */

/***
  This file is part of PulseAudio.

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

#include <signal.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <getopt.h>
#include <math.h>

#include <pulse/pulseaudio.h>
#include <pulse/mainloop.h>

#include <pulsecore/thread.h>

static pa_context *context = NULL;
static pa_stream *stream = NULL;
static pa_mainloop_api *mainloop_api = NULL;

static void stream_write_cb(pa_stream *p, size_t length, void *userdata) {

    /* Just some silence */
    pa_stream_write(p, pa_xmalloc0(length), length, pa_xfree, 0, PA_SEEK_RELATIVE);
}

/* This is called whenever the context status changes */
static void context_state_callback(pa_context *c, void *userdata) {
    assert(c);

    switch (pa_context_get_state(c)) {
        case PA_CONTEXT_CONNECTING:
        case PA_CONTEXT_AUTHORIZING:
        case PA_CONTEXT_SETTING_NAME:
            break;

        case PA_CONTEXT_READY: {

            static const pa_sample_spec ss = {
                .format = PA_SAMPLE_S16LE,
                .rate = 44100,
                .channels = 1
            };

            fprintf(stderr, "Connection established.\n");

            stream = pa_stream_new(c, "interpol-test", &ss, NULL);
            assert(stream);

            pa_stream_connect_playback(stream, NULL, NULL, PA_STREAM_INTERPOLATE_TIMING|PA_STREAM_AUTO_TIMING_UPDATE, NULL, NULL);
            pa_stream_set_write_callback(stream, stream_write_cb, NULL);

            break;
        }

        case PA_CONTEXT_TERMINATED:
            break;

        case PA_CONTEXT_FAILED:
        default:
            fprintf(stderr, "Context error: %s\n", pa_strerror(pa_context_errno(c)));
            abort();
    }
}

int main(int argc, char *argv[]) {
    pa_threaded_mainloop* m = NULL;
    int k, r;
    struct timeval start, last_info = { 0, 0 };
    pa_usec_t old_t = 0, old_rtc = 0;

    /* Set up a new main loop */
    m = pa_threaded_mainloop_new();
    assert(m);

    mainloop_api = pa_threaded_mainloop_get_api(m);

    context = pa_context_new(mainloop_api, argv[0]);
    assert(context);

    pa_context_set_state_callback(context, context_state_callback, NULL);

    r = pa_context_connect(context, NULL, 0, NULL);
    assert(r >= 0);

    pa_gettimeofday(&start);

    pa_threaded_mainloop_start(m);

    for (k = 0; k < 5000; k++) {
        int success = 0, changed = 0;
        pa_usec_t t, rtc;
        struct timeval now, tv;

        pa_threaded_mainloop_lock(m);

        if (stream) {
            const pa_timing_info *info;

            if (pa_stream_get_time(stream, &t) >= 0)
                success = 1;

            if ((info = pa_stream_get_timing_info(stream)))
                if (last_info.tv_usec != info->timestamp.tv_usec || last_info.tv_sec != info->timestamp.tv_sec) {
                    changed = 1;
                    last_info = info->timestamp;
                }
        }

        pa_threaded_mainloop_unlock(m);

        if (success) {
            pa_gettimeofday(&now);

            rtc = pa_timeval_diff(&now, &start);
            printf("%i\t%llu\t%llu\t%llu\t%llu\t%u\n", k, (unsigned long long) rtc, (unsigned long long) t, (unsigned long long) (rtc-old_rtc), (unsigned long long) (t-old_t), changed);
            old_t = t;
            old_rtc = rtc;
        }

        /* Spin loop, ugly but normal usleep() is just too badly grained */

        tv = now;
        while (pa_timeval_diff(pa_gettimeofday(&now), &tv) < 1000)
            pa_thread_yield();
    }

    if (m)
        pa_threaded_mainloop_stop(m);

    if (stream) {
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
    }

    if (context) {
        pa_context_disconnect(context);
        pa_context_unref(context);
    }

    if (m)
        pa_threaded_mainloop_free(m);

    return 0;
}
