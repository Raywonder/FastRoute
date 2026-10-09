#pragma once
#include "audio.h"

// ---------------------------------------------------------------------------
// Networked audio: sockets, wire formats and discovery
//
// Two protocols live here side by side, because they answer two different
// questions.
//
// FastRoute's own protocol is for talking to another copy of FastRoute. It
// announces itself on a multicast group, so a Net Receive block can list the
// senders it has heard by name rather than asking anybody to type an address,
// and its header has room for the codecs this program actually has: PCM at
// three depths, FLAC and Opus.
//
// VBAN is VB-Audio's protocol, which VoiceMeeter, VBAN Receptor on a phone and
// a good deal of other software already speak, and supporting it is what makes
// this useful outside FastRoute. Its codec field has slots only for PCM and
// two of VB-Audio's own codecs, so a VBAN stream from here is PCM; and it is
// discovered by asking rather than by listening, through a ping that every
// VBAN host answers.
//
// Nothing in here opens an audio device or touches a window. Winsock is
// started on demand and counted, so a session with four net blocks starts it
// once and stops it once.
// ---------------------------------------------------------------------------

#include <cstdint>

namespace net {

// Winsock, reference counted. Every socket holds one for its lifetime.
bool Startup(std::wstring* err);
void Shutdown();

// ---------------------------------------------------------------------------
// Addresses
//
// IPv4 only, deliberately. Multicast discovery, VBAN and the whole idea of a
// device on "the network" here all mean a LAN, and a dual stack implementation
// would double the surface for a case nobody has asked for.
// ---------------------------------------------------------------------------

std::wstring AddrText(uint32_t v4);                   // "192.168.1.40"
std::wstring AddrText(uint32_t v4, uint16_t port);    // "192.168.1.40:6982"

// "192.168.1.40", "192.168.1.40:6000", "deskpc" or "" for none. A name is
// resolved, which blocks, so this is never called from a render thread.
bool ParseHost(const std::wstring& text, uint16_t defPort,
               uint32_t* v4, uint16_t* port, std::wstring* err);

bool IsMulticast(uint32_t v4);        // 224.0.0.0/4

// Every up IPv4 interface on this machine: for joining a group on all of them,
// and for saying where we are.
std::vector<uint32_t> LocalAddresses();
std::wstring HostName();

// ---------------------------------------------------------------------------
// A UDP socket
//
// Thin on purpose - enough to bind, join a group, send and receive with a
// timeout, and nothing that hides an error. The handle is kept as an integer
// so that winsock2.h stays inside net.cpp.
// ---------------------------------------------------------------------------
class Udp
{
public:
    Udp() {}
    ~Udp();
    Udp(const Udp&) = delete;
    Udp& operator=(const Udp&) = delete;

    bool Open(std::wstring* err);
    // Shared, because two sessions on one machine may both want to listen to
    // the same group, and so may another program entirely.
    bool Bind(uint16_t port, bool shared, std::wstring* err);
    // Joins on every local interface rather than letting the stack pick one.
    // A machine with Wi-Fi, Ethernet and a virtual adapter would otherwise
    // listen on whichever the routing table happened to prefer, which is not
    // reliably the one the audio is arriving on.
    bool JoinGroup(uint32_t group, std::wstring* err);
    // The hop limit for packets sent to a group, and only for those. The
    // unicast TTL is deliberately left at the system default: it is not the
    // same question, and the answer that is right for a group - one hop, stay
    // on this subnet - stops a packet to a named host at the first router.
    bool SetMulticastTtl(int ttl);
    // Read back, so that a check can tell the two apart rather than trust
    // that they were set separately.
    int MulticastTtl() const;
    int UnicastTtl() const;
    bool SetMulticastLoopback(bool on);
    // Needed to ask every host on the subnet a question at once, which is how
    // a VBAN host is found: it answers a request rather than announcing itself.
    bool SetBroadcast(bool on);
    bool SetSendBufferBytes(int bytes);
    bool SetRecvBufferBytes(int bytes);
    uint16_t LocalPort() const;

    int Send(const void* data, int len, uint32_t v4, uint16_t port);  // <0 fails
    // <0 fails, 0 means the timeout passed with nothing waiting.
    int Recv(void* data, int len, int timeoutMs, uint32_t* fromV4, uint16_t* fromPort);

    void Close();
    bool Valid() const { return m_s != kInvalid; }

private:
    static const uintptr_t kInvalid = (uintptr_t)~(uintptr_t)0;
    uintptr_t m_s = kInvalid;
    bool m_winsock = false;
    std::vector<uint32_t> m_joined;
};

// ---------------------------------------------------------------------------
// FastRoute's own protocol
// ---------------------------------------------------------------------------

// 239.255.0.0/16 is the administratively scoped block - the right place for
// something that must not leave the site.
const uint32_t kBeaconGroup = 0xEFFF4D14u;    // 239.255.77.20
const uint16_t kBeaconPort  = 6981;
const uint32_t kDataGroup   = 0xEFFF4D15u;    // 239.255.77.21
const uint16_t kDataPort    = 6982;

// What a packet carries. Stored in a session file, so the order is fixed once
// written.
enum NetCodec
{
    NC_PCM16 = 0,
    NC_PCM24 = 1,
    NC_PCM_FLOAT = 2,
    NC_FLAC = 3,
    NC_OPUS = 4,
};
static const int kNetCodecCount = 5;

const wchar_t* NetCodecName(int c);           // "16 bit PCM"
bool NetCodecIsLossless(int c);
// Roughly what a stereo stream costs, so the dialog can say it out loud before
// somebody saturates a Wi-Fi link by accident.
int NetCodecKbps(int c, int rate, int opusKbps);

// --- the announcement ------------------------------------------------------
//
// Sent to the beacon group about twice a second by every running sender. Fixed
// layout, little endian, so a future version can be recognised and ignored
// rather than misread.
static const int kBeaconBytes = 96;

struct Beacon
{
    uint32_t streamId = 0;        // stable for the life of the block
    uint32_t group = 0;           // data group, or 0 for "unicast, ask me"
    uint16_t port = kDataPort;
    uint8_t  codec = NC_PCM16;
    uint8_t  channels = (uint8_t)kChannels;
    uint32_t rate = 0;
    uint16_t packetFrames = 0;
    std::wstring name;            // stream name, up to 31 ASCII
    std::wstring host;            // machine name, up to 31 ASCII
};

void PackBeacon(const Beacon& b, unsigned char out[kBeaconBytes]);
bool ParseBeacon(const unsigned char* p, int len, Beacon* out);

// --- the audio -------------------------------------------------------------
static const int kDataHeaderBytes = 32;
// One packet stays inside a 1500 byte MTU, for the same reason VBAN does: a
// fragmented audio packet is one the network can lose in halves.
static const int kMaxPayloadBytes = 1400;

// --- asking for it ---------------------------------------------------------
//
// The piece that lets a receiver go and get the audio rather than wait to be
// given it. A group needs none of this: a sender puts one stream on the group
// and a receiver joins it, which is already the receiver doing the asking. A
// named host is the other case, and without this the sender would have to be
// told every receiver's address in advance - which is backwards, since the
// receiver is the one that knows who it wants to listen to.
//
// So a receiver pointed at a host sends one of these to it every second, and
// the sender adds whoever asked to the list it is sending to. Stop asking and
// the sender forgets you, which is also what happens when a receiver is closed
// without saying so.
//
// It gets through a home router in the bargain. The request is outbound from
// the receiver, so the reply comes back down a path the router has already
// opened, and no port has to be forwarded on the receiving side.
static const int kSubscribeBytes = 48;
// Renewed every second; forgotten after this long without one.
static const int kSubscribeEveryMs = 1000;
static const int kSubscribeExpiryMs = 6000;

struct Subscribe
{
    uint32_t wantStreamId = 0;    // 0 for whatever that sender is sending
    std::wstring name;            // or by name, which is what somebody chose
    bool wantBeacon = true;       // answer with what you are, so I can say so
};

void PackSubscribe(const Subscribe& s, unsigned char out[kSubscribeBytes]);
bool ParseSubscribe(const unsigned char* p, int len, Subscribe* out);

struct DataHeader
{
    uint8_t  codec = NC_PCM16;
    uint8_t  channels = (uint8_t)kChannels;
    uint32_t rate = 0;
    uint32_t streamId = 0;
    uint32_t seq = 0;             // counts packets, for spotting loss
    uint64_t frames = 0;          // frames before this packet, for the clock
    uint16_t frameCount = 0;      // frames in this packet
    uint16_t payloadBytes = 0;
};

void PackDataHeader(const DataHeader& h, unsigned char out[kDataHeaderBytes]);
bool ParseDataHeader(const unsigned char* p, int len, DataHeader* out);

// ---------------------------------------------------------------------------
// VBAN
//
// Layout from VB-Audio's VBAN Protocol specification, revision 13: a 28 byte
// header of 'VBAN', four format bytes, a 16 character stream name and a 32 bit
// frame counter, little endian throughout.
// ---------------------------------------------------------------------------
namespace vban {

const uint16_t kPort = 6980;
static const int kHeaderBytes = 28;
static const int kMaxDataBytes = 1436;      // 1500 MTU less 64, as Voicemeeter does
static const int kMaxFrames = 256;          // the nbs field is frames - 1
static const int kNameBytes = 16;

// Sub protocol, in bits 5 to 7 of format_SR.
enum
{
    PROTOCOL_AUDIO   = 0x00,
    PROTOCOL_SERIAL  = 0x20,
    PROTOCOL_TXT     = 0x40,
    PROTOCOL_SERVICE = 0x60,
    PROTOCOL_USER    = 0xE0,
};
static const uint8_t kProtocolMask = 0xE0;
static const uint8_t kRateMask = 0x1F;

// Data type, in bits 0 to 2 of format_bit.
enum
{
    DATATYPE_BYTE8   = 0x00,
    DATATYPE_INT16   = 0x01,
    DATATYPE_INT24   = 0x02,
    DATATYPE_INT32   = 0x03,
    DATATYPE_FLOAT32 = 0x04,
    DATATYPE_FLOAT64 = 0x05,
};
static const uint8_t kDatatypeMask = 0x07;

// Codec, in bits 4 to 7 of format_bit. VBCA and VBCV are VB-Audio's own and
// are not implemented here, which is why a VBAN stream from FastRoute is PCM.
enum
{
    CODEC_PCM  = 0x00,
    CODEC_VBCA = 0x10,
    CODEC_VBCV = 0x20,
    CODEC_USER = 0xF0,
};
static const uint8_t kCodecMask = 0xF0;

// The service sub protocol, which is how VBAN hosts are found: a host is sent
// an IDENTIFICATION request and answers with its own details.
enum { SERVICE_IDENTIFICATION = 0, SERVICE_CHATUTF8 = 1,
       SERVICE_RTPACKETREGISTER = 32, SERVICE_RTPACKET = 33 };
enum { FNCT_PING0 = 0x00, FNCT_REPLY = 0x80 };

// VBAN carries the rate as an index into a fixed table, so a rate that is not
// in the table cannot be sent at all.
int RateIndex(int rate);        // -1 when VBAN has no index for it
int RateAt(int index);          // 0 when out of range
static const int kRateCount = 21;

struct Header
{
    uint8_t  protocol = PROTOCOL_AUDIO;
    uint8_t  rateIndex = 0;      // or the service type/function, see below
    uint16_t frames = 1;         // 1 to 256; the service type for SERVICE
    uint16_t channels = 1;       // 1 to 256; the service function for SERVICE
    uint8_t  datatype = DATATYPE_INT16;
    uint8_t  codec = CODEC_PCM;
    std::wstring name;           // 16 ASCII, null padded
    uint32_t counter = 0;
};

void PackHeader(const Header& h, unsigned char out[kHeaderBytes]);
bool ParseHeader(const unsigned char* p, int len, Header* out);

// The identification reply's payload. Every field VB-Audio defines is here so
// that the size is right - a host only answers a ping whose PING0 is present,
// even when it is all zero - but only a handful are filled in.
static const int kPing0Bytes = 676;

enum
{
    PING_TYPE_RECEPTOR      = 0x00000001,
    PING_TYPE_TRANSMITTER   = 0x00000002,
    PING_TYPE_RECEPTORSPOT  = 0x00000004,
    PING_TYPE_TRANSMITTERSPOT = 0x00000008,
    PING_TYPE_VIRTUALDEVICE = 0x00000010,
    PING_TYPE_VIRTUALMIXER  = 0x00000020,
    PING_TYPE_MATRIX        = 0x00000040,
    PING_TYPE_DAW           = 0x00000080,
    PING_TYPE_SERVER        = 0x01000000,
};
enum
{
    PING_FEATURE_AUDIO  = 0x00000001,
    PING_FEATURE_AOIP   = 0x00000002,
    PING_FEATURE_VOIP   = 0x00000004,
    PING_FEATURE_SERIAL = 0x00000100,
    PING_FEATURE_MIDI   = 0x00000300,
    PING_FEATURE_FRAME  = 0x00001000,
    PING_FEATURE_TXT    = 0x00010000,
};

struct Ping0
{
    uint32_t bitType = 0;
    uint32_t bitFeature = 0;
    uint32_t bitFeatureEx = 0;
    uint32_t preferredRate = 0;
    uint32_t minRate = 0;
    uint32_t maxRate = 0;
    uint32_t colorRgb = 0;
    uint8_t  version[4] = { 0, 0, 0, 0 };
    std::wstring deviceName;        // 64 ASCII
    std::wstring manufacturer;      // 64 ASCII
    std::wstring application;       // 64 ASCII
    std::wstring hostName;          // 64 ASCII
    std::wstring userName;          // 128 UTF-8
    std::wstring userComment;       // 128 UTF-8
};

void PackPing0(const Ping0& p, unsigned char out[kPing0Bytes]);
bool ParsePing0(const unsigned char* p, int len, Ping0* out);

// What FastRoute says about itself when asked.
Ping0 OurPing0();

}  // namespace vban

// ---------------------------------------------------------------------------
// Who is out there
//
// One listener for the whole program, started while something cares and
// stopped when nothing does. It hears FastRoute beacons on their group, and
// sends VBAN identification pings and collects the replies, so the two kinds
// of sender arrive in the same list.
// ---------------------------------------------------------------------------
struct Peer
{
    bool     vbanHost = false;      // answered a VBAN ping rather than beaconing
    uint32_t streamId = 0;
    uint32_t from = 0;              // where it was heard from
    uint32_t group = 0;             // its data group, 0 for unicast only
    uint16_t port = 0;
    int      codec = NC_PCM16;
    int      rate = 0;
    int      channels = 2;
    int      packetFrames = 0;
    std::wstring name;
    std::wstring host;
    unsigned long long lastSeen = 0;   // GetTickCount64 when last heard

    // "Desk mic on DESKPC - Opus, 48 kHz stereo"
    std::wstring Label() const;
    int SecondsAgo() const;
};

// Held for as long as something wants to know. The dialog holds one while it
// is open; a receive block holds one so it can follow a sender that moves.
class Listener
{
public:
    Listener();
    ~Listener();
    Listener(const Listener&) = delete;
    Listener& operator=(const Listener&) = delete;

    // Everything heard in the last half minute, best first.
    std::vector<Peer> Peers() const;
    // The one with this stream id, or with this name when the id has changed
    // because the far end restarted. False when it has not been heard.
    bool Find(uint32_t streamId, const std::wstring& name, Peer* out) const;
    // Asks the VBAN hosts to identify themselves again, rather than waiting
    // for the next round of pings.
    void Poke();
    std::wstring Error() const;    // empty while it is working

private:
    std::unique_ptr<struct ListenerState> m_st;
};

// ---------------------------------------------------------------------------
// What a block's stored answers come to
//
// One place, so that the sender, the receiver and the dialog cannot come to
// disagree about what a given block actually means. The defaults are filled in
// here, the numbers are clamped here, and the address is resolved here - which
// is also the one place that can refuse a block with a reason worth reading.
// ---------------------------------------------------------------------------
// Everything Opus is told, gathered so that it travels as one thing rather
// than as six more arguments on every call.
struct OpusOptions
{
    int kbps = 128;
    int lossPct = 5;
    int complexity = 10;
    int signal = NOS_AUTO;
    int rateMode = NOR_VBR;
    bool dtx = false;
};

struct Settings
{
    int protocol = NP_FASTROUTE;
    int mode = NM_MULTICAST;
    uint32_t addr = 0;              // the group to use, or the host to send to
    uint16_t port = kDataPort;
    std::wstring name;
    int codec = NC_PCM16;
    int packetMs = 5;
    OpusOptions opus;
    int ttl = 1;
    int bufferMs = 60;
    uint32_t streamId = 0;          // ours when sending, wanted when receiving

    bool Multicast() const { return IsMulticast(addr); }
};

bool Resolve(const Block& b, bool sending, Settings* out, std::wstring* err);

// A stream's identity, worked out from its name and the machine it is on
// rather than drawn at random, so that the same stream keeps the same id
// across a restart and a receiver pointed at it stays pointed at it.
uint32_t StreamIdFor(const std::wstring& host, const std::wstring& name);


}  // namespace net

// MakeNetCapture and NetSink live in audio.h, beside CaptureSource and
// Recorder: the engine holds both by value and needs their whole types, while
// nothing in the engine needs the wire formats above.
