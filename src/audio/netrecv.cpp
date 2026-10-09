#include "net.h"
#include "netcodec.h"
#include "resample.h"
#include "util.h"

#include <mutex>
#include <thread>

// ---------------------------------------------------------------------------
// Net Receive
//
// A capture source like any other, so the rest of the graph neither knows nor
// cares that this one arrived over a network. What is different is everything
// about the clock: the audio comes in bursts, from a machine whose sound card
// is not this one and never will be exactly in step with it, over a link that
// drops things.
//
// Almost all of that is already handled. CaptureSource has a constructor meant
// for exactly this - a ring three times the chosen buffer, a working depth of
// one buffer, and a drift resampler that corrects for the two clocks pulling
// apart - and the stream block has been using it for internet radio since 0.1.
// So this block's own job is small: get packets off the wire, turn them into
// frames at the session rate, and put them in the ring.
//
// It also answers VBAN identification pings while it is running, which is how
// VoiceMeeter and the phone apps find a VBAN receiver. FastRoute announces its
// senders on a group and is found by listening; VBAN is found by asking, and
// this is the end that answers.
// ---------------------------------------------------------------------------

namespace {

const int kRecvTimeoutMs = 200;
const int kFollowEveryMs = 1000;

class Receiver : public CaptureSource
{
public:
    Receiver(const Block& b, int rate)
        : CaptureSource(rate, BufferMsOf(b)), m_block(b) {}

    ~Receiver() override { Stop(); }

    bool Start(std::wstring* err) override
    {
        if (!net::Resolve(m_block, false, &m_set, err)) return false;

        if (!m_sock.Open(err)) return false;

        // Two ways to get the audio, and which one this is depends on what it
        // was pointed at.
        //
        // A group is joined: the sender puts one stream on the group and
        // everybody who wants it joins, so the port has to be the one the
        // group's traffic is addressed to.
        //
        // A named host is asked. The request goes out from here, so the reply
        // comes back down a path the router has already opened and the port
        // can be any port at all - which means no port forwarding on this
        // side, and no clash with anything else on this machine.
        m_pull = (m_set.protocol == NP_FASTROUTE) && m_set.addr != 0 &&
                 !net::IsMulticast(m_set.addr);

        // Shared, because another program on this machine may be listening to
        // the same VBAN port, and because two sessions here may both want the
        // same group.
        if (!m_sock.Bind(m_pull ? 0 : m_set.port, true, err)) return false;
        m_sock.SetRecvBufferBytes(1024 * 1024);
        if (!m_pull && net::IsMulticast(m_set.addr) &&
            !m_sock.JoinGroup(m_set.addr, err))
            return false;
        m_joined = (!m_pull && net::IsMulticast(m_set.addr)) ? m_set.addr : 0;

        // Only FastRoute senders announce themselves, so only a FastRoute
        // receiver has anything to listen for.
        if (m_set.protocol == NP_FASTROUTE) m_listener.reset(new net::Listener());

        m_want = m_set.streamId;
        // How long a missing packet is waited for before the hole is filled
        // in. Half the buffer leaves the other half to absorb what the wait
        // itself costs; ten milliseconds is the floor, below which it is not
        // worth waiting at all.
        m_waitMs = m_set.bufferMs / 2;
        if (m_waitMs < 10) m_waitMs = 10;
        ResetFlow();
        m_stop = false;
        m_thread = std::thread([this] { Run(); });

        // Deliberately not waiting for audio. Nothing may be sending yet, and
        // a receiver whose sender is switched on later is the normal way round
        // rather than a failure.
        return true;
    }

    void Stop() override
    {
        m_stop = true;
        if (m_thread.joinable()) m_thread.join();
        m_sock.Close();
        m_listener.reset();
    }

    void Configure(const Block& b) override
    {
        // The buffer depth and the chosen sender are both baked into how this
        // was built, so KeyFor makes a change of either a new node. What is
        // left to push in live is nothing, but the block is kept so that
        // TargetName can say what was asked for.
        std::lock_guard<std::mutex> g(m_lock);
        m_block = b;
    }

    std::wstring TargetName() const override
    {
        std::lock_guard<std::mutex> g(m_lock);
        if (!m_lastErr.empty()) return m_lastErr;

        std::wstring who = m_set.name;
        if (who.empty()) who = m_heard;
        if (who.empty())
            who = m_from ? net::AddrText(m_from) : std::wstring(L"whatever arrives");

        if (m_packets == 0)
        {
            if (m_pull && !m_answered)
                return FormatW(L"asking %s for %s, no answer yet",
                               net::AddrText(m_set.addr, m_set.port).c_str(),
                               who.c_str());
            if (m_pull)
                return FormatW(L"%s answered; waiting for the audio", who.c_str());
            return FormatW(L"waiting for %s over %s", who.c_str(),
                           NetProtocolName(m_set.protocol));
        }

        std::wstring line = FormatW(L"%s from %s, %d Hz %s", who.c_str(),
                                    net::AddrText(m_from).c_str(), m_wireRate,
                                    net::NetCodecName(m_wireCodec));

        // What the network is actually doing, because on a link that is
        // misbehaving the only thing worse than the audio being wrong is not
        // being able to tell why. The loss figure counts frames rather than
        // packets, so it means the same thing whatever the packets are sized
        // at, and the jitter figure is the worst wait any packet has had -
        // which is the number to set the buffer from.
        const unsigned long long total = m_totalFrames + m_lostFrames;
        if (total > 0 && (m_lostFrames || m_late || m_reordered))
        {
            const double lostPct = 100.0 * (double)m_lostFrames / (double)total;
            line += FormatW(L"; %.2f%% lost", lostPct);
            if (m_reordered)
                line += FormatW(L", %llu out of order",
                                (unsigned long long)m_reordered);
            if (m_late)
                line += FormatW(L", %llu too late", (unsigned long long)m_late);
        }
        // Deliberately not called jitter: it is how long a packet had to be
        // held before its turn came, which after a lost one includes the wait
        // spent hoping it would still arrive. That makes it larger than the
        // network's own unevenness and exactly the right figure to size the
        // buffer from, which is what anybody reading it wants it for.
        if (m_worstWait > 0)
            line += FormatW(L"; held up to %d ms", m_worstWait);
        if (m_resets) line += FormatW(L"; restarted %llu times",
                                      (unsigned long long)m_resets);
        return line;
    }

private:
    static int BufferMsOf(const Block& b)
    {
        int ms = b.netBufferMs;
        if (ms < kNetBufferMsMin) ms = kNetBufferMsMin;
        if (ms > kNetBufferMsMax) ms = kNetBufferMsMax;
        return ms;
    }

    // Everything that turns a packet's payload into frames at the session
    // rate. Rebuilt whenever the far end changes what it is sending, which it
    // is allowed to do under way.
    bool PrepareFor(int codec, int wireRate)
    {
        if (m_dec && codec == m_wireCodec && wireRate == m_wireRate) return true;

        std::wstring err;
        m_dec = net::MakeDecoder(codec, wireRate, &err);
        if (!m_dec)
        {
            Note(err.empty() ? L"that stream is in a format this cannot read" : err);
            return false;
        }
        if (!m_conv.Prepare(wireRate, m_rate))
        {
            Note(L"the rate converter could not be created");
            return false;
        }
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_wireCodec = codec;
            m_wireRate = wireRate;
            m_lastErr.clear();
        }
        // Deliberately not resetting the sequencing: the sender's frame
        // counter runs straight through a change of codec, and so does this.
        return true;
    }

    void Deliver(std::vector<float>& wire)
    {
        if (wire.empty()) return;
        m_converted.clear();
        m_conv.Run(wire.data(), wire.size() / kChannels, m_converted);
        if (!m_converted.empty())
        {
            m_ring.Write(m_converted.data(), m_converted.size() / kChannels);
            NotePacket((unsigned)(m_converted.size() / kChannels));
        }
        wire.clear();
    }

    void Note(const std::wstring& why)
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_lastErr = why;
    }

    void Seen(uint32_t from)
    {
        std::lock_guard<std::mutex> g(m_lock);
        m_from = from;
        ++m_packets;
    }

    // --- the jitter buffer --------------------------------------------------
    //
    // Packets are held here until it is their turn, rather than being used the
    // moment they land. Three things make that necessary on a network nobody
    // is looking after.
    //
    // They arrive out of order. UDP is allowed to reorder, and a wireless link
    // or anything with more than one path through it does. Using a packet the
    // moment it lands means a packet that overtook its neighbour gets played
    // early and the one it overtook gets thrown away as "old" - so one
    // reordering costs two packets instead of none.
    //
    // They arrive late. A packet three milliseconds behind its neighbour is
    // not lost, and with sixty milliseconds of buffer there is no reason to
    // give up on it after three. So a gap waits for a deadline before it is
    // declared missing.
    //
    // And when one really is missing, the hole has to be exactly the right
    // length. This is the part that was wrong, and it was not a small thing:
    // the ring in front of the mixer works out the two machines' clock drift
    // from how many frames went in against how many came out, and corrects it
    // by resampling. Frames quietly dropped instead of replaced look exactly
    // like a sender running slow, so the correction engages and the whole
    // session is resampled - which is heard as the pitch wandering about. The
    // sender's own frame counter is in every packet, so the size of a gap is
    // known exactly rather than estimated, and filling it exactly is what
    // keeps the pitch still.
    struct Held
    {
        uint32_t seq = 0;
        unsigned long long arrived = 0;
        std::vector<unsigned char> bytes;
    };

    // A ceiling on how many can be waiting, so that a sender talking into a
    // hole does not grow this without end. The deadline is the real control.
    static const size_t kMaxHeld = 192;

    void ResetFlow()
    {
        m_held.clear();
        m_flowing = false;
        m_next = 0;
        m_expect = 0;
        m_highest = 0;
    }

    // A silence long enough to mean the sender stopped, rather than that the
    // network hiccuped. Concealing across one of those would insert however
    // many seconds of it, so the stream is restarted instead.
    void CheckStall(unsigned long long now)
    {
        if (!m_flowing || m_lastArrival == 0) return;
        // Generous on purpose. A gap of a few hundred milliseconds is a
        // network hiccup, and is concealed; this is for a sender that has gone
        // away. It also has to tolerate a sender using Opus discontinuous
        // transmission, which deliberately sends almost nothing while its
        // input is silent - a short threshold would restart the stream every
        // time somebody stopped talking.
        const unsigned long long stallMs =
            (unsigned long long)(m_set.bufferMs * 5 > 3000 ? m_set.bufferMs * 5 : 3000);
        if (now - m_lastArrival <= stallMs) return;

        ResetFlow();
        m_ring.Clear();
        m_ring.PrimeSilence(m_prime);
        std::lock_guard<std::mutex> g(m_lock);
        ++m_resets;
    }

    void Enqueue(uint32_t seq, const unsigned char* buf, int got,
                 unsigned long long atFrames)
    {
        const unsigned long long now = GetTickCount64();
        m_lastArrival = now;

        if (!m_flowing)
        {
            m_flowing = true;
            m_next = seq;
            m_expect = atFrames;
            m_highest = seq;
        }

        // Older than what has already been played. Not usable: putting it in
        // now would play it after audio that comes later.
        if (seq < m_next)
        {
            std::lock_guard<std::mutex> g(m_lock);
            ++m_late;
            return;
        }
        for (const Held& h : m_held)
            if (h.seq == seq)
            {
                std::lock_guard<std::mutex> g(m_lock);
                ++m_dupes;
                return;
            }

        if (seq < m_highest)
        {
            std::lock_guard<std::mutex> g(m_lock);
            ++m_reordered;      // behind one already here, but still in time
        }
        if (seq > m_highest) m_highest = seq;

        Held h;
        h.seq = seq;
        h.arrived = now;
        h.bytes.assign(buf, buf + got);

        // Kept in order, so the one at the front is always the oldest turn
        // and the deadline only has to be checked once.
        size_t at = 0;
        while (at < m_held.size() && m_held[at].seq < seq) ++at;
        m_held.insert(m_held.begin() + (ptrdiff_t)at, std::move(h));
    }

    void Drain(unsigned long long now)
    {
        for (;;)
        {
            size_t found = m_held.size();
            for (size_t i = 0; i < m_held.size(); ++i)
                if (m_held[i].seq == m_next) { found = i; break; }

            if (found < m_held.size())
            {
                const unsigned long long waited = now > m_held[found].arrived
                    ? now - m_held[found].arrived : 0;
                if ((int)waited > m_worstWait)
                {
                    std::lock_guard<std::mutex> g(m_lock);
                    m_worstWait = (int)waited;
                }
                std::vector<unsigned char> bytes = std::move(m_held[found].bytes);
                m_held.erase(m_held.begin() + (ptrdiff_t)found);
                ++m_next;
                Play(bytes.data(), (int)bytes.size());
                continue;
            }

            if (m_held.empty()) break;

            // Its turn has come and it is not here. Wait a while - most late
            // packets are only a little late - and give up when the one behind
            // it has been waiting longer than the deadline, or when too many
            // are stacked up behind it to keep holding them.
            const unsigned long long waited = now > m_held.front().arrived
                ? now - m_held.front().arrived : 0;
            if ((int)waited < m_waitMs && m_held.size() <= kMaxHeld) break;

            // Skipped rather than filled in here: the next packet that does
            // play carries the sender's frame counter, which says exactly how
            // much is missing, and Play puts in precisely that much.
            ++m_next;
        }
    }

    // What to put where a gap was, and exactly how much of it.
    void FillGap(unsigned long long gapFrames, const unsigned char* next,
                 int nextBytes, int onePacket)
    {
        if (gapFrames == 0) return;

        // Opus can rebuild one lost packet from the next one when the sender
        // was asked to carry the redundancy, which is better than guessing and
        // costs nothing to ask for. Only one packet back, and only the one
        // immediately before, so it is asked when the gap is exactly a packet
        // and not otherwise.
        //
        // Whether the redundancy was really there cannot be known from here -
        // Opus conceals instead and says nothing - so this is counted as lost
        // either way. It was lost; what differs is how well it was covered.
        bool filled = false;
        if (m_dec && onePacket > 0 && gapFrames == (unsigned long long)onePacket)
            filled = m_dec->DecodeFec(next, nextBytes, onePacket, m_wire);

        if (!filled && m_dec) m_dec->Conceal((int)gapFrames, m_wire);

        std::lock_guard<std::mutex> g(m_lock);
        m_lostFrames += gapFrames;
    }

    void Play(const unsigned char* buf, int got)
    {
        if (m_set.protocol == NP_VBAN) PlayVban(buf, got);
        else                           PlayFastRoute(buf, got);
    }

    // --- FastRoute ---------------------------------------------------------

    void PlayFastRoute(const unsigned char* buf, int got)
    {
        net::DataHeader h;
        if (!net::ParseDataHeader(buf, got, &h)) return;
        if (!PrepareFor(h.codec, (int)h.rate)) return;

        const unsigned char* payload = buf + net::kDataHeaderBytes;

        // The sender counts the frames it has produced, so the two counters
        // say exactly how much never arrived - whatever the packets were
        // sized at, and however many of them went missing.
        if (h.frames > m_expect)
        {
            const unsigned long long gap = h.frames - m_expect;
            // Beyond a sane amount this is a sender that restarted rather than
            // a network that lost that much, and filling it would insert
            // minutes of audio.
            const unsigned long long sane = (unsigned long long)m_rate;   // one second
            if (gap > sane)
            {
                ResetFlow();
                m_ring.Clear();
                m_ring.PrimeSilence(m_prime);
                std::lock_guard<std::mutex> g(m_lock);
                ++m_resets;
                return;
            }
            FillGap(gap, payload, h.payloadBytes, (int)h.frameCount);
        }
        else if (h.frames + h.frameCount <= m_expect)
        {
            // Entirely behind what has already been delivered.
            std::lock_guard<std::mutex> g(m_lock);
            ++m_late;
            return;
        }

        if (!m_dec->Decode(payload, h.payloadBytes, h.frameCount, m_wire)) return;
        m_expect = h.frames + (unsigned long long)h.frameCount;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_totalFrames += h.frameCount;
        }
        Deliver(m_wire);
        Seen(m_from ? m_from : 0);
    }

    void TakeFastRoute(const unsigned char* buf, int got, uint32_t from)
    {
        net::DataHeader h;
        if (!net::ParseDataHeader(buf, got, &h)) return;

        // Which stream. A block that was pointed at one takes only that one; a
        // block that was not locks onto the first it hears, so that naming an
        // address and nothing else still works.
        if (m_want == 0) m_want = h.streamId;
        if (h.streamId != m_want) return;

        {
            std::lock_guard<std::mutex> g(m_lock);
            m_from = from;
        }
        Enqueue(h.seq, buf, got, h.frames);
    }

    // --- VBAN --------------------------------------------------------------

    void PlayVban(const unsigned char* buf, int got)
    {
        net::vban::Header h;
        if (!net::vban::ParseHeader(buf, got, &h)) return;

        const int per = net::VbanBytesPerSample(h.datatype);
        const int wireRate = net::vban::RateAt(h.rateIndex);
        if (per <= 0 || wireRate <= 0) return;
        if (got < net::vban::kHeaderBytes + (int)h.frames * (int)h.channels * per)
            return;

        // No decoder object: VBAN audio is interleaved PCM behind a header and
        // VbanToFloat is the whole of it. The rate converter is still wanted,
        // because a VBAN sender at 44.1 kHz is ordinary.
        if (!m_conv.Prepare(wireRate, m_rate))
        {
            Note(L"the rate converter could not be created");
            return;
        }
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_wireRate = wireRate;
            m_wireCodec = net::NC_PCM16;
            m_lastErr.clear();
        }

        // VBAN counts packets rather than frames - its counter goes up by one
        // per packet, as the specification's own example does - so a gap is
        // that many packets of whatever this one is carrying. Every VBAN
        // sender uses one packet size throughout, which is what makes that
        // sound rather than approximate.
        if (h.counter > m_expect)
        {
            const unsigned long long missing = h.counter - m_expect;
            const unsigned long long gap = missing * (unsigned long long)h.frames;
            if (gap > (unsigned long long)m_rate)
            {
                ResetFlow();
                m_ring.Clear();
                m_ring.PrimeSilence(m_prime);
                std::lock_guard<std::mutex> g(m_lock);
                ++m_resets;
                return;
            }
            // Nothing to rebuild it from: VBAN is PCM, and a packet that did
            // not arrive leaves nothing behind. Silence of the right length is
            // all there is, and the right length is the point.
            m_wire.insert(m_wire.end(), (size_t)gap * kChannels, 0.0f);
            std::lock_guard<std::mutex> g(m_lock);
            m_lostFrames += gap;
        }

        if (!net::VbanToFloat(h.datatype, buf + net::vban::kHeaderBytes,
                              (int)h.frames, (int)h.channels, m_wire))
            return;
        m_expect = (unsigned long long)h.counter + 1;
        {
            std::lock_guard<std::mutex> g(m_lock);
            m_totalFrames += h.frames;
        }
        Deliver(m_wire);
        Seen(m_from ? m_from : 0);
    }

    void TakeVban(const unsigned char* buf, int got, uint32_t from, uint16_t fromPort)
    {
        net::vban::Header h;
        if (!net::vban::ParseHeader(buf, got, &h)) return;

        if (h.protocol == net::vban::PROTOCOL_SERVICE)
        {
            AnswerPing(h, buf, got, from, fromPort);
            return;
        }
        if (h.protocol != net::vban::PROTOCOL_AUDIO) return;

        // A VBAN stream is identified by its name, not by any id: that is how
        // one VoiceMeeter sends eight streams to one port. An empty name here
        // means take whatever arrives.
        if (!m_set.name.empty() && h.name != m_set.name) return;

        if (h.codec != net::vban::CODEC_PCM)
        {
            Note(FormatW(L"\"%s\" is using one of VB-Audio's own codecs, which "
                         L"this cannot decode. Set the sender to PCM.",
                         h.name.c_str()));
            return;
        }
        if (net::VbanBytesPerSample(h.datatype) <= 0)
        {
            Note(FormatW(L"\"%s\" is in a sample format this does not handle "
                         L"(VBAN data type %d).", h.name.c_str(), (int)h.datatype));
            return;
        }

        {
            std::lock_guard<std::mutex> g(m_lock);
            m_from = from;
        }
        Enqueue(h.counter, buf, got, (unsigned long long)h.counter);
    }

    void AnswerPing(const net::vban::Header& h, const unsigned char* buf, int got,
                    uint32_t from, uint16_t fromPort)
    {
        // Both counts arrive one more than the wire said, because VBAN stores
        // them that way for audio and reuses the fields here.
        const int service = (int)h.frames - 1;
        const int fnct = (int)h.channels - 1;
        if (service != net::vban::SERVICE_IDENTIFICATION) return;
        if (fnct & net::vban::FNCT_REPLY) return;      // somebody else's answer
        (void)buf;
        (void)got;

        unsigned char out[net::vban::kHeaderBytes + net::vban::kPing0Bytes];
        net::vban::Header r;
        r.protocol = net::vban::PROTOCOL_SERVICE;
        r.rateIndex = 0;
        r.frames = net::vban::SERVICE_IDENTIFICATION + 1;
        r.channels = net::vban::FNCT_REPLY + 1;
        r.datatype = 0;
        r.codec = 0;
        r.name = m_set.name.empty() ? std::wstring(L"FastRoute") : m_set.name;
        // The same ident the question carried, so the asker can match the
        // answer to it.
        r.counter = h.counter;
        net::vban::PackHeader(r, out);

        net::vban::Ping0 me = net::vban::OurPing0();
        me.preferredRate = (uint32_t)m_rate;
        if (!m_set.name.empty()) me.deviceName = m_set.name;
        net::vban::PackPing0(me, out + net::vban::kHeaderBytes);

        m_sock.Send(out, (int)sizeof(out), from, fromPort);
    }

    // Asking the sender to send. Repeated rather than said once, because the
    // sender forgets an asker that has gone quiet - which is how it knows to
    // stop sending to a receiver that was closed without saying so, and how a
    // sender that was restarted picks this receiver up again without anybody
    // touching either end.
    void Ask()
    {
        if (!m_pull) return;
        net::Subscribe ask;
        ask.wantStreamId = m_set.streamId;
        ask.name = m_set.name;
        // Multicast discovery does not cross a subnet, so this is the only way
        // a pulled stream can say what it is.
        ask.wantBeacon = true;

        unsigned char buf[net::kSubscribeBytes];
        net::PackSubscribe(ask, buf);
        if (m_sock.Send(buf, sizeof(buf), m_set.addr, m_set.port) < 0)
            Note(FormatW(L"could not reach %s",
                         net::AddrText(m_set.addr, m_set.port).c_str()));
    }

    // The answer to an ask: what that sender is, which is what the session
    // window shows while waiting for the first audio.
    void TakeBeacon(const unsigned char* buf, int got)
    {
        net::Beacon b;
        if (!net::ParseBeacon(buf, got, &b)) return;
        std::lock_guard<std::mutex> g(m_lock);
        m_wireCodec = b.codec;
        if (b.rate > 0) m_wireRate = (int)b.rate;
        if (m_set.name.empty() && !b.name.empty()) m_heard = b.name;
        m_answered = true;
    }

    // A sender that is restarted comes back with a new stream id, and somebody
    // who chose "Desk mic" yesterday means the thing called Desk mic today. So
    // the name is followed rather than the number.
    void FollowByName()
    {
        if (!m_listener || m_set.name.empty()) return;
        net::Peer p;
        if (!m_listener->Find(0, m_set.name, &p)) return;
        if (p.streamId == 0 || p.streamId == m_want) return;

        m_want = p.streamId;
        ResetFlow();
        // It may also have moved to a different group, which is a join rather
        // than a rebind as long as the port has not changed.
        if (p.group && p.group != m_joined && p.port == m_set.port)
        {
            if (m_sock.JoinGroup(p.group, nullptr)) m_joined = p.group;
        }
    }

    void Run()
    {
        m_ring.PrimeSilence(m_prime);

        unsigned char buf[2048];
        unsigned long long lastFollow = 0;
        unsigned long long lastAsk = 0;

        while (!m_stop.load())
        {
            uint32_t from = 0;
            uint16_t fromPort = 0;
            const int got = m_sock.Recv(buf, sizeof(buf), kRecvTimeoutMs,
                                        &from, &fromPort);
            if (got > 0)
            {
                if (m_set.protocol == NP_VBAN) TakeVban(buf, got, from, fromPort);
                else if (got >= net::kBeaconBytes &&
                         memcmp(buf, "FRNB", 4) == 0) TakeBeacon(buf, got);
                else TakeFastRoute(buf, got, from);
            }

            const unsigned long long now = GetTickCount64();

            // Every pass, not only when something arrived: a packet whose turn
            // has come and gone has to be given up on even if nothing else is
            // turning up either.
            Drain(now);
            CheckStall(now);

            if (m_pull && now - lastAsk >= (unsigned long long)net::kSubscribeEveryMs)
            {
                lastAsk = now;
                Ask();
            }
            if (now - lastFollow >= (unsigned long long)kFollowEveryMs)
            {
                lastFollow = now;
                FollowByName();
            }
        }
    }

    Block m_block;
    net::Settings m_set;
    net::Udp m_sock;
    uint32_t m_joined = 0;
    std::unique_ptr<net::Listener> m_listener;

    std::unique_ptr<net::Decoder> m_dec;
    Converter m_conv;
    std::vector<float> m_wire;           // at the wire's rate
    std::vector<float> m_converted;      // at the session's rate

    uint32_t m_want = 0;                 // the stream id being taken

    // The jitter buffer, all owned by the socket thread.
    std::vector<Held> m_held;
    bool m_flowing = false;
    uint32_t m_next = 0;                 // the sequence number wanted next
    uint32_t m_highest = 0;              // the highest seen, for spotting reorder
    unsigned long long m_expect = 0;     // the sender's own count, where we are
    unsigned long long m_lastArrival = 0;
    int m_waitMs = 30;

    mutable std::mutex m_lock;
    bool m_pull = false;                 // asking a host, rather than joining a group
    bool m_answered = false;             // it has told us what it is
    std::wstring m_heard;                // the name it gave itself
    uint32_t m_from = 0;
    int m_wireRate = 0;
    int m_wireCodec = net::NC_PCM16;
    unsigned long long m_packets = 0;
    unsigned long long m_totalFrames = 0, m_lostFrames = 0;
    unsigned long long m_late = 0, m_dupes = 0, m_reordered = 0, m_resets = 0;
    int m_worstWait = 0;
    std::wstring m_lastErr;
};

}  // namespace

std::unique_ptr<CaptureSource> MakeNetCapture(const Block& b, int rate)
{
    return std::unique_ptr<CaptureSource>(new Receiver(b, rate));
}
