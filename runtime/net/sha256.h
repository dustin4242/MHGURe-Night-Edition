// SHA-256 (FIPS 180-4): a matchmaking key's hash for the hub server (runtime/net/ldn.c).
#pragma once
#include <stdint.h>

typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; } Sha256;
void sha256_init(Sha256 *s);
void sha256_update(Sha256 *s, const void *data, size_t size);
void sha256_hex(Sha256 *s, char out[65]);
