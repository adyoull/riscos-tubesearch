/* sha3.h - SHA3-512 (FIPS 202), to check the solver scripts against the
   hashes yt-dlp pins. Part of TubeSearch. MIT licence. */
#ifndef TUBE_SHA3_H
#define TUBE_SHA3_H
#include <stddef.h>

/* The SHA3-512 of data, as 128 lower-case hex digits and a NUL. */
void sha3_512_hex (const void *data, size_t len, char out[129]);

#endif
