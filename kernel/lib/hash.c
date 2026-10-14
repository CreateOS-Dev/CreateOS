// ============================================================
// CreateOS 0.0.1
// ============================================================

#include "lib/hash.h"
#include "lib/string.h"
#include "arch/x86_64/random.h"

extern volatile uint64_t g_ticks;

static const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define CH(x, y, z) ((x & y) ^ (~x & z))
#define MAJ(x, y, z) ((x & y) ^ (x & z) ^ (y & z))
#define ROTR(x, n) ((x >> n) | (x << (32 - n)))
#define SIG0(x) (ROTR(x, 2) ^ ROTR(x, 13) ^ ROTR(x, 22))
#define SIG1(x) (ROTR(x, 6) ^ ROTR(x, 11) ^ ROTR(x, 25))
#define SIG0_LO(x) (ROTR(x, 7) ^ ROTR(x, 18) ^ (x >> 3))
#define SIG1_LO(x) (ROTR(x, 17) ^ ROTR(x, 19) ^ (x >> 10))

void sha256_init(sha256_ctx* ctx) {
    ctx->state[0] = 0x6a09e667;
    ctx->state[1] = 0xbb67ae85;
    ctx->state[2] = 0x3c6ef372;
    ctx->state[3] = 0xa54ff53a;
    ctx->state[4] = 0x510e527f;
    ctx->state[5] = 0x9b05688c;
    ctx->state[6] = 0x1f83d9ab;
    ctx->state[7] = 0x5be0cd19;
    ctx->count = 0;
    for (int i = 0; i < 64; i++) ctx->buffer[i] = 0;
}

static void sha256_transform(sha256_ctx* ctx, const uint8_t* block) {
    uint32_t w[64];
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t t1, t2;

    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i * 4] << 24) |
               ((uint32_t)block[i * 4 + 1] << 16) |
               ((uint32_t)block[i * 4 + 2] << 8) |
               ((uint32_t)block[i * 4 + 3]);
    }

    for (int i = 16; i < 64; i++) {
        w[i] = SIG1_LO(w[i - 2]) + w[i - 7] + SIG0_LO(w[i - 15]) + w[i - 16];
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        t1 = h + SIG1(e) + CH(e, f, g) + K[i] + w[i];
        t2 = SIG0(a) + MAJ(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

void sha256_update(sha256_ctx* ctx, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    size_t offset = ctx->count % 64;
    ctx->count += len;

    while (len > 0) {
        size_t chunk = 64 - offset;
        if (chunk > len) chunk = len;

        for (size_t i = 0; i < chunk; i++) {
            ctx->buffer[offset + i] = p[i];
        }

        offset += chunk;
        p += chunk;
        len -= chunk;

        if (offset == 64) {
            sha256_transform(ctx, ctx->buffer);
            offset = 0;
        }
    }
}

void sha256_final(sha256_ctx* ctx, uint8_t hash[32]) {
    size_t offset = ctx->count % 64;

    ctx->buffer[offset++] = 0x80;

    if (offset > 56) {
        while (offset < 64) {
            ctx->buffer[offset++] = 0;
        }
        sha256_transform(ctx, ctx->buffer);
        offset = 0;
    }

    while (offset < 56) {
        ctx->buffer[offset++] = 0;
    }

    uint64_t bit_len = ctx->count * 8;
    ctx->buffer[56] = (bit_len >> 56) & 0xFF;
    ctx->buffer[57] = (bit_len >> 48) & 0xFF;
    ctx->buffer[58] = (bit_len >> 40) & 0xFF;
    ctx->buffer[59] = (bit_len >> 32) & 0xFF;
    ctx->buffer[60] = (bit_len >> 24) & 0xFF;
    ctx->buffer[61] = (bit_len >> 16) & 0xFF;
    ctx->buffer[62] = (bit_len >> 8) & 0xFF;
    ctx->buffer[63] = bit_len & 0xFF;

    sha256_transform(ctx, ctx->buffer);

    for (int i = 0; i < 8; i++) {
        hash[i * 4] = (ctx->state[i] >> 24) & 0xFF;
        hash[i * 4 + 1] = (ctx->state[i] >> 16) & 0xFF;
        hash[i * 4 + 2] = (ctx->state[i] >> 8) & 0xFF;
        hash[i * 4 + 3] = ctx->state[i] & 0xFF;
    }
}

void sha256(const void* data, size_t len, uint8_t hash[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, data, len);
    sha256_final(&ctx, hash);
}

int generate_salt(uint8_t salt[16]) {
    if (!salt)
        return 0;
    if (!x86_64_random_bytes(salt, 16)) {
        memset(salt, 0, 16);
        return 0;
    }
    return 1;
}

void hash_password(const char* password, const uint8_t salt[16], uint8_t hash_out[32]) {
    sha256_ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, salt, 16);
    sha256_update(&ctx, password, strlen(password));
    sha256_final(&ctx, hash_out);
}

int verify_password(const char* password, const uint8_t salt[16], const uint8_t stored_hash[32]) {
    uint8_t computed[32];
    hash_password(password, salt, computed);

    int diff = 0;
    for (int i = 0; i < 32; i++) {
        diff |= computed[i] ^ stored_hash[i];
    }
    return diff == 0;
}

static const char hex_chars[] = "0123456789abcdef";

void hash_to_hex(const uint8_t hash[32], char hex[65]) {
    for (int i = 0; i < 32; i++) {
        hex[i * 2] = hex_chars[(hash[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hex_chars[hash[i] & 0xF];
    }
    hex[64] = 0;
}

void salt_to_hex(const uint8_t salt[16], char hex[33]) {
    for (int i = 0; i < 16; i++) {
        hex[i * 2] = hex_chars[(salt[i] >> 4) & 0xF];
        hex[i * 2 + 1] = hex_chars[salt[i] & 0xF];
    }
    hex[32] = 0;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

int hex_to_hash(const char hex[65], uint8_t hash[32]) {
    for (int i = 0; i < 32; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        hash[i] = (hi << 4) | lo;
    }
    return 0;
}

int hex_to_salt(const char hex[33], uint8_t salt[16]) {
    for (int i = 0; i < 16; i++) {
        int hi = hex_val(hex[i * 2]);
        int lo = hex_val(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return -1;
        salt[i] = (hi << 4) | lo;
    }
    return 0;
}