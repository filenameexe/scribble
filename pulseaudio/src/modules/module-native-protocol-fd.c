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
#include <unistd.h>

#include <pulsecore/module.h>
#include <pulsecore/macro.h>
#include <pulsecore/iochannel.h>
#include <pulsecore/modargs.h>
#include <pulsecore/protocol-native.h>
#include <pulsecore/log.h>

#include "module-native-protocol-fd-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering");
PA_MODULE_DESCRIPTION("Native protocol autospawn helper");
PA_MODULE_VERSION(PACKAGE_VERSION);
PA_MODULE_LOAD_ONCE(TRUE);

static const char* const valid_modargs[] = {
    "fd",
    "public",
    "cookie",
    NULL,
};

int pa__init(pa_module*m) {
    pa_iochannel *io;
    pa_modargs *ma;
    int fd, r = -1;

    pa_assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log("Failed to parse module arguments.");
        goto finish;
    }

    if (pa_modargs_get_value_s32(ma, "fd", &fd) < 0) {
        pa_log("Invalid file descriptor.");
        goto finish;
    }

    io = pa_iochannel_new(m->core->mainloop, fd, fd);

    if (!(m->userdata = pa_protocol_native_new_iochannel(m->core, io, m, ma))) {
        pa_iochannel_free(io);
        goto finish;
    }

    r = 0;

finish:
    if (ma)
        pa_modargs_free(ma);

    return r;
}

void pa__done(pa_module*m) {
    pa_assert(m);

    pa_protocol_native_free(m->userdata);
}
