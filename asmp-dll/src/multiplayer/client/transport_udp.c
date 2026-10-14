/* UDP backend of the transport boundary, built on the existing epnet client. Layer:
   multiplayer/client/ (connection lifecycle). It only translates epnet events and
   calls; packet validation stays in state_client.c. Worker thread only. Each backend
   instance owns one epnet client and one epnet_init reference. */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "transport.h"
#include "epnet_client.h"

typedef struct UdpTransport
{
    Transport base;
    epnet_client_t* client;
} UdpTransport;

static void udp_update(void* self, double seconds)
{
    UdpTransport* udp = self;
    epnet_client_update(udp->client, seconds);
}

/* Copies one epnet event into the backend-neutral form. Reliable-channel events are
   not used by the relay protocol and are skipped. */
static int udp_poll(void* self, TransportEvent* out)
{
    UdpTransport* udp = self;
    epnet_event_t event;

    while (epnet_client_poll_events(udp->client, &event))
    {
        memset(out, 0, sizeof(*out));
        if (event.type == EPNET_EVENT_CONNECTED)
        {
            out->type = TRANSPORT_CONNECTED;
            return 1;
        }
        if (event.type == EPNET_EVENT_DISCONNECTED)
        {
            out->type = TRANSPORT_DISCONNECTED;
            out->reason = event.data.disconnect_reason;
            return 1;
        }
        if (event.type == EPNET_EVENT_PACKET && event.data.packet.len >= 0 &&
            event.data.packet.len <= TRANSPORT_MAX_PAYLOAD)
        {
            out->type = TRANSPORT_PACKET;
            out->packet_type = event.data.packet.pkt_type;
            out->length = event.data.packet.len;
            memcpy(out->data, event.data.packet.data, (size_t)out->length);
            return 1;
        }
    }
    return 0;
}

static void udp_send(void* self, uint8_t packet_type, const void* data, size_t length)
{
    UdpTransport* udp = self;
    if (length > TRANSPORT_MAX_PAYLOAD)
        return;
    epnet_client_send(udp->client, packet_type, data, (int)length);
}

static uint32_t udp_local_id(const void* self)
{
    const UdpTransport* udp = self;
    return epnet_client_get_id(udp->client);
}

static void udp_destroy(void* self)
{
    UdpTransport* udp = self;
    epnet_client_disconnect(udp->client);
    epnet_client_destroy(udp->client);
    epnet_shutdown();
    free(udp);
}

static const TransportOps udp_ops = {udp_update, udp_poll, udp_send, udp_local_id, udp_destroy};

Transport* transport_udp_connect(const char* host, unsigned short port)
{
    if (!host || !port || epnet_init())
        return NULL;

    UdpTransport* udp = calloc(1, sizeof(*udp));
    if (udp)
    {
        udp->client = epnet_client_create();
        if (udp->client && !epnet_client_connect(udp->client, host, port))
        {
            udp->base.ops = &udp_ops;
            udp->base.self = udp;
            return &udp->base;
        }
        if (udp->client)
            epnet_client_destroy(udp->client);
        free(udp);
    }
    epnet_shutdown();
    return NULL;
}

/* 10.0.0.0/8, 172.16.0.0/12 and 192.168.0.0/16: where friends on the same network
   can reach the host. */
static int is_private(const unsigned char* octet)
{
    if (octet[0] == 10)
        return 1;
    if (octet[0] == 172 && octet[1] >= 16 && octet[1] <= 31)
        return 1;
    return octet[0] == 192 && octet[1] == 168;
}

/* Loopback (127/8) and link-local (169.254/16) addresses are useless to friends. */
static int is_usable(const unsigned char* octet)
{
    return octet[0] != 127 && !(octet[0] == 169 && octet[1] == 254) && octet[0] != 0;
}

void transport_udp_lan_address(char* out, size_t capacity)
{
    char host[256];
    struct addrinfo hints;
    struct addrinfo* found = NULL;

    out[0] = 0;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    if (gethostname(host, sizeof(host)) || getaddrinfo(host, NULL, &hints, &found))
        return;

    int have_private = 0;
    for (struct addrinfo* item = found; item && !have_private; item = item->ai_next)
    {
        const struct sockaddr_in* address = (const struct sockaddr_in*)item->ai_addr;
        const unsigned char* octet = (const unsigned char*)&address->sin_addr;
        if (!is_usable(octet) || (out[0] && !is_private(octet)))
            continue;
        sprintf_s(out, capacity, "%u.%u.%u.%u", octet[0], octet[1], octet[2], octet[3]);
        have_private = is_private(octet);
    }
    freeaddrinfo(found);
}
