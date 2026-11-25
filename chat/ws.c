#include "ws.h"

#include <limits.h>
#include <stdint.h>
#include <string.h>

static uint32_t rotate_left(uint32_t x, unsigned int bits) {
    return (x << bits) | (x >> (32 - bits));
}

static void sha1_block(uint32_t state[5], const unsigned char block[64]) {
    uint32_t words[80];
    for (size_t i = 0; i < 16; i++) {
        const unsigned char *p = block + 4 * i;
        words[i] = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
                   (uint32_t)p[2] << 8 | p[3];
    }
    for (size_t i = 16; i < 80; i++)
        words[i] = rotate_left(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);

    uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (size_t i = 0; i < 80; i++) {
        uint32_t mix, k;
        if (i < 20) {
            mix = (b & c) | (~b & d);
            k = 0x5a827999;
        } else if (i < 40) {
            mix = b ^ c ^ d;
            k = 0x6ed9eba1;
        } else if (i < 60) {
            mix = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdc;
        } else {
            mix = b ^ c ^ d;
            k = 0xca62c1d6;
        }
        uint32_t next = rotate_left(a, 5) + mix + e + k + words[i];
        e = d;
        d = c;
        c = rotate_left(b, 30);
        b = a;
        a = next;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

static void sha1(const unsigned char *p, size_t n, unsigned char digest[20]) {
    uint32_t state[] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    uint64_t bit_len = (uint64_t)n * 8;
    while (n >= 64) {
        sha1_block(state, p);
        p += 64;
        n -= 64;
    }

    unsigned char tail[128] = {0};
    if (n) memcpy(tail, p, n);
    tail[n] = 0x80;
    size_t tail_len = n < 56 ? 64 : 128;
    for (size_t i = 0; i < 8; i++)
        tail[tail_len - 1 - i] = (unsigned char)(bit_len >> (8 * i));
    sha1_block(state, tail);
    if (tail_len == 128) sha1_block(state, tail + 64);

    for (size_t i = 0; i < 5; i++)
        for (size_t j = 0; j < 4; j++)
            digest[4 * i + j] = (unsigned char)(state[i] >> (24 - 8 * j));
}

static const char base64_alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void base64(const unsigned char *p, size_t n, char *out) {
    while (n) {
        unsigned int a = p[0];
        unsigned int b = n >= 2 ? p[1] : 0;
        unsigned int c = n >= 3 ? p[2] : 0;
        *out++ = base64_alphabet[a >> 2];
        *out++ = base64_alphabet[((a & 3) << 4) | (b >> 4)];
        *out++ = n >= 2 ? base64_alphabet[((b & 15) << 2) | (c >> 6)] : '=';
        *out++ = n >= 3 ? base64_alphabet[c & 63] : '=';
        if (n <= 3) break;
        n -= 3;
        p += 3;
    }
    *out = '\0';
}

int ws_accept(const char *key, size_t n, char out[29]) {
    static const char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (n != 24 || key[22] != '=' || key[23] != '=') return -1;
    for (size_t i = 0; i < 22; i++) {
        const char *digit = memchr(base64_alphabet, key[i], sizeof(base64_alphabet) - 1);
        if (!digit || (i == 21 && (digit - base64_alphabet) % 16 != 0)) return -1;
    }

    unsigned char input[24 + sizeof(guid) - 1];
    unsigned char digest[20];
    memcpy(input, key, n);
    memcpy(input + n, guid, sizeof(guid) - 1);
    sha1(input, sizeof(input), digest);
    base64(digest, sizeof(digest), out);
    return 0;
}

ssize_t ws_parse(const unsigned char *buf, size_t n, size_t max, struct ws_frame *f) {
    f->close = 0;
    if (n < 2) return 0;

    int fin = (buf[0] & 0x80) != 0;
    int op = buf[0] & 0x0f;
    unsigned int marker = buf[1] & 0x7f;
    if (!(buf[1] & 0x80) || (buf[0] & 0x70) || (op > 2 && op < 8) || op > 10 ||
        (op >= 8 && (!fin || marker > 125)) || (op == 8 && marker == 1))
        goto protocol_error;

    uint64_t len = marker;
    size_t mask_at = 2;
    if (marker >= 126) {
        mask_at = marker == 126 ? 4 : 10;
        if (n < mask_at) return 0;
        if (marker == 127 && (buf[2] & 0x80)) goto protocol_error;
        len = 0;
        for (size_t i = 2; i < mask_at; i++) len = (len << 8) | buf[i];
        if (len < (marker == 126 ? 126 : 65536)) goto protocol_error;
    }

    size_t head = mask_at + sizeof(f->mask);
    if (len > max || len > (size_t)SSIZE_MAX - head) {
        f->close = 1009;
        return -1;
    }
    if (n < head || n - head < len) return 0;

    f->fin = fin;
    f->op = op;
    f->head = head;
    f->len = (size_t)len;
    memcpy(f->mask, buf + mask_at, sizeof(f->mask));
    return (ssize_t)(head + f->len);

protocol_error:
    f->close = 1002;
    return -1;
}

void ws_unmask(unsigned char *p, size_t n, const unsigned char mask[4]) {
    for (size_t i = 0; i < n; i++) p[i] ^= mask[i & 3];
}

size_t ws_head(unsigned char out[10], int fin, int op, size_t len) {
    out[0] = (fin ? 0x80 : 0) | op;
    if (len < 126) {
        out[1] = (unsigned char)len;
        return 2;
    }
    size_t head = len <= UINT16_MAX ? 4 : 10;
    out[1] = head == 4 ? 126 : 127;
    uint64_t remaining = len;
    for (size_t i = head; i > 2; remaining >>= 8)
        out[--i] = (unsigned char)remaining;
    return head;
}

int ws_utf8(const unsigned char *p, size_t n) {
    while (n) {
        unsigned char first = *p++;
        n--;
        if (first < 0x80) continue;

        size_t extra;
        unsigned char low = 0x80, high = 0xbf;
        if (first >= 0xc2 && first <= 0xdf) {
            extra = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            extra = 2;
            if (first == 0xe0) low = 0xa0;
            if (first == 0xed) high = 0x9f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            extra = 3;
            if (first == 0xf0) low = 0x90;
            if (first == 0xf4) high = 0x8f;
        } else {
            return 0;
        }
        if (n < extra || p[0] < low || p[0] > high) return 0;
        for (size_t i = 1; i < extra; i++)
            if (p[i] < 0x80 || p[i] > 0xbf) return 0;
        p += extra;
        n -= extra;
    }
    return 1;
}
