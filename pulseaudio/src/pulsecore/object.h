#ifndef foopulseobjecthfoo
#define foopulseobjecthfoo

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

#include <string.h>
#include <sys/types.h>

#include <pulse/xmalloc.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/macro.h>

typedef struct pa_object pa_object;

struct pa_object {
    PA_REFCNT_DECLARE;
    const char *type_name;
    void (*free)(pa_object *o);
    int (*check_type)(const char *type_name);
};

pa_object *pa_object_new_internal(size_t size, const char *type_name, int (*check_type)(const char *type_name));
#define pa_object_new(type) ((type*) pa_object_new_internal(sizeof(type), #type, type##_check_type)

#define pa_object_free ((void (*) (pa_object* o)) pa_xfree)

int pa_object_check_type(const char *type);

static inline int pa_object_isinstance(void *o) {
    pa_object *obj = (pa_object*) o;
    return obj ? obj->check_type("pa_object") : 0;
}

pa_object *pa_object_ref(pa_object *o);
void pa_object_unref(pa_object *o);

static inline int pa_object_refcnt(pa_object *o) {
    return o ? PA_REFCNT_VALUE(o) : 0;
}

static inline pa_object* pa_object_cast(void *o) {
    pa_object *obj = (pa_object*) o;
    pa_assert(!obj || obj->check_type("pa_object"));
    return obj;
}

#define pa_object_assert_ref(o) pa_assert(pa_object_refcnt(o) > 0)

#define PA_OBJECT(o) pa_object_cast(o)

#define PA_DECLARE_CLASS(c)                                             \
    static inline int c##_isinstance(void *o) {                         \
        pa_object *obj = (pa_object*) o;                                \
        return obj ? obj->check_type(#c) : 1;                           \
    }                                                                   \
    static inline c* c##_cast(void *o) {                                \
        pa_assert(c##_isinstance(o));                                   \
        return (c*) o;                                                  \
    }                                                                   \
    static inline c* c##_ref(c *o) {                                    \
        return (c*) pa_object_ref(PA_OBJECT(o));                        \
    }                                                                   \
    static inline void c##_unref(c* o) {                                \
        pa_object_unref(PA_OBJECT(o));                                  \
    }                                                                   \
    static inline int c##_refcnt(c* o) {                                \
        return pa_object_refcnt(PA_OBJECT(o));                          \
    }                                                                   \
    static inline void c##_assert_ref(c *o) {                           \
        pa_object_assert_ref(PA_OBJECT(o));                             \
    }                                                                   \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_DEFINE_CHECK_TYPE(c, parent)                                 \
    int c##_check_type(const char *type) {                              \
        pa_assert(type);                                                \
        if (strcmp(type, #c) == 0)                                      \
            return 1;                                                   \
        return parent##_check_type(type);                               \
    }                                                                   \
    struct __stupid_useless_struct_to_allow_trailing_semicolon


#endif
