#pragma once

// ---------------------------------------------------------------------------
// Dialling out: what RTMP and Icecast both have to get right
//
// Include after <winsock2.h> and <ws2tcpip.h>.
//
// Two rules, shared because both protocols had the same bug.
//
// A name can resolve to several addresses and answer on only some of them: a
// host reached through a userspace proxy such as Tailscale commonly resolves
// to an IPv6 address that swallows the connection and an IPv4 address that
// works, in that order. Handing the whole connect budget to the first address
// means the rest get no time at all, so the address that works is never really
// tried and the user is told the server cannot be reached when it can.
//
// And an attempt given up on must be reset, not closed politely. A connect
// abandoned half way can still complete underneath - the far side then holds
// an open connection that never says anything, and waits out its own
// handshake timeout before giving up on us. A server log showing a socket
// opened with no handshake on it is this, seen from the other end.
// ---------------------------------------------------------------------------

// The most any one address gets while there are others still to try. The last
// address left keeps whatever is left of the overall deadline, so a single
// slow server is unaffected.
const unsigned long long kAddressTryMs = 3000;

// Closes a connect that was given up on, abortively: SO_LINGER with a zero
// timeout makes the close a reset rather than a graceful shutdown, so a far
// side that did complete the handshake hears about it at once instead of
// sitting on a silent connection.
inline void AbandonConnect(SOCKET s)
{
    if (s == INVALID_SOCKET) return;
    linger lg;
    lg.l_onoff = 1;
    lg.l_linger = 0;
    setsockopt(s, SOL_SOCKET, SO_LINGER, (const char*)&lg, sizeof(lg));
    closesocket(s);
}
