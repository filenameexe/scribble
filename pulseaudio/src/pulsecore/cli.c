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
#include <string.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>

#include <pulsecore/ioline.h>
#include <pulsecore/module.h>
#include <pulsecore/sink.h>
#include <pulsecore/source.h>
#include <pulsecore/client.h>
#include <pulsecore/sink-input.h>
#include <pulsecore/source-output.h>
#include <pulsecore/tokenizer.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/namereg.h>
#include <pulsecore/cli-text.h>
#include <pulsecore/cli-command.h>
#include <pulsecore/log.h>
#include <pulsecore/macro.h>

#include "cli.h"

#define PROMPT ">>> "

struct pa_cli {
    pa_core *core;
    pa_ioline *line;

    void (*eof_callback)(pa_cli *c, void *userdata);
    void *userdata;

    pa_client *client;

    pa_bool_t fail, kill_requested;
    int defer_kill;
};

static void line_callback(pa_ioline *line, const char *s, void *userdata);
static void client_kill(pa_client *c);

pa_cli* pa_cli_new(pa_core *core, pa_iochannel *io, pa_module *m) {
    char cname[256];
    pa_cli *c;
    pa_assert(io);

    c = pa_xnew(pa_cli, 1);
    c->core = core;
    pa_assert_se(c->line = pa_ioline_new(io));

    c->userdata = NULL;
    c->eof_callback = NULL;

    pa_iochannel_socket_peer_to_string(io, cname, sizeof(cname));
    pa_assert_se(c->client = pa_client_new(core, __FILE__, cname));
    c->client->kill = client_kill;
    c->client->userdata = c;
    c->client->owner = m;

    pa_ioline_set_callback(c->line, line_callback, c);
    pa_ioline_puts(c->line, "Welcome to PulseAudio! Use \"help\" for usage information.\n"PROMPT);

    c->fail = c->kill_requested = FALSE;
    c->defer_kill = 0;

    return c;
}

void pa_cli_free(pa_cli *c) {
    pa_assert(c);

    pa_ioline_close(c->line);
    pa_ioline_unref(c->line);
    pa_client_free(c->client);
    pa_xfree(c);
}

static void client_kill(pa_client *client) {
    pa_cli *c;

    pa_assert(client);
    pa_assert_se(c = client->userdata);

    pa_log_debug("CLI client killed.");
    if (c->defer_kill)
        c->kill_requested = TRUE;
    else {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    }
}

static void line_callback(pa_ioline *line, const char *s, void *userdata) {
    pa_strbuf *buf;
    pa_cli *c = userdata;
    char *p;

    pa_assert(line);
    pa_assert(c);

    if (!s) {
        pa_log_debug("CLI got EOF from user.");
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);

        return;
    }

    pa_assert_se(buf = pa_strbuf_new());
    c->defer_kill++;
    pa_cli_command_execute_line(c->core, s, buf, &c->fail);
    c->defer_kill--;
    pa_ioline_puts(line, p = pa_strbuf_tostring_free(buf));
    pa_xfree(p);

    if (c->kill_requested) {
        if (c->eof_callback)
            c->eof_callback(c, c->userdata);
    } else
        pa_ioline_puts(line, PROMPT);
}

void pa_cli_set_eof_callback(pa_cli *c, void (*cb)(pa_cli*c, void *userdata), void *userdata) {
    pa_assert(c);

    c->eof_callback = cb;
    c->userdata = userdata;
}
