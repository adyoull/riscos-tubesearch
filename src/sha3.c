/* sha3.c - see sha3.h. Keccak-f[1600], bytes absorbed one at a time
   (no unaligned loads). Part of TubeSearch. MIT licence. */
#include <stdint.h>
#include <string.h>
#include "sha3.h"

static const uint64_t RC[24] = {
  0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL, 0x8000000080008000ULL,
  0x000000000000808BULL, 0x0000000080000001ULL, 0x8000000080008081ULL, 0x8000000000008009ULL,
  0x000000000000008AULL, 0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
  0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL, 0x8000000000008003ULL,
  0x8000000000008002ULL, 0x8000000000000080ULL, 0x000000000000800AULL, 0x800000008000000AULL,
  0x8000000080008081ULL, 0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
};
static const int ROT[24] = { 1, 3, 6, 10, 15, 21, 28, 36, 45, 55, 2, 14, 27, 41, 56, 8, 25, 43, 62, 18, 39, 61, 20, 44 };
static const int PI[24] = { 10, 7, 11, 17, 18, 3, 5, 16, 8, 21, 24, 4, 15, 23, 19, 13, 12, 2, 20, 14, 22, 9, 6, 1 };

#define ROL(x, n) (((x) << (n)) | ((x) >> (64 - (n))))

static void keccak (uint64_t s[25])
{
  for (int round = 0; round < 24; round++)
    {
      uint64_t c[5], t;
      for (int x = 0; x < 5; x++)
        c[x] = s[x] ^ s[x + 5] ^ s[x + 10] ^ s[x + 15] ^ s[x + 20];
      for (int x = 0; x < 5; x++)
        {
          t = c[(x + 4) % 5] ^ ROL (c[(x + 1) % 5], 1);
          for (int y = 0; y < 25; y += 5)
            s[y + x] ^= t;
        }
      t = s[1];
      for (int i = 0; i < 24; i++)
        {
          int j = PI[i];
          uint64_t u = s[j];
          s[j] = ROL (t, ROT[i]);
          t = u;
        }
      for (int y = 0; y < 25; y += 5)
        {
          uint64_t r[5];
          for (int x = 0; x < 5; x++)
            r[x] = s[y + x];
          for (int x = 0; x < 5; x++)
            s[y + x] = r[x] ^ (~r[(x + 1) % 5] & r[(x + 2) % 5]);
        }
      s[0] ^= RC[round];
    }
}

static void absorb_byte (uint64_t s[25], size_t pos, unsigned char b)
{
  s[pos / 8] ^= (uint64_t) b << (8 * (pos % 8));
}

void sha3_512_hex (const void *data, size_t len, char out[129])
{
  enum { RATE = 72 };                       /* (1600 - 2 * 512) / 8 */
  static const char hex[] = "0123456789abcdef";
  const unsigned char *p = data;
  uint64_t s[25];
  size_t pos = 0;
  memset (s, 0, sizeof s);
  while (len--)
    {
      absorb_byte (s, pos++, *p++);
      if (pos == RATE)
        {
          keccak (s);
          pos = 0;
        }
    }
  absorb_byte (s, pos, 0x06);               /* SHA-3 padding */
  absorb_byte (s, RATE - 1, 0x80);
  keccak (s);
  for (int i = 0; i < 64; i++)
    {
      unsigned char b = (unsigned char) (s[i / 8] >> (8 * (i % 8)));
      out[2 * i] = hex[b >> 4];
      out[2 * i + 1] = hex[b & 15];
    }
  out[128] = 0;
}
