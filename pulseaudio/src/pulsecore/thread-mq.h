#ifndef foopulsethreadmqhfoo
#define foopulsethreadmqhfoo

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

#include <pulse/mainloop-api.h>
#include <pulsecore/asyncmsgq.h>

/* Two way communication between a thread and a mainloop. Before the
 * thread is started a pa_pthread_mq should be initialized and than
 * attached to the thread using pa_thread_mq_install(). */

typedef struct pa_thread_mq {
    pa_mainloop_api *mainloop;
    pa_asyncmsgq *inq, *outq;
    pa_io_event *io_event;
} pa_thread_mq;

void pa_thread_mq_init(pa_thread_mq *q, pa_mainloop_api *mainloop);
void pa_thread_mq_done(pa_thread_mq *q);

/* Install the specified pa_thread_mq object for the current thread */
void pa_thread_mq_install(pa_thread_mq *q);

/* Return the pa_thread_mq object that is set for the current thread */
pa_thread_mq *pa_thread_mq_get(void);

#endif
