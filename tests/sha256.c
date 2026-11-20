/* Minimal self-contained SHA-256 (test tooling only).
 * SPDX-License-Identifier: Apache-2.0 */
#include "sha256.h"
#include <string.h>
#include <stdio.h>

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static const uint32_t K[64] = {
    0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
    0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
    0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
    0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
    0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
    0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
    0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
    0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u
};

typedef struct {
    uint32_t h[8];
    uint64_t total;
    uint8_t  buf[64];
    size_t   buflen;
} sha_ctx;

static void compress(sha_ctx *c, const uint8_t p[64])
{
    uint32_t w[64];
    uint32_t a,b,cc,d,e,f,g,hh,t1,t2,S0,S1,ch,maj,s0,s1;
    int i;
    for (i = 0; i < 16; ++i)
        w[i] = ((uint32_t)p[4*i] << 24) | ((uint32_t)p[4*i+1] << 16)
             | ((uint32_t)p[4*i+2] << 8) | (uint32_t)p[4*i+3];
    for (i = 16; i < 64; ++i) {
        s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15] >> 3);
        s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=c->h[0]; b=c->h[1]; cc=c->h[2]; d=c->h[3];
    e=c->h[4]; f=c->h[5]; g=c->h[6]; hh=c->h[7];
    for (i = 0; i < 64; ++i) {
        S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        ch = (e & f) ^ ((~e) & g);
        t1 = hh + S1 + ch + K[i] + w[i];
        S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        maj = (a & b) ^ (a & cc) ^ (b & cc);
        t2 = S0 + maj;
        hh=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c->h[0]+=a; c->h[1]+=b; c->h[2]+=cc; c->h[3]+=d;
    c->h[4]+=e; c->h[5]+=f; c->h[6]+=g; c->h[7]+=hh;
}

static void update(sha_ctx *c, const uint8_t *data, size_t len)
{
    c->total += len;
    while (len) {
        size_t take = 64 - c->buflen;
        if (take > len) take = len;
        memcpy(c->buf + c->buflen, data, take);
        c->buflen += take; data += take; len -= take;
        if (c->buflen == 64) { compress(c, c->buf); c->buflen = 0; }
    }
}

void wccs_sha256_hex(const uint8_t *data, size_t len, char out_hex[65])
{
    sha_ctx c;
    uint8_t tail[128];
    uint64_t bits;
    size_t pad, i;
    unsigned char digest[32];

    c.h[0]=0x6a09e667u; c.h[1]=0xbb67ae85u; c.h[2]=0x3c6ef372u; c.h[3]=0xa54ff53au;
    c.h[4]=0x510e527fu; c.h[5]=0x9b05688cu; c.h[6]=0x1f83d9abu; c.h[7]=0x5be0cd19u;
    c.total = 0; c.buflen = 0;
    update(&c, data, len);

    bits = c.total * 8;
    memset(tail, 0, sizeof(tail));
    tail[0] = 0x80;
    pad = (c.buflen < 56) ? (56 - c.buflen) : (120 - c.buflen);
    for (i = 0; i < 8; ++i) tail[pad + i] = (uint8_t)(bits >> (56 - 8*i));
    update(&c, tail, pad + 8);

    for (i = 0; i < 8; ++i) {
        digest[4*i]   = (uint8_t)(c.h[i] >> 24);
        digest[4*i+1] = (uint8_t)(c.h[i] >> 16);
        digest[4*i+2] = (uint8_t)(c.h[i] >> 8);
        digest[4*i+3] = (uint8_t)c.h[i];
    }
    for (i = 0; i < 32; ++i) sprintf(out_hex + 2*i, "%02x", digest[i]);
    out_hex[64] = '\0';
}
