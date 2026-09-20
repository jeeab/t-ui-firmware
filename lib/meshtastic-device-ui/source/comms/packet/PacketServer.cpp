#include "comms/PacketServer.h"
#include "util/SharedQueue.h"
#include <assert.h>

// ⛔ THIS NUMBER IS A MEMORY BUDGET, NOT A PACKET COUNT. It was 300.
//
// Every entry is a whole meshtastic_FromRadio on the heap - measured at 768 bytes, plus the
// DataPacket wrapper, two allocation headers and a deque node, so call it ~810 bytes each.
// 300 of those is 240KB on a device whose free INTERNAL heap at boot is about 86KB. The cap
// could therefore never fire: the heap ran out first, every single time.
//
// What that looked like, traced from a cold boot on 2026-09-19 (@@mem reports pktq/peak now):
//     39s   free=50,904  largest=31,732   queue 41 deep
//     49s   free= 2,004  largest=   436   queue 145 deep   <-- 145 x 768 = 111KB asked for
//     50s   Wi-Fi drops: "Reason 204" (handshake timeout) - it could not allocate
//     57s   free=71,004  largest=31,732   queue drained to 0, everything recovers
// The device spent the first minute of every boot one allocation away from a crash, and that
// is the window Get Apps and Wi-Fi both fail in.
//
// WHY LOWERING IT LOSES NOTHING. PacketAPI::sendPacket() checks available() BEFORE it calls
// getFromRadio(), so a full queue means the packet is never taken off the radio's own queue -
// it waits there and arrives a moment later. This is backpressure, not dropping.
//
// 32 x ~810 = ~26KB, which leaves the largest free block up around 31KB where the Wi-Fi
// handshake is happy. Raise it only with a measurement to justify it.
const uint32_t max_packet_queue_size = 32;

SharedQueue *sharedQueue = nullptr;

// Instrumentation, 2026-09-19. Every boot drives the internal heap to a few hundred bytes
// while the node database syncs, and this queue is the prime suspect: it holds up to
// max_packet_queue_size whole meshtastic_FromRadio structures, each one a separate heap
// allocation, and the UI task drains it only as fast as LVGL lets it. Measure before changing
// anything - twice today an inference from a single number turned out to be wrong.
static uint32_t s_queuePeak = 0;
extern "C" uint32_t tdeck_pktq_depth(void) { return sharedQueue ? (uint32_t)sharedQueue->serverQueueSize() : 0u; }
extern "C" uint32_t tdeck_pktq_peak(void) { return s_queuePeak; }
extern "C" uint32_t tdeck_pktq_cap(void) { return max_packet_queue_size; }

PacketServer *packetServer = nullptr;

PacketServer::PacketServer() : queue(nullptr) {}

PacketServer *PacketServer::init(void)
{
    packetServer = new PacketServer;
    sharedQueue = new SharedQueue;
    packetServer->begin(sharedQueue);
    return packetServer;
}

void PacketServer::begin(SharedQueue *_queue)
{
    queue = _queue;
}

Packet::PacketPtr PacketServer::receivePacket(void)
{
    assert(queue);
    if (queue->clientQueueSize() == 0)
        return {nullptr};
    return queue->serverReceive();
}

bool PacketServer::sendPacket(Packet &&p)
{
    assert(queue);
    if (queue->serverQueueSize() >= max_packet_queue_size) {
        return false;
    }
    queue->serverSend(std::move(p));
    const uint32_t n = (uint32_t)queue->serverQueueSize();
    if (n > s_queuePeak)
        s_queuePeak = n;
    return true;
}

bool PacketServer::hasData() const
{
    assert(queue);
    return queue->clientQueueSize() > 0;
}

bool PacketServer::available() const
{
    assert(queue);
    return queue->serverQueueSize() < max_packet_queue_size;
}
