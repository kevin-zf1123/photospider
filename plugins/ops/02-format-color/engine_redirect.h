#pragma once
#include <stdlib.h>

#include "engine_memory.h"  // NOLINT(build/include_subdir)
/* Only the private LCMS C translation units include this header. */
#define malloc ps_engine_malloc
#define calloc ps_engine_calloc
#define realloc ps_engine_realloc
#define free ps_engine_free

void* ps_engine_icc_read_tag(void*, unsigned);
void ps_engine_icc_route(void*, const char*, unsigned);
