/* Transport boundary of the network client: connect, pump, receive and send typed
   packets to one relay or peer. Layer: multiplayer/client/ (connection lifecycle).
   state_client.c speaks only to this interface; transport_udp.c is the current epnet
   backend, and a Steam backend can be added beside it without touching the game
   codecs, the lobby or the authority checks. Worker thread only. */

#ifndef ASMP_TRANSPORT_H
#define ASMP_TRANSPORT_H

#include <stddef.h>
#include <stdint.h>

/* Largest packet body a backend must carry (equals the epnet payload limit). */
#define TRANSPORT_MAX_PAYLOAD 1220

enum TransportEventType
{
    TRANSPORT_CONNECTED = 1,    /* The remote side accepted the connection. */
    TRANSPORT_DISCONNECTED = 2, /* Refused, timed out or closed; `reason` is backend data. */
    TRANSPORT_PACKET = 3
};

typedef struct TransportEvent
{
    enum TransportEventType type;
    unsigned int reason;
    uint8_t packet_type;
    int length;
    uint8_t data[TRANSPORT_MAX_PAYLOAD];
} TransportEvent;

typedef struct Transport Transport;

/* One backend. `self` is the backend's own state. */
typedef struct TransportOps
{
    void (*update)(void* self, double seconds);
    /* Returns 1 and fills `out` while events are queued, then 0. */
    int (*poll)(void* self, TransportEvent* out);
    void (*send)(void* self, uint8_t packet_type, const void* data, size_t length);
    /* This client's id on the relay; meaningful after TRANSPORT_CONNECTED. */
    uint32_t (*local_id)(const void* self);
    /* Disconnects and frees the backend state. */
    void (*destroy)(void* self);
} TransportOps;

struct Transport
{
    const TransportOps* ops;
    void* self;
};

/* Starts connecting to `host:port` over UDP (epnet). Returns NULL on a bad address or
   socket failure; the caller frees it with transport_destroy. */
Transport* transport_udp_connect(const char* host, unsigned short port);

/* Writes this PC's IPv4 address on the local network (a private address when there is
   one, else the first that is not loopback or link-local), or "" when none is found.
   Needs Winsock started, so call it while a UDP transport exists. */
void transport_udp_lan_address(char* out, size_t capacity);

/* Thin wrappers that accept NULL so the client can call them without checks. */
void transport_update(Transport* transport, double seconds);
int transport_poll(Transport* transport, TransportEvent* out);
void transport_send(Transport* transport, uint8_t packet_type, const void* data, size_t length);
uint32_t transport_local_id(const Transport* transport);
void transport_destroy(Transport* transport);

#endif
