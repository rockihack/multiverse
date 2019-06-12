#ifndef PATCH_VECTOR_H
#define PATCH_VECTOR_H

#include <stddef.h>

struct patch {
    void *pos;
    size_t len;
    unsigned char data[];
};

#endif /* PATCH_VECTOR_H */
