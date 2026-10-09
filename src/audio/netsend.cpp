#include "net.h"
#include "netcodec.h"
#include "util.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Net Send
//
// Takes the bus and puts it on the network. Shaped like the recorder rather
// than like a renderer: Render() hands it audio and it clocks nothing, so a
// session whose only output is a Net Send block still needs a device to drive
// it, exactly as a session whose only output is a recorder does.
//
// The render thread does none of the work. It drops frames into a queue and
// wakes a thread that does the encoding and the sending, because sendto can
// block for as long as the stack feels like and a render callback that is late
// is a dropout on every output in the session, not just this one.
//
// In FastRoute's own protocol the sender also announces itself twice a second
// on the beacon group, so that a receiver anywhere on the subnet can list it
// by name without anybody typing an address. In VBAN mode there is nothing to
// announce: a VBAN sender is found by the far end asking, and what it asks is
// answered by a Net Receive block rather than by this one.
// ---------------------------------------------------------------------------

namespace {

const int kBeaconEveryMs = 500;
const int kWakeMs = 200;

class Sender : public NetSink
{
public:
    ~Sender() override { Stop(); }

    bool Start(const Block& b, int rate, std::wstring* err) override
    {
        Stop();
        if (!net::Resolve(b, true, &m_set, err)) return false;
        m_rate = rate > 0 ? rate : kDefaultRate;

        if (m_set.protocol == NP_VBAN)
        {
            // VBAN carries the rate as an index into a fixed table, so a rate
            // that is not in the table cannot be sent at all. Better to say so
            // than to send something a receiver will read as the wrong speed.
            m_vbanRate = net::vban::RateIndex(m_rate);
            if (m_vbanRate < 0)
            {
                if (err) *err = FormatW(
                    L"VBAN has no way to describe %d Hz, which is what this "
                    L"session runs at. Send it over FastRoute instead, or run "
                    L"the session at a rate VBAN knows, such as 48000 or 44100.",
                    m_rate);
                return false;
            }
            // Its frame count is one byte, so 256 frames is the ceiling
            // whatever the packet length asks for.
            m_vbanFrames = net::FramesPerPacketFor(net::NC_PCM16, m_rate,
                                                   m_set.packetMs);
            if (m_vbanFrames > net::vban::kMaxFrames)
                m_vbanFrames = net::vban::kMaxFrames;
            m_scratch.assign((size_t)m_vbanFrames * kChannels, 0.0f);
        }
        else
        {
            m_enc = net::MakeEncoder(m_set.codec, m_rate, m_set.packetMs,
                                     m_set.opus, err);
            if (!m_enc) return false;
        }

        if (!m_sock.Open(err)) return false;
        // A burst of packets has to go somewhere while the stack drains it.
        m_sock.SetSendBufferBytes(512 * 1024);
        // Bound, not just opened, because this socket is also where receivers
        // ask to be sent the audio. Shared, so that a machine already using
        // this port for something else does not stop the sender working for
        // the receivers it can reach.
        if (!m_sock.Bind(m_set.port, true, nullptr))
            m_takesAsks = false;
        if (m_set.Multicast())
        {
            // The hop limit is a multicast setting and is applied only to
            // multicast. A packet to a named host keeps the system's own TTL,
            // because a host worth naming is often not on this subnet and one
            // hop would not reach it.
            m_sock.SetMulticastTtl(m_set.ttl);
            // On, so that a Net Receive block on this same machine hears this
            // sender. Trying the pair out on one PC is the first thing anybody
            // does, and a loopback that was off would make that silently fail.
            m_sock.SetMulticastLoopback(true);
        }
        else
        {
            m_sock.SetBroadcast(true);
        }

        {
            std::lock_guard<std::mutex> g(m_lock);
            m_queue.clear();
            m_packets = 0;
            m_bytes = 0;
            m_dropped = 0;
            m_lastErr.clear();
        }
        m_frames = 0;
        m_seq = 0;
        m_running = true;
        m_thread = std::thread([this] { Run(); });
        return true;
    }

    void Write(const float* buf, size_t frames) override
    {
        if (!m_running.load()) return;
        {
            std::lock_guard<std::mutex> g(m_lock);
            // A queue that grew without limit would turn a stalled network
            // into a memory leak. Half a second is far more than the sender
            // thread ever needs, so reaching it means something is wrong and
            // the oldest audio is the least useful thing to keep.
            const size_t cap = (size_t)m_rate * kChannels / 2;
            if (m_queue.size() + frames * kChannels > cap)
            {
                m_dropped += frames;
                return;
            }
            m_queue.insert(m_queue.end(), buf, buf + frames * kChannels);
        }
        m_wake.notify_one();
    }

    void Configure(const Block& b) override
    {
        // Everything a Net Send block has decides how its socket or its
        // encoder was built, so KeyFor makes a change of any of them a new
        // node rather than a live edit. What is left is the stream name, which
        // only the beacon carries and can therefore change under way.
        net::Settings s;
        if (!net::Resolve(b, true, &s, nullptr)) return;
        std::lock_guard<std::mutex> g(m_lock);
        m_set.name = s.name;
        m_set.streamId = s.streamId;
    }

    // Who has asked for this stream, and when they last asked. A receiver
    // renews every second; one that stops asking is dropped, which is also
    // what happens to one that was closed without saying goodbye.
    struct Asker
    {
        uint32_t addr = 0;
        uint16_t port = 0;
        unsigned long long lastSeen = 0;
    };

    void Stop() override
    {
        if (!m_running.exchange(false)) return;
        m_wake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        m_sock.Close();
        m_enc.reset();
    }

    std::wstring StatusText() override
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_lastErr.empty())
            return FormatW(L"Net Send \"%s\": %s", m_set.name.c_str(),
                           m_lastErr.c_str());

        const int kbps = m_elapsedMs > 0
            ? (int)((unsigned long long)m_bytes * 8 / (unsigned long long)m_elapsedMs)
            : 0;
        std::wstring where;
        if (m_set.Multicast())
            where = FormatW(L"to the group %s",
                            net::AddrText(m_set.addr, m_set.port).c_str());
        else if (m_set.addr)
            where = FormatW(L"to %s", net::AddrText(m_set.addr, m_set.port).c_str());
        else if (m_askers == 0)
            where = FormatW(L"waiting on port %u for a receiver to ask",
                            (unsigned)m_set.port);
        else
            where = FormatW(L"to %d receiver%s that asked", m_askers,
                            m_askers == 1 ? L"" : L"s");

        std::wstring line = FormatW(L"Net Send \"%s\" %s over %s: %llu packets, %d kbps",
                                    m_set.name.c_str(), where.c_str(),
                                    NetProtocolName(m_set.protocol),
                                    (unsigned long long)m_packets, kbps);
        if (m_set.addr && m_askers > 0)
            line += FormatW(L", and %d that asked", m_askers);
        if (!m_takesAsks)
            line += FormatW(L"; port %u was already taken, so receivers cannot "
                            L"ask this sender for the audio",
                            (unsigned)m_set.port);
        if (m_dropped)
            line += FormatW(L", %llu frames dropped waiting for the network",
                            (unsigned long long)m_dropped);
        return line;
    }

private:
    int CodecOnWire() const
    {
        return (m_set.protocol == NP_VBAN) ? net::NC_PCM16 : m_set.codec;
    }

    // What this sender is, in the one shape both the announcement on the
    // group and the answer to a direct ask want it in.
    net::Beacon Describe()
    {
        net::Beacon b;
        {
            std::lock_guard<std::mutex> g(m_lock);
            b.streamId = m_set.streamId;
            b.name = m_set.name;
        }
        b.host = net::HostName();
        b.codec = (uint8_t)CodecOnWire();
        b.channels = (uint8_t)kChannels;
        // Where a receiver should go to get it. A unicast sender has no group
        // to offer, so it says so with a zero and is reached by being asked.
        b.group = m_set.Multicast() ? m_set.addr : 0;
        b.port = m_set.port;
        b.rate = (uint32_t)(m_enc ? m_enc->WireRate() : m_rate);
        b.packetFrames = (uint16_t)(m_enc ? m_enc->WireFramesPerPacket()
                                          : m_vbanFrames);
        return b;
    }

    void SendBeacon()
    {
        net::Beacon b = Describe();
        unsigned char buf[net::kBeaconBytes];
        net::PackBeacon(b, buf);
        m_sock.Send(buf, sizeof(buf), net::kBeaconGroup, net::kBeaconPort);
    }

    void SendBeaconTo(uint32_t addr, uint16_t port)
    {
        net::Beacon b = Describe();
        unsigned char buf[net::kBeaconBytes];
        net::PackBeacon(b, buf);
        m_sock.Send(buf, sizeof(buf), addr, port);
    }

    void SendFastRoute(std::vector<float>& chunk)
    {
        m_enc->Push(chunk.data(), (int)(chunk.size() / kChannels));

        unsigned char packet[net::kDataHeaderBytes + net::kMaxPayloadBytes];
        for (;;)
        {
            int frames = 0;
            const int bytes = m_enc->Pull(packet + net::kDataHeaderBytes,
                                          net::kMaxPayloadBytes, &frames);
            if (bytes == 0) break;
            if (bytes < 0) { Fail(L"the encoder failed"); break; }

            net::DataHeader h;
            h.codec = (uint8_t)m_set.codec;
            h.channels = (uint8_t)kChannels;
            h.rate = (uint32_t)m_enc->WireRate();
            h.streamId = m_set.streamId;
            h.seq = m_seq++;
            h.frames = m_frames;
            h.frameCount = (uint16_t)frames;
            h.payloadBytes = (uint16_t)bytes;
            m_frames += (unsigned long long)frames;
            net::PackDataHeader(h, packet);

            Fire(packet, net::kDataHeaderBytes + bytes);
        }
    }

    void SendVban(std::vector<float>& chunk)
    {
        // VBAN is written by hand rather than through an encoder, because the
        // packet is a VBAN header followed by plain interleaved PCM and there
        // is nothing in between for an encoder to do.
        m_pending.insert(m_pending.end(), chunk.begin(), chunk.end());

        const size_t need = (size_t)m_vbanFrames * kChannels;
        unsigned char packet[net::vban::kHeaderBytes + net::vban::kMaxDataBytes];

        while (m_pending.size() >= need)
        {
            for (size_t i = 0; i < need; ++i) m_scratch[i] = m_pending[i];
            m_pending.erase(m_pending.begin(), m_pending.begin() + (ptrdiff_t)need);

            net::vban::Header h;
            h.protocol = net::vban::PROTOCOL_AUDIO;
            h.rateIndex = (uint8_t)m_vbanRate;
            h.frames = (uint16_t)m_vbanFrames;
            h.channels = (uint16_t)kChannels;
            h.datatype = net::vban::DATATYPE_INT16;
            h.codec = net::vban::CODEC_PCM;
            h.name = m_set.name;
            h.counter = (uint32_t)m_seq++;
            net::vban::PackHeader(h, packet);

            const int bytes = m_vbanFrames * net::PcmBytesPerFrame(net::NC_PCM16);
            net::PcmFromFloat(net::NC_PCM16, m_scratch.data(), m_vbanFrames,
                              packet + net::vban::kHeaderBytes);
            m_frames += (unsigned long long)m_vbanFrames;

            Fire(packet, net::vban::kHeaderBytes + bytes);
        }
    }

    void Fire(const unsigned char* data, int len)
    {
        bool anySent = false;
        bool anyRefused = false;
        uint32_t refusedBy = 0;

        // The fixed target, when there is one: a group always, and a named
        // host when one was given.
        if (m_set.addr)
        {
            if (m_sock.Send(data, len, m_set.addr, m_set.port) == len) anySent = true;
            else { anyRefused = true; refusedBy = m_set.addr; }
        }

        // And everybody who asked. This is the ordinary case for a unicast
        // sender: it does not know who wants the audio until they say so.
        for (const Asker& a : m_askers_list)
        {
            if (m_sock.Send(data, len, a.addr, a.port) == len) anySent = true;
            else { anyRefused = true; refusedBy = a.addr; }
        }

        std::lock_guard<std::mutex> g(m_lock);
        if (anySent)
        {
            ++m_packets;
            m_bytes += (unsigned long long)len;
        }
        if (anyRefused)
        {
            // Not fatal and not worth stopping for: a link that comes back
            // should start working again without anybody restarting a session.
            m_lastErr = FormatW(L"the network refused a packet for %s",
                                net::AddrText(refusedBy, m_set.port).c_str());
        }
        else if (anySent)
        {
            m_lastErr.clear();
        }
    }

    // Whoever has asked since the last pass, and a reply telling them what
    // they are about to get.
    void TakeAsks()
    {
        if (!m_takesAsks) return;

        unsigned char buf[2048];
        for (int i = 0; i < 32; ++i)
        {
            uint32_t from = 0;
            uint16_t fromPort = 0;
            const int got = m_sock.Recv(buf, sizeof(buf), 0, &from, &fromPort);
            if (got <= 0) break;

            net::Subscribe ask;
            if (!net::ParseSubscribe(buf, got, &ask)) continue;

            // Asked for by name or by id, so that a machine running several
            // senders hands over the right one. An empty ask takes whatever
            // this sender happens to be.
            if (ask.wantStreamId && ask.wantStreamId != m_set.streamId) continue;
            if (!ask.name.empty() && ask.name != m_set.name) continue;

            bool known = false;
            for (Asker& a : m_askers_list)
                if (a.addr == from && a.port == fromPort)
                {
                    a.lastSeen = GetTickCount64();
                    known = true;
                    break;
                }
            if (!known)
            {
                Asker a;
                a.addr = from;
                a.port = fromPort;
                a.lastSeen = GetTickCount64();
                m_askers_list.push_back(a);
            }

            // Multicast discovery does not cross a subnet, so a receiver that
            // had to be given an address has no idea what it is about to
            // receive. The answer to the ask says so.
            if (ask.wantBeacon) SendBeaconTo(from, fromPort);
        }

        const unsigned long long now = GetTickCount64();
        for (size_t i = m_askers_list.size(); i > 0; --i)
        {
            const Asker& a = m_askers_list[i - 1];
            if (now > a.lastSeen &&
                now - a.lastSeen > (unsigned long long)net::kSubscribeExpiryMs)
                m_askers_list.erase(m_askers_list.begin() + (ptrdiff_t)(i - 1));
        }

        std::lock_guard<std::mutex> g(m_lock);
        m_askers = (int)m_askers_list.size();
    }

    void Fail(const wchar_t* why)
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_lastErr = why;
    }

    void Run()
    {
        const unsigned long long started = GetTickCount64();
        unsigned long long lastBeacon = 0;
        std::vector<float> chunk;

        while (m_running.load())
        {
            {
                std::unique_lock<std::mutex> g(m_lock);
                m_wake.wait_for(g, std::chrono::milliseconds(kWakeMs),
                                [this] { return !m_queue.empty() || !m_running.load(); });
                chunk.assign(m_queue.begin(), m_queue.end());
                m_queue.clear();
                m_elapsedMs = GetTickCount64() - started;
            }

            // Before the audio, so that a receiver which has just asked is
            // already on the list when this round of packets goes out.
            TakeAsks();

            if (!chunk.empty())
            {
                if (m_set.protocol == NP_VBAN) SendVban(chunk);
                else                           SendFastRoute(chunk);
            }

            // On a timer rather than per packet, and whether or not there is
            // any audio: a sender that has gone quiet is still there, and a
            // receiver that stopped seeing it would drop it off the list.
            const unsigned long long now = GetTickCount64();
            if (m_set.protocol == NP_FASTROUTE &&
                now - lastBeacon >= (unsigned long long)kBeaconEveryMs)
            {
                lastBeacon = now;
                SendBeacon();
            }
        }
    }

    net::Settings m_set;
    net::Udp m_sock;
    std::unique_ptr<net::Encoder> m_enc;

    int m_rate = kDefaultRate;
    int m_vbanRate = -1;
    int m_vbanFrames = 0;
    std::vector<float> m_scratch;        // one VBAN packet's worth
    std::vector<float> m_pending;        // VBAN's own part-filled packet

    unsigned long long m_frames = 0;     // frames sent, for the receiver's clock
    uint32_t m_seq = 0;

    std::deque<float> m_queue;
    mutable std::mutex m_lock;
    std::condition_variable m_wake;
    std::thread m_thread;
    std::atomic<bool> m_running{ false };

    std::vector<Asker> m_askers_list;      // the sender thread's own
    bool m_takesAsks = true;
    int  m_askers = 0;                     // under m_lock, for the status line

    unsigned long long m_packets = 0, m_bytes = 0, m_dropped = 0;
    unsigned long long m_elapsedMs = 0;
    std::wstring m_lastErr;
};

}  // namespace

std::unique_ptr<NetSink> MakeNetSink()
{
    return std::unique_ptr<NetSink>(new Sender());
}
