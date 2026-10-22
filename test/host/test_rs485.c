// Host tests of the X13 RS-485 framing (components/serial_phy/rs485_frame.c)
#include <stdio.h>
#include <string.h>

#include "rs485_frame.h"

static int failed, passed;

#define CHECK(cond) do { \
    if (cond) { passed++; } else { failed++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define OWN     0x21

static rs485_frame_decoder_t dec;
static size_t got_len;                  // Last message received by feed()
static uint8_t got[MQTTSN_LINK_MAX_LEN];
static int frames;                      // Messages received by feed()

static void feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        size_t n = rs485_frame_decode(&dec, data[i], OWN);
        if (n > 0) {
            frames++;
            got_len = n;
            memcpy(got, dec.buf, n);
        }
    }
}

static size_t encode(const uint8_t *msg, size_t len, uint8_t dst, uint8_t src, uint8_t *out)
{
    return rs485_frame_encode(msg, len, dst, src, out, RS485_FRAME_SIZE(MQTTSN_LINK_MAX_LEN));
}

int main(void)
{
    const uint8_t pingreq[] = { 0x02, 0x16 };
    const uint8_t gwinfo[] = { 0x03, 0x02, 0x01 };
    uint8_t frame[RS485_FRAME_SIZE(MQTTSN_LINK_MAX_LEN)];
    uint8_t two[2 * sizeof(frame)];

    // Encoding as X13 rs485_phy: LEN = data + 2, CRC = LEN + DST + SRC + data
    CHECK(encode(pingreq, sizeof(pingreq), 0x10, OWN, frame) == 8);
    CHECK(memcmp(frame, (const uint8_t[]){ 0xF0, 0x1C, 0x04, 0x10, 0x21, 0x02, 0x16, 0x4D }, 8) == 0);
    CHECK(encode(gwinfo, sizeof(gwinfo), 0x00, 0xFF, frame) == 9);
    CHECK(memcmp(frame, (const uint8_t[]){ 0xF0, 0x1C, 0x05, 0x00, 0xFF, 0x03, 0x02, 0x01, 0x0A }, 9) == 0);

    // Length limits and the output buffer
    uint8_t big[MQTTSN_LINK_MAX_LEN + 1];
    for (size_t i = 0; i < sizeof(big); i++) {
        big[i] = (uint8_t)(0xF0 + i);
    }
    big[0] = (uint8_t)MQTTSN_LINK_MAX_LEN;
    big[1] = RS485_FRAME_SOF1;                  // The start of a frame inside the data, there is no escaping
    big[2] = RS485_FRAME_SOF2;
    CHECK(encode(big, MQTTSN_LINK_MAX_LEN, OWN, 0x10, frame) == RS485_FRAME_SIZE(MQTTSN_LINK_MAX_LEN));
    CHECK(encode(big, MQTTSN_LINK_MAX_LEN + 1, OWN, 0x10, frame) == 0);
    CHECK(encode(big, 1, OWN, 0x10, frame) == 0);
    CHECK(rs485_frame_encode(pingreq, 2, OWN, 0x10, frame, 7) == 0);

    // The longest frame to this node, the sender is known
    rs485_frame_decoder_reset(&dec);
    size_t n = encode(big, MQTTSN_LINK_MAX_LEN, OWN, 0x10, frame);
    frames = 0;
    feed(frame, n);
    CHECK((frames == 1) && (got_len == MQTTSN_LINK_MAX_LEN) && (memcmp(got, big, got_len) == 0));
    CHECK(dec.src == 0x10);
    CHECK(!rs485_frame_decoder_busy(&dec));

    // Broadcast is taken, a frame to another node is read to its end and skipped, the next one is taken
    frames = 0;
    n = encode(gwinfo, sizeof(gwinfo), 0x00, 0x10, frame);
    feed(frame, n);
    CHECK((frames == 1) && (got_len == 3) && (memcmp(got, gwinfo, 3) == 0) && (dec.src == 0x10));
    frames = 0;
    n = encode(big, MQTTSN_LINK_MAX_LEN, 0x22, 0x10, frame);  // DATA with F0 1C inside
    size_t n2 = encode(pingreq, sizeof(pingreq), OWN, 0x11, &frame[n]);
    feed(frame, n);
    CHECK((frames == 0) && !rs485_frame_decoder_busy(&dec));
    feed(&frame[n], n2);
    CHECK((frames == 1) && (got_len == 2) && (dec.src == 0x11));

    // Two frames in one chunk
    frames = 0;
    n = encode(pingreq, sizeof(pingreq), OWN, 0x10, two);
    n += encode(gwinfo, sizeof(gwinfo), 0x00, 0x12, &two[n]);
    feed(two, n);
    CHECK((frames == 2) && (got_len == 3) && (dec.src == 0x12));

    // Noise before the start of a frame, F0 F0 1C
    frames = 0;
    n = encode(pingreq, sizeof(pingreq), OWN, 0x10, frame);
    feed((const uint8_t[]){ 0x00, 0x1C, 0xFF, 0xF0, 0x55, 0xF0 }, 6);
    CHECK(rs485_frame_decoder_busy(&dec));
    feed(frame, n);
    CHECK(frames == 1);

    // Bad CRC: the frame is dropped, the next one is taken
    frames = 0;
    n = encode(pingreq, sizeof(pingreq), OWN, 0x10, frame);
    frame[n - 1] ^= 0xFF;
    feed(frame, n);
    CHECK((frames == 0) && !rs485_frame_decoder_busy(&dec));
    frame[n - 1] ^= 0xFF;
    feed(frame, n);
    CHECK(frames == 1);

    // Bad LEN (X13: lost arbitration): too short and too long
    frames = 0;
    feed((const uint8_t[]){ 0xF0, 0x1C, 0x03 }, 3);
    CHECK(!rs485_frame_decoder_busy(&dec));
    feed((const uint8_t[]){ 0xF0, 0x1C, (uint8_t)(MQTTSN_LINK_MAX_LEN + 3) }, 3);
    CHECK(!rs485_frame_decoder_busy(&dec));
    feed(frame, n);
    CHECK(frames == 1);

    // A node without an address (DHCP) takes frames to 0xFF
    frames = 0;
    n = encode(pingreq, sizeof(pingreq), 0xFF, 0x10, frame);
    for (size_t i = 0; i < n; i++) {
        if (rs485_frame_decode(&dec, frame[i], 0xFF) > 0) {
            frames++;
        }
    }
    CHECK(frames == 1);
    feed(frame, n);                             // Not for OWN
    CHECK(frames == 1);

    // Reset drops a partial frame
    feed(frame, 4);
    CHECK(rs485_frame_decoder_busy(&dec));
    rs485_frame_decoder_reset(&dec);
    CHECK(!rs485_frame_decoder_busy(&dec));

    printf("%d passed, %d failed\n", passed, failed);
    printf(failed ? "FAILED\n" : "ALL PASSED\n");
    return failed ? 1 : 0;
}
