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
  Lesser General Public License for more details

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
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>

#include <pulse/xmalloc.h>
#include <pulse/def.h>

#include <pulsecore/shm.h>
#include <pulsecore/log.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/semaphore.h>
#include <pulsecore/macro.h>
#include <pulsecore/flist.h>
#include <pulsecore/core-util.h>

#include "memblock.h"

#define PA_MEMPOOL_SLOTS_MAX 128
#define PA_MEMPOOL_SLOT_SIZE (16*1024)

#define PA_MEMEXPORT_SLOTS_MAX 128

#define PA_MEMIMPORT_SLOTS_MAX 128
#define PA_MEMIMPORT_SEGMENTS_MAX 16

struct pa_memblock {
    PA_REFCNT_DECLARE; /* the reference counter */
    pa_mempool *pool;

    pa_memblock_type_t type;
    int read_only; /* boolean */

    pa_atomic_ptr_t data;
    size_t length;

    pa_atomic_t n_acquired;
    pa_atomic_t please_signal;

    union {
        struct {
            /* If type == PA_MEMBLOCK_USER this points to a function for freeing this memory block */
            pa_free_cb_t free_cb;
        } user;

        struct  {
            uint32_t id;
            pa_memimport_segment *segment;
        } imported;
    } per_type;
};

struct pa_memimport_segment {
    pa_memimport *import;
    pa_shm memory;
    unsigned n_blocks;
};

struct pa_memimport {
    pa_mutex *mutex;

    pa_mempool *pool;
    pa_hashmap *segments;
    pa_hashmap *blocks;

    /* Called whenever an imported memory block is no longer
     * needed. */
    pa_memimport_release_cb_t release_cb;
    void *userdata;

    PA_LLIST_FIELDS(pa_memimport);
};

struct memexport_slot {
    PA_LLIST_FIELDS(struct memexport_slot);
    pa_memblock *block;
};

struct pa_memexport {
    pa_mutex *mutex;
    pa_mempool *pool;

    struct memexport_slot slots[PA_MEMEXPORT_SLOTS_MAX];

    PA_LLIST_HEAD(struct memexport_slot, free_slots);
    PA_LLIST_HEAD(struct memexport_slot, used_slots);
    unsigned n_init;

    /* Called whenever a client from which we imported a memory block
       which we in turn exported to another client dies and we need to
       revoke the memory block accordingly */
    pa_memexport_revoke_cb_t revoke_cb;
    void *userdata;

    PA_LLIST_FIELDS(pa_memexport);
};

struct mempool_slot {
    PA_LLIST_FIELDS(struct mempool_slot);
    /* the actual data follows immediately hereafter */
};

struct pa_mempool {
    pa_semaphore *semaphore;
    pa_mutex *mutex;

    pa_shm memory;
    size_t block_size;
    unsigned n_blocks;

    pa_atomic_t n_init;

    PA_LLIST_HEAD(pa_memimport, imports);
    PA_LLIST_HEAD(pa_memexport, exports);

    /* A list of free slots that may be reused */
    pa_flist *free_slots;

    pa_mempool_stat stat;
};

static void segment_detach(pa_memimport_segment *seg);

PA_STATIC_FLIST_DECLARE(unused_memblocks, 0, pa_xfree);

/* No lock necessary */
static void stat_add(pa_memblock*b) {
    pa_assert(b);
    pa_assert(b->pool);

    pa_atomic_inc(&b->pool->stat.n_allocated);
    pa_atomic_add(&b->pool->stat.allocated_size, b->length);

    pa_atomic_inc(&b->pool->stat.n_accumulated);
    pa_atomic_add(&b->pool->stat.accumulated_size, b->length);

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        pa_atomic_inc(&b->pool->stat.n_imported);
        pa_atomic_add(&b->pool->stat.imported_size, b->length);
    }

    pa_atomic_inc(&b->pool->stat.n_allocated_by_type[b->type]);
    pa_atomic_inc(&b->pool->stat.n_accumulated_by_type[b->type]);
}

/* No lock necessary */
static void stat_remove(pa_memblock *b) {
    pa_assert(b);
    pa_assert(b->pool);

    pa_assert(pa_atomic_load(&b->pool->stat.n_allocated) > 0);
    pa_assert(pa_atomic_load(&b->pool->stat.allocated_size) >= (int) b->length);

    pa_atomic_dec(&b->pool->stat.n_allocated);
    pa_atomic_sub(&b->pool->stat.allocated_size,  b->length);

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        pa_assert(pa_atomic_load(&b->pool->stat.n_imported) > 0);
        pa_assert(pa_atomic_load(&b->pool->stat.imported_size) >= (int) b->length);

        pa_atomic_dec(&b->pool->stat.n_imported);
        pa_atomic_sub(&b->pool->stat.imported_size, b->length);
    }

    pa_atomic_dec(&b->pool->stat.n_allocated_by_type[b->type]);
}

static pa_memblock *memblock_new_appended(pa_mempool *p, size_t length);

/* No lock necessary */
pa_memblock *pa_memblock_new(pa_mempool *p, size_t length) {
    pa_memblock *b;

    pa_assert(p);
    pa_assert(length > 0);

    if (!(b = pa_memblock_new_pool(p, length)))
        b = memblock_new_appended(p, length);

    return b;
}

/* No lock necessary */
static pa_memblock *memblock_new_appended(pa_mempool *p, size_t length) {
    pa_memblock *b;

    pa_assert(p);
    pa_assert(length > 0);

    /* If -1 is passed as length we choose the size for the caller. */

    if (length == (size_t) -1)
        length = p->block_size - PA_ALIGN(sizeof(struct mempool_slot)) - PA_ALIGN(sizeof(pa_memblock));

    b = pa_xmalloc(PA_ALIGN(sizeof(pa_memblock)) + length);
    PA_REFCNT_INIT(b);
    b->pool = p;
    b->type = PA_MEMBLOCK_APPENDED;
    b->read_only = 0;
    pa_atomic_ptr_store(&b->data, (uint8_t*) b + PA_ALIGN(sizeof(pa_memblock)));
    b->length = length;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);

    stat_add(b);
    return b;
}

/* No lock necessary */
static struct mempool_slot* mempool_allocate_slot(pa_mempool *p) {
    struct mempool_slot *slot;
    pa_assert(p);

    if (!(slot = pa_flist_pop(p->free_slots))) {
        int idx;

        /* The free list was empty, we have to allocate a new entry */

        if ((unsigned) (idx = pa_atomic_inc(&p->n_init)) >= p->n_blocks)
            pa_atomic_dec(&p->n_init);
        else
            slot = (struct mempool_slot*) ((uint8_t*) p->memory.ptr + (p->block_size * idx));

        if (!slot) {
            pa_log_debug("Pool full");
            pa_atomic_inc(&p->stat.n_pool_full);
            return NULL;
        }
    }

    return slot;
}

/* No lock necessary */
static void* mempool_slot_data(struct mempool_slot *slot) {
    pa_assert(slot);

    return (uint8_t*) slot + PA_ALIGN(sizeof(struct mempool_slot));
}

/* No lock necessary */
static unsigned mempool_slot_idx(pa_mempool *p, void *ptr) {
    pa_assert(p);

    pa_assert((uint8_t*) ptr >= (uint8_t*) p->memory.ptr);
    pa_assert((uint8_t*) ptr < (uint8_t*) p->memory.ptr + p->memory.size);

    return ((uint8_t*) ptr - (uint8_t*) p->memory.ptr) / p->block_size;
}

/* No lock necessary */
static struct mempool_slot* mempool_slot_by_ptr(pa_mempool *p, void *ptr) {
    unsigned idx;

    if ((idx = mempool_slot_idx(p, ptr)) == (unsigned) -1)
        return NULL;

    return (struct mempool_slot*) ((uint8_t*) p->memory.ptr + (idx * p->block_size));
}

/* No lock necessary */
pa_memblock *pa_memblock_new_pool(pa_mempool *p, size_t length) {
    pa_memblock *b = NULL;
    struct mempool_slot *slot;

    pa_assert(p);
    pa_assert(length > 0);

    /* If -1 is passed as length we choose the size for the caller: we
     * take the largest size that fits in one of our slots. */

    if (length == (size_t) -1)
        length = pa_mempool_block_size_max(p);

    if (p->block_size - PA_ALIGN(sizeof(struct mempool_slot)) >= PA_ALIGN(sizeof(pa_memblock)) + length) {

        if (!(slot = mempool_allocate_slot(p)))
            return NULL;

        b = mempool_slot_data(slot);
        b->type = PA_MEMBLOCK_POOL;
        pa_atomic_ptr_store(&b->data, (uint8_t*) b + PA_ALIGN(sizeof(pa_memblock)));

    } else if (p->block_size - PA_ALIGN(sizeof(struct mempool_slot)) >= length) {

        if (!(slot = mempool_allocate_slot(p)))
            return NULL;

        if (!(b = pa_flist_pop(PA_STATIC_FLIST_GET(unused_memblocks))))
            b = pa_xnew(pa_memblock, 1);

        b->type = PA_MEMBLOCK_POOL_EXTERNAL;
        pa_atomic_ptr_store(&b->data, mempool_slot_data(slot));

    } else {
        pa_log_debug("Memory block too large for pool: %lu > %lu", (unsigned long) length, (unsigned long) (p->block_size - PA_ALIGN(sizeof(struct mempool_slot))));
        pa_atomic_inc(&p->stat.n_too_large_for_pool);
        return NULL;
    }

    PA_REFCNT_INIT(b);
    b->pool = p;
    b->read_only = 0;
    b->length = length;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);

    stat_add(b);
    return b;
}

/* No lock necessary */
pa_memblock *pa_memblock_new_fixed(pa_mempool *p, void *d, size_t length, int read_only) {
    pa_memblock *b;

    pa_assert(p);
    pa_assert(d);
    pa_assert(length != (size_t) -1);
    pa_assert(length > 0);

    if (!(b = pa_flist_pop(PA_STATIC_FLIST_GET(unused_memblocks))))
        b = pa_xnew(pa_memblock, 1);
    PA_REFCNT_INIT(b);
    b->pool = p;
    b->type = PA_MEMBLOCK_FIXED;
    b->read_only = read_only;
    pa_atomic_ptr_store(&b->data, d);
    b->length = length;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);

    stat_add(b);
    return b;
}

/* No lock necessary */
pa_memblock *pa_memblock_new_user(pa_mempool *p, void *d, size_t length, void (*free_cb)(void *p), int read_only) {
    pa_memblock *b;

    pa_assert(p);
    pa_assert(d);
    pa_assert(length > 0);
    pa_assert(length != (size_t) -1);
    pa_assert(free_cb);

    if (!(b = pa_flist_pop(PA_STATIC_FLIST_GET(unused_memblocks))))
        b = pa_xnew(pa_memblock, 1);
    PA_REFCNT_INIT(b);
    b->pool = p;
    b->type = PA_MEMBLOCK_USER;
    b->read_only = read_only;
    pa_atomic_ptr_store(&b->data, d);
    b->length = length;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);

    b->per_type.user.free_cb = free_cb;

    stat_add(b);
    return b;
}

/* No lock necessary */
int pa_memblock_is_read_only(pa_memblock *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    return b->read_only && PA_REFCNT_VALUE(b) == 1;
}

/* No lock necessary */
int pa_memblock_ref_is_one(pa_memblock *b) {
    int r;

    pa_assert(b);

    r = PA_REFCNT_VALUE(b);
    pa_assert(r > 0);

    return r == 1;
}

/* No lock necessary */
void* pa_memblock_acquire(pa_memblock *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    pa_atomic_inc(&b->n_acquired);

    return pa_atomic_ptr_load(&b->data);
}

/* No lock necessary, in corner cases locks by its own */
void pa_memblock_release(pa_memblock *b) {
    int r;
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    r = pa_atomic_dec(&b->n_acquired);
    pa_assert(r >= 1);

    /* Signal a waiting thread that this memblock is no longer used */
    if (r == 1 && pa_atomic_load(&b->please_signal))
        pa_semaphore_post(b->pool->semaphore);
}

size_t pa_memblock_get_length(pa_memblock *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    return b->length;
}

pa_mempool* pa_memblock_get_pool(pa_memblock *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    return b->pool;
}

/* No lock necessary */
pa_memblock* pa_memblock_ref(pa_memblock*b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    PA_REFCNT_INC(b);
    return b;
}

static void memblock_free(pa_memblock *b) {
    pa_assert(b);

    pa_assert(pa_atomic_load(&b->n_acquired) == 0);

    stat_remove(b);

    switch (b->type) {
        case PA_MEMBLOCK_USER :
            pa_assert(b->per_type.user.free_cb);
            b->per_type.user.free_cb(pa_atomic_ptr_load(&b->data));

            /* Fall through */

        case PA_MEMBLOCK_FIXED:
        case PA_MEMBLOCK_APPENDED :
            if (pa_flist_push(PA_STATIC_FLIST_GET(unused_memblocks), b) < 0)
                pa_xfree(b);

            break;

        case PA_MEMBLOCK_IMPORTED : {
            pa_memimport_segment *segment;
            pa_memimport *import;

            /* FIXME! This should be implemented lock-free */

            segment = b->per_type.imported.segment;
            pa_assert(segment);
            import = segment->import;
            pa_assert(import);

            pa_mutex_lock(import->mutex);
            pa_hashmap_remove(import->blocks, PA_UINT32_TO_PTR(b->per_type.imported.id));
            if (-- segment->n_blocks <= 0)
                segment_detach(segment);

            pa_mutex_unlock(import->mutex);

            import->release_cb(import, b->per_type.imported.id, import->userdata);

            if (pa_flist_push(PA_STATIC_FLIST_GET(unused_memblocks), b) < 0)
                pa_xfree(b);
            break;
        }

        case PA_MEMBLOCK_POOL_EXTERNAL:
        case PA_MEMBLOCK_POOL: {
            struct mempool_slot *slot;
            int call_free;

            slot = mempool_slot_by_ptr(b->pool, pa_atomic_ptr_load(&b->data));
            pa_assert(slot);

            call_free = b->type == PA_MEMBLOCK_POOL_EXTERNAL;

            /* The free list dimensions should easily allow all slots
             * to fit in, hence try harder if pushing this slot into
             * the free list fails */
            while (pa_flist_push(b->pool->free_slots, slot) < 0)
                ;

            if (call_free)
                if (pa_flist_push(PA_STATIC_FLIST_GET(unused_memblocks), b) < 0)
                    pa_xfree(b);

            break;
        }

        case PA_MEMBLOCK_TYPE_MAX:
        default:
            pa_assert_not_reached();
    }
}

/* No lock necessary */
void pa_memblock_unref(pa_memblock*b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    if (PA_REFCNT_DEC(b) > 0)
        return;

    memblock_free(b);
}

/* Self locked */
static void memblock_wait(pa_memblock *b) {
    pa_assert(b);

    if (pa_atomic_load(&b->n_acquired) > 0) {
        /* We need to wait until all threads gave up access to the
         * memory block before we can go on. Unfortunately this means
         * that we have to lock and wait here. Sniff! */

        pa_atomic_inc(&b->please_signal);

        while (pa_atomic_load(&b->n_acquired) > 0)
            pa_semaphore_wait(b->pool->semaphore);

        pa_atomic_dec(&b->please_signal);
    }
}

/* No lock necessary. This function is not multiple caller safe! */
static void memblock_make_local(pa_memblock *b) {
    pa_assert(b);

    pa_atomic_dec(&b->pool->stat.n_allocated_by_type[b->type]);

    if (b->length <= b->pool->block_size - PA_ALIGN(sizeof(struct mempool_slot))) {
        struct mempool_slot *slot;

        if ((slot = mempool_allocate_slot(b->pool))) {
            void *new_data;
            /* We can move it into a local pool, perfect! */

            new_data = mempool_slot_data(slot);
            memcpy(new_data, pa_atomic_ptr_load(&b->data), b->length);
            pa_atomic_ptr_store(&b->data, new_data);

            b->type = PA_MEMBLOCK_POOL_EXTERNAL;
            b->read_only = 0;

            goto finish;
        }
    }

    /* Humm, not enough space in the pool, so lets allocate the memory with malloc() */
    b->per_type.user.free_cb = pa_xfree;
    pa_atomic_ptr_store(&b->data, pa_xmemdup(pa_atomic_ptr_load(&b->data), b->length));

    b->type = PA_MEMBLOCK_USER;
    b->read_only = 0;

finish:
    pa_atomic_inc(&b->pool->stat.n_allocated_by_type[b->type]);
    pa_atomic_inc(&b->pool->stat.n_accumulated_by_type[b->type]);
    memblock_wait(b);
}

/* No lock necessary. This function is not multiple caller safe*/
void pa_memblock_unref_fixed(pa_memblock *b) {
    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);
    pa_assert(b->type == PA_MEMBLOCK_FIXED);

    if (PA_REFCNT_VALUE(b) > 1)
        memblock_make_local(b);

    pa_memblock_unref(b);
}

/* No lock necessary. */
pa_memblock *pa_memblock_will_need(pa_memblock *b) {
    void *p;

    pa_assert(b);
    pa_assert(PA_REFCNT_VALUE(b) > 0);

    p = pa_memblock_acquire(b);
    pa_will_need(p, b->length);
    pa_memblock_release(b);

    return b;
}

/* Self-locked. This function is not multiple-caller safe */
static void memblock_replace_import(pa_memblock *b) {
    pa_memimport_segment *seg;

    pa_assert(b);
    pa_assert(b->type == PA_MEMBLOCK_IMPORTED);

    pa_assert(pa_atomic_load(&b->pool->stat.n_imported) > 0);
    pa_assert(pa_atomic_load(&b->pool->stat.imported_size) >= (int) b->length);
    pa_atomic_dec(&b->pool->stat.n_imported);
    pa_atomic_sub(&b->pool->stat.imported_size, b->length);

    seg = b->per_type.imported.segment;
    pa_assert(seg);
    pa_assert(seg->import);

    pa_mutex_lock(seg->import->mutex);

    pa_hashmap_remove(
            seg->import->blocks,
            PA_UINT32_TO_PTR(b->per_type.imported.id));

    memblock_make_local(b);

    if (-- seg->n_blocks <= 0) {
        pa_mutex_unlock(seg->import->mutex);
        segment_detach(seg);
    } else
        pa_mutex_unlock(seg->import->mutex);
}

pa_mempool* pa_mempool_new(int shared) {
    pa_mempool *p;

    p = pa_xnew(pa_mempool, 1);

    p->mutex = pa_mutex_new(TRUE, TRUE);
    p->semaphore = pa_semaphore_new(0);

    p->block_size = PA_PAGE_ALIGN(PA_MEMPOOL_SLOT_SIZE);
    if (p->block_size < PA_PAGE_SIZE)
        p->block_size = PA_PAGE_SIZE;

    p->n_blocks = PA_MEMPOOL_SLOTS_MAX;

    pa_assert(p->block_size > PA_ALIGN(sizeof(struct mempool_slot)));

    if (pa_shm_create_rw(&p->memory, p->n_blocks * p->block_size, shared, 0700) < 0) {
        pa_xfree(p);
        return NULL;
    }

    memset(&p->stat, 0, sizeof(p->stat));
    pa_atomic_store(&p->n_init, 0);

    PA_LLIST_HEAD_INIT(pa_memimport, p->imports);
    PA_LLIST_HEAD_INIT(pa_memexport, p->exports);

    p->free_slots = pa_flist_new(p->n_blocks*2);

    return p;
}

void pa_mempool_free(pa_mempool *p) {
    pa_assert(p);

    pa_mutex_lock(p->mutex);

    while (p->imports)
        pa_memimport_free(p->imports);

    while (p->exports)
        pa_memexport_free(p->exports);

    pa_mutex_unlock(p->mutex);

    pa_flist_free(p->free_slots, NULL);

    if (pa_atomic_load(&p->stat.n_allocated) > 0) {
/*         raise(SIGTRAP);  */
        pa_log_warn("Memory pool destroyed but not all memory blocks freed! %u remain.", pa_atomic_load(&p->stat.n_allocated));
    }

    pa_shm_free(&p->memory);

    pa_mutex_free(p->mutex);
    pa_semaphore_free(p->semaphore);

    pa_xfree(p);
}

/* No lock necessary */
const pa_mempool_stat* pa_mempool_get_stat(pa_mempool *p) {
    pa_assert(p);

    return &p->stat;
}

/* No lock necessary */
size_t pa_mempool_block_size_max(pa_mempool *p) {
    pa_assert(p);

    return p->block_size - PA_ALIGN(sizeof(struct mempool_slot)) - PA_ALIGN(sizeof(pa_memblock));
}

/* No lock necessary */
void pa_mempool_vacuum(pa_mempool *p) {
    struct mempool_slot *slot;
    pa_flist *list;

    pa_assert(p);

    list = pa_flist_new(p->n_blocks*2);

    while ((slot = pa_flist_pop(p->free_slots)))
        while (pa_flist_push(list, slot) < 0)
            ;

    while ((slot = pa_flist_pop(list))) {
        pa_shm_punch(&p->memory,
                     (uint8_t*) slot - (uint8_t*) p->memory.ptr + PA_ALIGN(sizeof(struct mempool_slot)),
                     p->block_size - PA_ALIGN(sizeof(struct mempool_slot)));

        while (pa_flist_push(p->free_slots, slot))
            ;
    }

    pa_flist_free(list, NULL);
}

/* No lock necessary */
int pa_mempool_get_shm_id(pa_mempool *p, uint32_t *id) {
    pa_assert(p);

    if (!p->memory.shared)
        return -1;

    *id = p->memory.id;

    return 0;
}

/* No lock necessary */
int pa_mempool_is_shared(pa_mempool *p) {
    pa_assert(p);

    return !!p->memory.shared;
}

/* For recieving blocks from other nodes */
pa_memimport* pa_memimport_new(pa_mempool *p, pa_memimport_release_cb_t cb, void *userdata) {
    pa_memimport *i;

    pa_assert(p);
    pa_assert(cb);

    i = pa_xnew(pa_memimport, 1);
    i->mutex = pa_mutex_new(TRUE, TRUE);
    i->pool = p;
    i->segments = pa_hashmap_new(NULL, NULL);
    i->blocks = pa_hashmap_new(NULL, NULL);
    i->release_cb = cb;
    i->userdata = userdata;

    pa_mutex_lock(p->mutex);
    PA_LLIST_PREPEND(pa_memimport, p->imports, i);
    pa_mutex_unlock(p->mutex);

    return i;
}

static void memexport_revoke_blocks(pa_memexport *e, pa_memimport *i);

/* Should be called locked */
static pa_memimport_segment* segment_attach(pa_memimport *i, uint32_t shm_id) {
    pa_memimport_segment* seg;

    if (pa_hashmap_size(i->segments) >= PA_MEMIMPORT_SEGMENTS_MAX)
        return NULL;

    seg = pa_xnew(pa_memimport_segment, 1);

    if (pa_shm_attach_ro(&seg->memory, shm_id) < 0) {
        pa_xfree(seg);
        return NULL;
    }

    seg->import = i;
    seg->n_blocks = 0;

    pa_hashmap_put(i->segments, PA_UINT32_TO_PTR(shm_id), seg);
    return seg;
}

/* Should be called locked */
static void segment_detach(pa_memimport_segment *seg) {
    pa_assert(seg);

    pa_hashmap_remove(seg->import->segments, PA_UINT32_TO_PTR(seg->memory.id));
    pa_shm_free(&seg->memory);
    pa_xfree(seg);
}

/* Self-locked. Not multiple-caller safe */
void pa_memimport_free(pa_memimport *i) {
    pa_memexport *e;
    pa_memblock *b;

    pa_assert(i);

    pa_mutex_lock(i->mutex);

    while ((b = pa_hashmap_get_first(i->blocks)))
        memblock_replace_import(b);

    pa_assert(pa_hashmap_size(i->segments) == 0);

    pa_mutex_unlock(i->mutex);

    pa_mutex_lock(i->pool->mutex);

    /* If we've exported this block further we need to revoke that export */
    for (e = i->pool->exports; e; e = e->next)
        memexport_revoke_blocks(e, i);

    PA_LLIST_REMOVE(pa_memimport, i->pool->imports, i);

    pa_mutex_unlock(i->pool->mutex);

    pa_hashmap_free(i->blocks, NULL, NULL);
    pa_hashmap_free(i->segments, NULL, NULL);

    pa_mutex_free(i->mutex);

    pa_xfree(i);
}

/* Self-locked */
pa_memblock* pa_memimport_get(pa_memimport *i, uint32_t block_id, uint32_t shm_id, size_t offset, size_t size) {
    pa_memblock *b = NULL;
    pa_memimport_segment *seg;

    pa_assert(i);

    pa_mutex_lock(i->mutex);

    if (pa_hashmap_size(i->blocks) >= PA_MEMIMPORT_SLOTS_MAX)
        goto finish;

    if (!(seg = pa_hashmap_get(i->segments, PA_UINT32_TO_PTR(shm_id))))
        if (!(seg = segment_attach(i, shm_id)))
            goto finish;

    if (offset+size > seg->memory.size)
        goto finish;

    if (!(b = pa_flist_pop(PA_STATIC_FLIST_GET(unused_memblocks))))
        b = pa_xnew(pa_memblock, 1);

    PA_REFCNT_INIT(b);
    b->pool = i->pool;
    b->type = PA_MEMBLOCK_IMPORTED;
    b->read_only = 1;
    pa_atomic_ptr_store(&b->data, (uint8_t*) seg->memory.ptr + offset);
    b->length = size;
    pa_atomic_store(&b->n_acquired, 0);
    pa_atomic_store(&b->please_signal, 0);
    b->per_type.imported.id = block_id;
    b->per_type.imported.segment = seg;

    pa_hashmap_put(i->blocks, PA_UINT32_TO_PTR(block_id), b);

    seg->n_blocks++;

finish:
    pa_mutex_unlock(i->mutex);

    if (b)
    stat_add(b);

    return b;
}

int pa_memimport_process_revoke(pa_memimport *i, uint32_t id) {
    pa_memblock *b;
    int ret = 0;
    pa_assert(i);

    pa_mutex_lock(i->mutex);

    if (!(b = pa_hashmap_get(i->blocks, PA_UINT32_TO_PTR(id)))) {
        ret = -1;
        goto finish;
    }

    memblock_replace_import(b);

finish:
    pa_mutex_unlock(i->mutex);

    return ret;
}

/* For sending blocks to other nodes */
pa_memexport* pa_memexport_new(pa_mempool *p, pa_memexport_revoke_cb_t cb, void *userdata) {
    pa_memexport *e;

    pa_assert(p);
    pa_assert(cb);

    if (!p->memory.shared)
        return NULL;

    e = pa_xnew(pa_memexport, 1);
    e->mutex = pa_mutex_new(TRUE, TRUE);
    e->pool = p;
    PA_LLIST_HEAD_INIT(struct memexport_slot, e->free_slots);
    PA_LLIST_HEAD_INIT(struct memexport_slot, e->used_slots);
    e->n_init = 0;
    e->revoke_cb = cb;
    e->userdata = userdata;

    pa_mutex_lock(p->mutex);
    PA_LLIST_PREPEND(pa_memexport, p->exports, e);
    pa_mutex_unlock(p->mutex);
    return e;
}

void pa_memexport_free(pa_memexport *e) {
    pa_assert(e);

    pa_mutex_lock(e->mutex);
    while (e->used_slots)
        pa_memexport_process_release(e, e->used_slots - e->slots);
    pa_mutex_unlock(e->mutex);

    pa_mutex_lock(e->pool->mutex);
    PA_LLIST_REMOVE(pa_memexport, e->pool->exports, e);
    pa_mutex_unlock(e->pool->mutex);

    pa_mutex_free(e->mutex);
    pa_xfree(e);
}

/* Self-locked */
int pa_memexport_process_release(pa_memexport *e, uint32_t id) {
    pa_memblock *b;

    pa_assert(e);

    pa_mutex_lock(e->mutex);

    if (id >= e->n_init)
        goto fail;

    if (!e->slots[id].block)
        goto fail;

    b = e->slots[id].block;
    e->slots[id].block = NULL;

    PA_LLIST_REMOVE(struct memexport_slot, e->used_slots, &e->slots[id]);
    PA_LLIST_PREPEND(struct memexport_slot, e->free_slots, &e->slots[id]);

    pa_mutex_unlock(e->mutex);

/*     pa_log("Processing release for %u", id); */

    pa_assert(pa_atomic_load(&e->pool->stat.n_exported) > 0);
    pa_assert(pa_atomic_load(&e->pool->stat.exported_size) >= (int) b->length);

    pa_atomic_dec(&e->pool->stat.n_exported);
    pa_atomic_sub(&e->pool->stat.exported_size, b->length);

    pa_memblock_unref(b);

    return 0;

fail:
    pa_mutex_unlock(e->mutex);

    return -1;
}

/* Self-locked */
static void memexport_revoke_blocks(pa_memexport *e, pa_memimport *i) {
    struct memexport_slot *slot, *next;
    pa_assert(e);
    pa_assert(i);

    pa_mutex_lock(e->mutex);

    for (slot = e->used_slots; slot; slot = next) {
        uint32_t idx;
        next = slot->next;

        if (slot->block->type != PA_MEMBLOCK_IMPORTED ||
            slot->block->per_type.imported.segment->import != i)
            continue;

        idx = slot - e->slots;
        e->revoke_cb(e, idx, e->userdata);
        pa_memexport_process_release(e, idx);
    }

    pa_mutex_unlock(e->mutex);
}

/* No lock necessary */
static pa_memblock *memblock_shared_copy(pa_mempool *p, pa_memblock *b) {
    pa_memblock *n;

    pa_assert(p);
    pa_assert(b);

    if (b->type == PA_MEMBLOCK_IMPORTED ||
        b->type == PA_MEMBLOCK_POOL ||
        b->type == PA_MEMBLOCK_POOL_EXTERNAL) {
        pa_assert(b->pool == p);
        return pa_memblock_ref(b);
    }

    if (!(n = pa_memblock_new_pool(p, b->length)))
        return NULL;

    memcpy(pa_atomic_ptr_load(&n->data), pa_atomic_ptr_load(&b->data), b->length);
    return n;
}

/* Self-locked */
int pa_memexport_put(pa_memexport *e, pa_memblock *b, uint32_t *block_id, uint32_t *shm_id, size_t *offset, size_t * size) {
    pa_shm *memory;
    struct memexport_slot *slot;
    void *data;

    pa_assert(e);
    pa_assert(b);
    pa_assert(block_id);
    pa_assert(shm_id);
    pa_assert(offset);
    pa_assert(size);
    pa_assert(b->pool == e->pool);

    if (!(b = memblock_shared_copy(e->pool, b)))
        return -1;

    pa_mutex_lock(e->mutex);

    if (e->free_slots) {
        slot = e->free_slots;
        PA_LLIST_REMOVE(struct memexport_slot, e->free_slots, slot);
    } else if (e->n_init < PA_MEMEXPORT_SLOTS_MAX)
        slot = &e->slots[e->n_init++];
    else {
        pa_mutex_unlock(e->mutex);
        pa_memblock_unref(b);
        return -1;
    }

    PA_LLIST_PREPEND(struct memexport_slot, e->used_slots, slot);
    slot->block = b;
    *block_id = slot - e->slots;

    pa_mutex_unlock(e->mutex);
/*     pa_log("Got block id %u", *block_id); */

    data = pa_memblock_acquire(b);

    if (b->type == PA_MEMBLOCK_IMPORTED) {
        pa_assert(b->per_type.imported.segment);
        memory = &b->per_type.imported.segment->memory;
    } else {
        pa_assert(b->type == PA_MEMBLOCK_POOL || b->type == PA_MEMBLOCK_POOL_EXTERNAL);
        pa_assert(b->pool);
        memory = &b->pool->memory;
    }

    pa_assert(data >= memory->ptr);
    pa_assert((uint8_t*) data + b->length <= (uint8_t*) memory->ptr + memory->size);

    *shm_id = memory->id;
    *offset = (uint8_t*) data - (uint8_t*) memory->ptr;
    *size = b->length;

    pa_memblock_release(b);

    pa_atomic_inc(&e->pool->stat.n_exported);
    pa_atomic_add(&e->pool->stat.exported_size, b->length);

    return 0;
}
