/* Backend-neutral calls of the transport boundary (transport.h). Layer:
   multiplayer/client/ (connection lifecycle). Each call forwards to the backend's
   TransportOps and accepts NULL, so callers need no checks. Worker thread only. */

#include "transport.h"

void transport_update(Transport* transport, double seconds)
{
    if (transport)
        transport->ops->update(transport->self, seconds);
}

int transport_poll(Transport* transport, TransportEvent* out)
{
    if (!transport || !out)
        return 0;
    return transport->ops->poll(transport->self, out);
}

void transport_send(Transport* transport, uint8_t packet_type, const void* data, size_t length)
{
    if (transport)
        transport->ops->send(transport->self, packet_type, data, length);
}

uint32_t transport_local_id(const Transport* transport)
{
    if (!transport)
        return UINT32_MAX;
    return transport->ops->local_id(transport->self);
}

void transport_destroy(Transport* transport)
{
    if (transport)
        transport->ops->destroy(transport->self);
}
