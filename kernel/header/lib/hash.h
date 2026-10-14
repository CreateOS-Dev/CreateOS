// ============================================================
// CreateOS 0.0.1
// ============================================================

#ifndef HASH_H
#define HASH_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint32_t state[8];
    uint64_t count;
    uint8_t buffer[64];
} sha256_ctx;

void sha256_init(sha256_ctx* ctx);

void sha256_update(sha256_ctx* ctx, const void* data, size_t len);

void sha256_final(sha256_ctx* ctx, uint8_t hash[32]);

void sha256(const void* data, size_t len, uint8_t hash[32]);

void hash_password(const char* password, const uint8_t salt[16], uint8_t hash_out[32]);

int verify_password(const char* password, const uint8_t salt[16], const uint8_t stored_hash[32]);

int generate_salt(uint8_t salt[16]);

void hash_to_hex(const uint8_t hash[32], char hex[65]);

void salt_to_hex(const uint8_t salt[16], char hex[33]);

int hex_to_hash(const char hex[65], uint8_t hash[32]);

int hex_to_salt(const char hex[33], uint8_t salt[16]);

#endif