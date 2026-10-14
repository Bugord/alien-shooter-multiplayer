/* UDP backend of the transport boundary, built on the existing epnet client. Layer:
   multiplayer/client/ (connection lifecycle). It only translates epnet events and
   calls; packet validation stays in state_client.c. Worker thread only. Each backend
   instance owns one epnet client and one epnet_init reference. */

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
