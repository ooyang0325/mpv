/*
 * Pure-C helpers for feeding compressed E-AC-3 to AVFoundation: IEC 61937
 * burst unwrapping and E-AC-3 (Annex E) sync frame parsing.
 *
 * Split out of ao_avfoundation.m so it can be tested without Objective-C or
 * an audio device -- this is the fiddly part, and it is bit-exact work.
 *
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// IEC 61937 burst preamble, as written by libavformat's spdif muxer.
#define IEC61937_SYNC_A 0xF872
#define IEC61937_SYNC_B 0x4E1F
#define IEC61937_DATA_TYPE_EAC3 21
#define IEC61937_HEADER_BYTES 8

// One E-AC-3 sync frame.
struct eac3_frame {
    int size;           // bytes
    int samples;        // 0 for a dependent substream (no independent timing)
    int rate;
    int channels;
    bool independent;
};

// Parse an E-AC-3 (Annex E) sync frame header. Returns false if `buf` does not
// start with a usable frame of at most `len` bytes.
static inline bool eac3_parse_frame(const uint8_t *buf, size_t len,
                                    struct eac3_frame *out)
{
    static const int rates[4] = {48000, 44100, 32000, 0};
    static const int blocks[4] = {1, 2, 3, 6};
    // acmod -> channel count, before LFE.
    static const int acmod_ch[8] = {2, 1, 2, 3, 3, 4, 4, 5};

    if (len < 6 || buf[0] != 0x0B || buf[1] != 0x77)
        return false;

    int strmtyp = (buf[2] >> 6) & 0x03;
    int frmsiz  = ((buf[2] & 0x07) << 8) | buf[3];
    int fscod   = (buf[4] >> 6) & 0x03;
    int numblks = (buf[4] >> 4) & 0x03;
    int acmod   = (buf[4] >> 1) & 0x07;
    int lfeon   = buf[4] & 0x01;

    // strmtyp 3 is reserved; fscod 3 selects the reduced sample rate form,
    // which needs fscod2 and which we do not attempt to handle.
    if (strmtyp == 3 || fscod == 3)
        return false;

    int size = (frmsiz + 1) * 2;
    if (size < 6 || (size_t)size > len)
        return false;

    *out = (struct eac3_frame) {
        .size        = size,
        .independent = strmtyp != 1,
        // Only an independent substream carries the frame's duration; a
        // dependent substream extends the same packet with more channels.
        .samples     = strmtyp != 1 ? blocks[numblks] * 256 : 0,
        .rate        = rates[fscod],
        .channels    = acmod_ch[acmod] + lfeon,
    };
    return true;
}

// Locate the next IEC 61937 burst at or after *pos. On success sets *pos to
// the burst start, *data_type to the Pc payload type and *payload_bytes to Pd,
// and returns true. Returns false if no complete preamble is present yet.
static inline bool iec61937_find_burst(const uint8_t *buf, size_t len,
                                       size_t *pos, int *data_type,
                                       int *payload_bytes)
{
    while (len - *pos >= IEC61937_HEADER_BYTES) {
        const uint8_t *b = buf + *pos;
        uint16_t pa = b[0] | (b[1] << 8);
        uint16_t pb = b[2] | (b[3] << 8);
        if (pa != IEC61937_SYNC_A || pb != IEC61937_SYNC_B) {
            *pos += 2;      // the stream is 16-bit aligned
            continue;
        }
        *data_type     = (b[4] | (b[5] << 8)) & 0x1F;
        *payload_bytes = b[6] | (b[7] << 8);
        return true;
    }
    return false;
}

// Undo the spdif muxer's 16-bit byte swap, recovering the original elementary
// stream byte order. `dst` must have room for `len` bytes; in-place is allowed.
static inline void iec61937_unswap(uint8_t *dst, const uint8_t *src, size_t len)
{
    size_t i = 0;
    for (; i + 1 < len; i += 2) {
        uint8_t a = src[i], b = src[i + 1];
        dst[i]     = b;
        dst[i + 1] = a;
    }
    if (i < len)
        dst[i] = src[i];
}
