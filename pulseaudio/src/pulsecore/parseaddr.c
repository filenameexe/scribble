/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering

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

#include <string.h>
#include <stdlib.h>

#include <pulse/xmalloc.h>
#include <pulse/util.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "parseaddr.h"

/* Parse addresses in one of the following forms:
 *    HOSTNAME
 *    HOSTNAME:PORT
 *    [HOSTNAME]
 *    [HOSTNAME]:PORT
 *
 *  Return a newly allocated string of the hostname and fill in *ret_port if specified  */

static char *parse_host(const char *s, uint16_t *ret_port) {
    pa_assert(s);
    pa_assert(ret_port);

    if (*s == '[') {
        char *e;
        if (!(e = strchr(s+1, ']')))
            return NULL;

        if (e[1] == ':')
            *ret_port = atoi(e+2);
        else if (e[1] != 0)
            return NULL;

        return pa_xstrndup(s+1, e-s-1);
    } else {
        char *e;

        if (!(e = strrchr(s, ':')))
            return pa_xstrdup(s);

        *ret_port = atoi(e+1);
        return pa_xstrndup(s, e-s);
    }
}

int pa_parse_address(const char *name, pa_parsed_address *ret_p) {
    const char *p;

    pa_assert(name);
    pa_assert(ret_p);

    memset(ret_p, 0, sizeof(pa_parsed_address));
    ret_p->type = PA_PARSED_ADDRESS_TCP_AUTO;

    if (*name == '{') {
        char hn[256], *pfx;
        /* The URL starts with a host specification for detecting local connections */

        if (!pa_get_host_name(hn, sizeof(hn)))
            return -1;

        pfx = pa_sprintf_malloc("{%s}", hn);
        if (!pa_startswith(name, pfx)) {
            pa_xfree(pfx);
            /* Not local */
            return -1;
        }

        p = name + strlen(pfx);
        pa_xfree(pfx);
    } else
        p = name;

    if (*p == '/')
        ret_p->type = PA_PARSED_ADDRESS_UNIX;
    else if (pa_startswith(p, "unix:")) {
        ret_p->type = PA_PARSED_ADDRESS_UNIX;
        p += sizeof("unix:")-1;
    } else if (pa_startswith(p, "tcp:")) {
        ret_p->type = PA_PARSED_ADDRESS_TCP4;
        p += sizeof("tcp:")-1;
    } else if (pa_startswith(p, "tcp4:")) {
        ret_p->type = PA_PARSED_ADDRESS_TCP4;
        p += sizeof("tcp4:")-1;
    } else if (pa_startswith(p, "tcp6:")) {
        ret_p->type = PA_PARSED_ADDRESS_TCP6;
        p += sizeof("tcp6:")-1;
    }

    if (ret_p->type == PA_PARSED_ADDRESS_UNIX)
        ret_p->path_or_host = pa_xstrdup(p);
    else
        if (!(ret_p->path_or_host = parse_host(p, &ret_p->port)))
            return -1;

    return 0;
}
