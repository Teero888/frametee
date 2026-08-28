#ifndef SHA256_H
#define SHA256_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Room for 64 hex digits and the terminator.
#define SHA256_HEX_SIZE 65

typedef struct {
  uint8_t block[64];
  uint32_t state[8];
  uint32_t length;
  uint64_t bits;
} sha256_ctx_t;

void sha256_init(sha256_ctx_t *ctx);
void sha256_update(sha256_ctx_t *ctx, const void *data, size_t size);
void sha256_final(sha256_ctx_t *ctx, uint8_t digest[32]);

bool sha256_file_hex(const char *path, char out_hex[SHA256_HEX_SIZE]);

#endif // SHA256_H
