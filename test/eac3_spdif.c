/* IEC 61937 unwrapping + E-AC-3 frame parsing, as used to hand compressed
 * Dolby Digital Plus (including its JOC/Atmos extension) to AVFoundation.
 *
 * This is bit-exact work on a byte-swapped transport, so it gets a real
 * round-trip check: build a frame, wrap it the way libavformat's spdif muxer
 * does, unwrap it again, and require the bytes back unchanged. */

#include <string.h>

#include "audio/out/ao_avfoundation_eac3.h"
#include "test_utils.h"

#define FRAME_BYTES 3072

// Build a syntactically valid E-AC-3 sync frame header.
static void make_frame(uint8_t *buf, int size, int strmtyp, int fscod,
                       int numblkscod, int acmod, int lfeon)
{
    memset(buf, 0xA5, size);        // payload filler, so a swap bug shows up
    int frmsiz = size / 2 - 1;
    buf[0] = 0x0B;
    buf[1] = 0x77;
    buf[2] = ((strmtyp & 3) << 6) | ((frmsiz >> 8) & 0x07);
    buf[3] = frmsiz & 0xFF;
    buf[4] = ((fscod & 3) << 6) | ((numblkscod & 3) << 4) |
             ((acmod & 7) << 1) | (lfeon & 1);
    buf[5] = 16 << 3;              // E-AC-3 bsid
}

static void test_dec3_cookie(void)
{
    uint8_t frame[FRAME_BYTES];
    struct eac3_frame fr;
    make_frame(frame, FRAME_BYTES, 0, 0, 3, 7, 1);
    assert_true(eac3_parse_frame(frame, sizeof(frame), &fr));

    uint8_t cookie[EAC3_DEC3_COOKIE_MAX_BYTES];
    const uint8_t plain[] = {
        0x00, 0x00, 0x00, 0x0d, 'd', 'e', 'c', '3',
        0x18, 0x00, 0x20, 0x0f, 0x00,
    };
    const uint8_t atmos[] = {
        0x00, 0x00, 0x00, 0x0f, 'd', 'e', 'c', '3',
        0x18, 0x00, 0x20, 0x0f, 0x00, 0x01, 0x10,
    };
    size_t size = eac3_make_dec3_cookie(&fr, FRAME_BYTES, false, cookie);
    assert_int_equal(size, sizeof(plain));
    assert_true(memcmp(cookie, plain, size) == 0);
    size = eac3_make_dec3_cookie(&fr, FRAME_BYTES, true, cookie);
    assert_int_equal(size, sizeof(atmos));
    assert_true(memcmp(cookie, atmos, size) == 0);
}

// Wrap a payload into an IEC 61937 burst exactly as the spdif muxer does:
// preamble in native order, payload byte-swapped in 16-bit words.
static void make_burst(uint8_t *dst, const uint8_t *payload, int len,
                       int data_type)
{
    dst[0] = IEC61937_SYNC_A & 0xFF;  dst[1] = IEC61937_SYNC_A >> 8;
    dst[2] = IEC61937_SYNC_B & 0xFF;  dst[3] = IEC61937_SYNC_B >> 8;
    dst[4] = data_type & 0xFF;        dst[5] = 0;
    dst[6] = len & 0xFF;              dst[7] = (len >> 8) & 0xFF;
    for (int i = 0; i + 1 < len; i += 2) {
        dst[8 + i]     = payload[i + 1];
        dst[8 + i + 1] = payload[i];
    }
}

static void test_frame_parsing(void)
{
    uint8_t f[FRAME_BYTES];
    struct eac3_frame fr;

    // 48 kHz, 6 blocks (1536 samples), acmod 3/2 + LFE == 5.1.
    make_frame(f, FRAME_BYTES, 0, 0, 3, 7, 1);
    assert_true(eac3_parse_frame(f, sizeof(f), &fr));
    assert_int_equal(fr.size, FRAME_BYTES);
    assert_int_equal(fr.samples, 1536);
    assert_int_equal(fr.rate, 48000);
    assert_int_equal(fr.channels, 6);
    assert_true(fr.independent);

    // Sample rate and block count come from the header, not from assumptions.
    make_frame(f, FRAME_BYTES, 0, 1, 0, 2, 0);
    assert_true(eac3_parse_frame(f, sizeof(f), &fr));
    assert_int_equal(fr.rate, 44100);
    assert_int_equal(fr.samples, 256);
    assert_int_equal(fr.channels, 2);

    // A dependent substream extends the previous packet; it has no duration
    // of its own, and must not be mistaken for the start of a new packet.
    make_frame(f, FRAME_BYTES, 1, 0, 3, 7, 1);
    assert_true(eac3_parse_frame(f, sizeof(f), &fr));
    assert_true(!fr.independent);
    assert_int_equal(fr.samples, 0);

    // Rejections: bad sync, truncated buffer, reserved fields.
    make_frame(f, FRAME_BYTES, 0, 0, 3, 7, 1);
    f[1] = 0x00;
    assert_true(!eac3_parse_frame(f, sizeof(f), &fr));

    make_frame(f, FRAME_BYTES, 0, 0, 3, 7, 1);
    assert_true(!eac3_parse_frame(f, FRAME_BYTES - 2, &fr));   // frame overruns
    assert_true(!eac3_parse_frame(f, 4, &fr));                 // shorter than a header

    make_frame(f, FRAME_BYTES, 3, 0, 3, 7, 1);                 // reserved strmtyp
    assert_true(!eac3_parse_frame(f, sizeof(f), &fr));
    make_frame(f, FRAME_BYTES, 0, 3, 3, 7, 1);                 // fscod2 form
    assert_true(!eac3_parse_frame(f, sizeof(f), &fr));
}

static void test_burst_round_trip(void)
{
    uint8_t frame[FRAME_BYTES];
    uint8_t burst[IEC61937_HEADER_BYTES + FRAME_BYTES];
    uint8_t out[FRAME_BYTES];

    make_frame(frame, FRAME_BYTES, 0, 0, 3, 7, 1);
    make_burst(burst, frame, FRAME_BYTES, IEC61937_DATA_TYPE_EAC3);

    // The wrapped payload must NOT already match, or the test proves nothing.
    assert_true(memcmp(burst + IEC61937_HEADER_BYTES, frame, FRAME_BYTES) != 0);

    size_t pos = 0;
    int data_type = 0, pd = 0;
    assert_true(iec61937_find_burst(burst, sizeof(burst), &pos, &data_type, &pd));
    assert_int_equal(pos, 0);
    assert_int_equal(data_type, IEC61937_DATA_TYPE_EAC3);
    assert_int_equal(pd, FRAME_BYTES);

    iec61937_unswap(out, burst + IEC61937_HEADER_BYTES, pd);
    assert_true(memcmp(out, frame, FRAME_BYTES) == 0);

    // The recovered bytes must parse as the frame we started from.
    struct eac3_frame fr;
    assert_true(eac3_parse_frame(out, pd, &fr));
    assert_int_equal(fr.size, FRAME_BYTES);
    assert_int_equal(fr.samples, 1536);

    // Unswapping is an involution, and works in place.
    iec61937_unswap(out, out, pd);
    assert_true(memcmp(out, burst + IEC61937_HEADER_BYTES, pd) == 0);
}

static void test_burst_scanning(void)
{
    // Bursts are separated by a zero-padded repetition period, so the scanner
    // has to skip padding and find the next preamble on its own.
    enum { PAD = 512, N = 3 };
    size_t stride = IEC61937_HEADER_BYTES + FRAME_BYTES + PAD;
    uint8_t *stream = calloc(N, stride);
    uint8_t frame[FRAME_BYTES];

    make_frame(frame, FRAME_BYTES, 0, 0, 3, 7, 1);
    for (int i = 0; i < N; i++)
        make_burst(stream + i * stride, frame, FRAME_BYTES, IEC61937_DATA_TYPE_EAC3);

    size_t pos = 0;
    int found = 0, data_type, pd;
    while (iec61937_find_burst(stream, N * stride, &pos, &data_type, &pd)) {
        assert_int_equal(data_type, IEC61937_DATA_TYPE_EAC3);
        assert_int_equal(pd, FRAME_BYTES);
        assert_int_equal(pos, (size_t)found * stride);
        found++;
        pos += IEC61937_HEADER_BYTES + pd;
    }
    assert_int_equal(found, N);

    // A partial trailing burst must be reported as "not yet", so the caller
    // keeps it buffered instead of enqueuing a truncated frame.
    pos = 0;
    assert_true(!iec61937_find_burst(stream, IEC61937_HEADER_BYTES - 1, &pos,
                                     &data_type, &pd));

    free(stream);
}

int main(void)
{
    test_frame_parsing();
    test_burst_round_trip();
    test_burst_scanning();
    test_dec3_cookie();
    return 0;
}
