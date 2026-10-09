/*
 * MQTT-SN protocol constants (spec v1.2 + X13 extensions).
 *
 * Based on X13.Home device firmware, Copyright (c) 2011-2017 <comparator@gmx.de>,
 * BSD 3-Clause License. See LICENSE.X13 for details.
 */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size of the variable part of a message, base for all buffers (X13: MQTTSN_MSG_SIZE)
#define MQTTSN_MSG_SIZE             48
// Largest complete message: Length + MsgType + variable part
#define MQTTSN_MAX_MSG_LEN          (MQTTSN_MSG_SIZE + 2)

#define MQTTSN_PROTOCOL_ID          0x01
#define MQTTSN_CLIENT_ID_MAX        23

// Return codes
#define MQTTSN_RC_ACCEPTED          0x00
#define MQTTSN_RC_REJ_CONG          0x01    // Rejected: congestion
#define MQTTSN_RC_REJ_INV_ID        0x02    // Rejected: invalid topic ID
#define MQTTSN_RC_REJ_NOT_SUPP      0x03    // Rejected: not supported

// Message types
#define MQTTSN_ADVERTISE            0x00
#define MQTTSN_SEARCHGW             0x01
#define MQTTSN_GWINFO               0x02
#define MQTTSN_CONNECT              0x04
#define MQTTSN_CONNACK              0x05
#define MQTTSN_WILLTOPICREQ         0x06
#define MQTTSN_WILLTOPIC            0x07
#define MQTTSN_WILLMSGREQ           0x08
#define MQTTSN_WILLMSG              0x09
#define MQTTSN_REGISTER             0x0A
#define MQTTSN_REGACK               0x0B
#define MQTTSN_PUBLISH              0x0C
#define MQTTSN_PUBACK               0x0D
#define MQTTSN_PUBCOMP              0x0E
#define MQTTSN_PUBREC               0x0F
#define MQTTSN_PUBREL               0x10
#define MQTTSN_SUBSCRIBE            0x12
#define MQTTSN_SUBACK               0x13
#define MQTTSN_UNSUBSCRIBE          0x14
#define MQTTSN_UNSUBACK             0x15
#define MQTTSN_PINGREQ              0x16
#define MQTTSN_PINGRESP             0x17
#define MQTTSN_DISCONNECT           0x18
#define MQTTSN_WILLTOPICUPD         0x1A
#define MQTTSN_WILLTOPICRESP        0x1B
#define MQTTSN_WILLMSGUPD           0x1C
#define MQTTSN_WILLMSGRESP          0x1D
// X13 extensions, not part of the standard
#define MQTTSN_DHCPREQ              0x43    // Address request, node -> gateway
#define MQTTSN_DHCPRESP             0x44    // Address assignment, gateway -> node
#define MQTTSN_FORWARD              0xFE

// Flags
#define MQTTSN_FL_DUP               0x80
#define MQTTSN_FL_QOS_MASK          0x60
#define MQTTSN_FL_QOSN1             0x60
#define MQTTSN_FL_QOS2              0x40
#define MQTTSN_FL_QOS1              0x20
#define MQTTSN_FL_QOS0              0x00
#define MQTTSN_FL_RETAIN            0x10
#define MQTTSN_FL_WILL              0x08
#define MQTTSN_FL_CLEANSESSION      0x04
#define MQTTSN_FL_TOPICID_MASK      0x03
#define MQTTSN_FL_TOPICID_NORM      0x00
#define MQTTSN_FL_TOPICID_PREDEF    0x01
#define MQTTSN_FL_TOPICID_SHORT     0x02

// Fixed sizes of messages (header included, variable tail excluded)
#define MQTTSN_SIZEOF_ADVERTISE     5
#define MQTTSN_SIZEOF_SEARCHGW      3
#define MQTTSN_SIZEOF_GWINFO        3
#define MQTTSN_SIZEOF_CONNECT       6
#define MQTTSN_SIZEOF_CONNACK       3
#define MQTTSN_SIZEOF_REGISTER      6
#define MQTTSN_SIZEOF_REGACK        7
#define MQTTSN_SIZEOF_PUBLISH       7
#define MQTTSN_SIZEOF_PUBACK        7
#define MQTTSN_SIZEOF_SUBSCRIBE     5
#define MQTTSN_SIZEOF_SUBACK        8
#define MQTTSN_SIZEOF_PINGREQ       2
#define MQTTSN_SIZEOF_PINGRESP      2
#define MQTTSN_SIZEOF_DISCONNECT    2
#define MQTTSN_SIZEOF_DHCPREQ       5
#define MQTTSN_SIZEOF_DHCPRESP      5

// Topic ids 0x0000 and 0xFFFF are reserved
#define MQTTSN_TOPIC_ID_NONE        0x0000
#define MQTTSN_TOPIC_ID_INVALID     0xFFFF

_Static_assert(MQTTSN_SIZEOF_CONNECT + MQTTSN_CLIENT_ID_MAX <= MQTTSN_MAX_MSG_LEN,
               "MQTTSN_MSG_SIZE too small for CONNECT");
_Static_assert(MQTTSN_MAX_MSG_LEN < 0xC0, "Message length must not collide with frame delimiter");

#ifdef __cplusplus
}
#endif
