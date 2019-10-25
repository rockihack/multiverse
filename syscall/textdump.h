#ifndef TEXT_DUMP_H
#define TEXT_DUMP_H

#include <stdint.h>
#include <stdio.h>

extern uint8_t _start;
extern uint8_t __etext;

static inline void textdump(const char *filename)
{
	uint8_t *ptr = &_start;
	size_t size = &__etext - &_start;

	FILE *file = fopen(filename, "wb");
	fwrite(ptr, sizeof(uint8_t), size, file);
	fclose(file);
}


#endif

