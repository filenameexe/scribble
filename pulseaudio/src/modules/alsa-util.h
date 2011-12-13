#ifndef fooalsautilhfoo
#define fooalsautilhfoo

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

#include <asoundlib.h>

#include <pulse/sample.h>
#include <pulse/mainloop-api.h>

#include <pulse/channelmap.h>

typedef struct pa_alsa_fdlist pa_alsa_fdlist;

struct pa_alsa_fdlist *pa_alsa_fdlist_new(void);
void pa_alsa_fdlist_free(struct pa_alsa_fdlist *fdl);
int pa_alsa_fdlist_set_mixer(struct pa_alsa_fdlist *fdl, snd_mixer_t *mixer_handle, pa_mainloop_api* m);

int pa_alsa_set_hw_params(
        snd_pcm_t *pcm_handle,
        pa_sample_spec *ss,
        uint32_t *periods,
        snd_pcm_uframes_t *period_size,
        pa_bool_t *use_mmap,
        pa_bool_t require_exact_channel_number);

int pa_alsa_set_sw_params(snd_pcm_t *pcm);

int pa_alsa_prepare_mixer(snd_mixer_t *mixer, const char *dev);
snd_mixer_elem_t *pa_alsa_find_elem(snd_mixer_t *mixer, const char *name, const char *fallback);

snd_pcm_t *pa_alsa_open_by_device_id(
        const char *dev_id,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        pa_bool_t *use_mmap);

snd_pcm_t *pa_alsa_open_by_device_string(
        const char *device,
        char **dev,
        pa_sample_spec *ss,
        pa_channel_map* map,
        int mode,
        uint32_t *nfrags,
        snd_pcm_uframes_t *period_size,
        pa_bool_t *use_mmap);

int pa_alsa_calc_mixer_map(snd_mixer_elem_t *elem, const pa_channel_map *channel_map, snd_mixer_selem_channel_id_t mixer_map[], pa_bool_t playback);

#endif
