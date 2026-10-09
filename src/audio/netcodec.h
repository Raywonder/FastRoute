#pragma once
#include "net.h"

// ---------------------------------------------------------------------------
// Turning the bus into packets, and back
//
// Both sides are push and pull rather than one call per packet, because
// neither end gets to choose how much audio it is handed. The render thread
// gives a sender whatever the device period happened to be - 441 frames,
// then 441, then 442 - while Opus insists on exactly 960 at a time. So audio
// is pushed in whatever size it arrives and packets are pulled out when there
// are enough frames to make one.
//
// Opus only works at five sample rates, and a session runs at whatever its
// master endpoint runs at, which is often 44100. The Opus encoder therefore
// converts to 48 kHz inside itself and says 48000 on the wire; the receiver
// converts whatever the wire says back to the session rate, which it has to do
// anyway, since a 44.1 kHz sender playing to a 48 kHz receiver is an ordinary
// thing rather than a strange one.
// ---------------------------------------------------------------------------

namespace net {

class Encoder
{
public:
    virtual ~Encoder() {}

    // Interleaved stereo at the session rate. Never blocks and never
    // allocates: this is called from the render thread.
    virtual void Push(const float* in, int frames) = 0;

    // One packet, if there is one yet. Returns the payload size in bytes, 0
    // when there is not enough audio for a packet, and negative on failure.
    // `frames` is how many frames of audio the packet carries, counted at
    // WireRate() rather than at the session rate.
    virtual int Pull(unsigned char* out, int maxBytes, int* frames) = 0;

    // The rate to declare in the packet header. The session rate for PCM and
    // FLAC; 48000 for Opus, whatever the session runs at.
    virtual int WireRate() const = 0;

    // Frames per packet at WireRate, for the beacon to advertise and for the
    // dialog to turn back into milliseconds.
    virtual int WireFramesPerPacket() const = 0;

    // Anything the codec delays by on the way in, so the sender can be honest
    // about the latency it is adding before the network even sees it.
    virtual int LatencyFrames() const { return 0; }

    // Everything still held, flushed into packets. Called once at the end.
    virtual void Finish() {}
};

// `packetMs` is what was asked for; the encoder may use less to stay inside
// one datagram, and WireFramesPerPacket says what it settled on. The Opus
// options are ignored by every other codec.
std::unique_ptr<Encoder> MakeEncoder(int codec, int rate, int packetMs,
                                     const OpusOptions& opus, std::wstring* err);

class Decoder
{
public:
    virtual ~Decoder() {}

    // One packet in; interleaved stereo at WireRate appended to `out`.
    // `frames` is what the header claimed, which a codec may use to size its
    // output and may equally ignore.
    virtual bool Decode(const unsigned char* in, int bytes, int frames,
                        std::vector<float>& out) = 0;

    // A packet that never arrived. Opus can guess at what was in it from what
    // came before; everything else can only put silence there, which is a
    // click, which is the honest sound of a lost packet.
    virtual bool Conceal(int frames, std::vector<float>& out) = 0;

    // Filling a gap using the packet that came after it.
    //
    // Opus can be told to put a low rate copy of each frame inside the next
    // packet, so a single lost packet can be rebuilt from its successor rather
    // than merely guessed at - but only the receiver can ask for that, and
    // asking is what this is. `in` is the packet that arrived; what comes out
    // stands in for the one before it that did not.
    //
    // It cannot be known from here whether the redundancy was actually there.
    // When it is absent Opus quietly conceals instead and reports success all
    // the same, and the libopus vendored here has no way to ask (the call that
    // would answer, opus_packet_has_lbrr, arrived after this version). So this
    // returning true means "the gap is filled, as well as this codec can",
    // which is worth asking for either way, and not "the audio was recovered".
    // Nothing counts recoveries for that reason: a figure that could not be
    // told from concealment would be a figure that flattered itself.
    //
    // False means the codec has no such mechanism at all, which is every codec
    // here but Opus.
    virtual bool DecodeFec(const unsigned char* in, int bytes, int frames,
                           std::vector<float>& out)
    {
        (void)in; (void)bytes; (void)frames; (void)out;
        return false;
    }

    virtual int WireRate() const = 0;
};

std::unique_ptr<Decoder> MakeDecoder(int codec, int rate, std::wstring* err);

// What one packet of this codec holds, in frames, for a wanted packet length
// in milliseconds - clamped so that the payload fits a single datagram. Used
// by the dialog to say what it is really going to send.
int FramesPerPacketFor(int codec, int rate, int packetMs);

// --- PCM, which the VBAN path needs on its own -----------------------------
//
// VBAN carries PCM and nothing else here, so these are exposed rather than
// hidden inside an encoder: a VBAN sender writes its own header and then needs
// exactly this.
int PcmBytesPerFrame(int codec);                       // 0 if not a PCM codec
void PcmFromFloat(int codec, const float* in, int frames, unsigned char* out);
void PcmToFloat(int codec, const unsigned char* in, int frames, float* out);

// The same for VBAN's own data type codes, which do not match ours.
int VbanBytesPerSample(int datatype);                  // 0 if unsupported
bool VbanToFloat(int datatype, const unsigned char* in, int frames, int channels,
                 std::vector<float>& out);

}  // namespace net
