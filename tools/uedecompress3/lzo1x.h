// LZO1X decompression, implemented from the published format description.
//
// Written rather than vendored deliberately. The reference implementation (miniLZO) is GPL,
// and while this tool is analysis-only and never links into the shipped mod, an independent
// implementation removes the licensing question rather than arguing about it.
//
// Only DEcompression is implemented - nothing here needs to compress.
//
// The encoding is an LZ77 variant where the first byte of each instruction selects between a
// literal run and a back-reference, with the reference's length and distance packed into the
// remaining bits and any overflow continued in following bytes. Lengths encoded as zero mean
// "read more bytes, 255 at a time" - which is where most of the fiddly loops come from.
//
// Correctness is not assumed. The caller verifies the output against known content; for a UE3
// package that means the name table decoding to "None", "ByteProperty", "IntProperty", which
// are UE3's canonical first three names and cannot appear by accident.

#pragma once
#include <cstdint>
#include <cstddef>

enum LzoResult { LZO_OK = 0, LZO_ERR_INPUT_OVERRUN, LZO_ERR_OUTPUT_OVERRUN, LZO_ERR_LOOKBEHIND };

// Bounds-checked throughout: this parses attacker-shaped data from a game file, and a silent
// buffer overrun here would be indistinguishable from a decompression bug.
inline int lzo1x_decompress(const uint8_t* in, size_t inLen,
                            uint8_t* out, size_t outCap, size_t* outLen)
{
    const uint8_t* ip = in;
    const uint8_t* const ipEnd = in + inLen;
    uint8_t* op = out;
    uint8_t* const opEnd = out + outCap;

    #define NEED_IN(n)  do { if ((size_t)(ipEnd - ip) < (size_t)(n)) return LZO_ERR_INPUT_OVERRUN;  } while (0)
    #define NEED_OUT(n) do { if ((size_t)(opEnd - op) < (size_t)(n)) return LZO_ERR_OUTPUT_OVERRUN; } while (0)

    size_t t = 0;
    const uint8_t* m_pos = nullptr;
    bool firstLiteral = true;

    NEED_IN(1);
    if (*ip > 17) {
        t = (size_t)(*ip++) - 17;
        if (t < 4) goto match_next;
        NEED_IN(t); NEED_OUT(t);
        do { *op++ = *ip++; } while (--t);
        goto first_literal_run;
    }

    for (;;) {
        NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;

        // ---- literal run ----
        if (t == 0) {
            NEED_IN(1);
            while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
            t += 15 + *ip++;
        }
        NEED_IN(t + 3); NEED_OUT(t + 3);
        { size_t n = t + 3; do { *op++ = *ip++; } while (--n); }

    first_literal_run:
        firstLiteral = false;
        NEED_IN(1);
        t = *ip++;
        if (t >= 16) goto match;
        NEED_IN(1);
        m_pos = op - (1 + 0x0800) - (t >> 2) - ((size_t)(*ip++) << 2);
        if (m_pos < out) return LZO_ERR_LOOKBEHIND;
        NEED_OUT(3);
        *op++ = *m_pos++; *op++ = *m_pos++; *op++ = *m_pos;
        goto match_done;

    match:
        if (t >= 64) {
            NEED_IN(1);
            m_pos = op - 1 - ((t >> 2) & 7) - ((size_t)(*ip++) << 3);
            t = (t >> 5) - 1;
        } else if (t >= 32) {
            t &= 31;
            if (t == 0) {
                NEED_IN(1);
                while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
                t += 31 + *ip++;
            }
            NEED_IN(2);
            m_pos = op - 1 - (((size_t)ip[0] | ((size_t)ip[1] << 8)) >> 2);
            ip += 2;
        } else if (t >= 16) {
            m_pos = op - ((t & 8) << 11);
            t &= 7;
            if (t == 0) {
                NEED_IN(1);
                while (*ip == 0) { t += 255; ip++; NEED_IN(1); }
                t += 7 + *ip++;
            }
            NEED_IN(2);
            m_pos -= ((size_t)ip[0] | ((size_t)ip[1] << 8)) >> 2;
            ip += 2;
            if (m_pos == op) goto eof_found;      // the end-of-stream marker
            m_pos -= 0x4000;
        } else {
            NEED_IN(1);
            m_pos = op - 1 - (t >> 2) - ((size_t)(*ip++) << 2);
            if (m_pos < out) return LZO_ERR_LOOKBEHIND;
            NEED_OUT(2);
            *op++ = *m_pos++; *op++ = *m_pos;
            goto match_done;
        }

        if (m_pos < out) return LZO_ERR_LOOKBEHIND;
        NEED_OUT(t + 2);
        // Byte at a time on purpose: matches may OVERLAP the output cursor, which is how LZO
        // encodes runs. A word-at-a-time copy would produce the wrong bytes there.
        { size_t n = t + 2; do { *op++ = *m_pos++; } while (--n); }

    match_done:
        t = (size_t)(ip[-2] & 3);
        if (t == 0) continue;

    match_next:
        NEED_IN(t); NEED_OUT(t);
        do { *op++ = *ip++; } while (--t);
        NEED_IN(1);
        t = *ip++;
        goto match;
    }

eof_found:
    (void)firstLiteral;
    if (outLen) *outLen = (size_t)(op - out);
    return LZO_OK;

    #undef NEED_IN
    #undef NEED_OUT
}
