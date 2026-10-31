/* The Arduino wrapper's allocator hooks, as plain malloc/free. */
#include <stdlib.h>
#include "utils/helix_memory.h"
void *helix_malloc(int size) { return malloc((size_t)size); }
void helix_free(void *ptr) { free(ptr); }
