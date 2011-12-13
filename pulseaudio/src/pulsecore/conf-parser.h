#ifndef fooconfparserhfoo
#define fooconfparserhfoo

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

#include <stdio.h>

/* An abstract parser for simple, line based, shallow configuration
 * files consisting of variable assignments only. */

/* Wraps info for parsing a specific configuration variable */
typedef struct pa_config_item {
    const char *lvalue; /* name of the variable */
    int (*parse)(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata); /* Function that is called to parse the variable's value */
    void *data; /* Where to store the variable's data */
} pa_config_item;

/* The configuration file parsing routine. Expects a table of
 * pa_config_items in *t that is terminated by an item where lvalue is
 * NULL */
int pa_config_parse(const char *filename, FILE *f, const pa_config_item *t, void *userdata);

/* Generic parsers for integers, booleans and strings */
int pa_config_parse_int(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);
int pa_config_parse_bool(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);
int pa_config_parse_string(const char *filename, unsigned line, const char *lvalue, const char *rvalue, void *data, void *userdata);

#endif
