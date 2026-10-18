#pragma once
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// AAA Streamer sign-in
//
// AAA Streamer signs a desktop program in the way a television signs in to a
// streaming service: the program asks for a code, the person approves it in a
// browser where they are already signed in, and the program, which has been
// asking every few seconds, is handed a token. No password ever passes through
// FastRoute.
//
// The token is kept per Windows user, encrypted with the data protection API,
// in %APPDATA%\FastRoute - not beside the executable with the sessions, since
// a portable folder copied to someone else must not take the sign-in with it.
//
// Every call blocks on the network and must not be made on the UI thread.
// ---------------------------------------------------------------------------
namespace aaa {

// https://aaastreamer.devinecreations.net
std::wstring BaseUrl();

struct DeviceStart
{
    std::wstring userCode;      // what the person may be asked to type
    std::wstring pollToken;
    std::wstring authorizeUrl;  // the page to open
};

bool StartSignIn(DeviceStart* out, std::wstring* err);

enum PollResult { PR_PENDING, PR_APPROVED, PR_DENIED, PR_EXPIRED, PR_ERROR };
// On PR_APPROVED the token has already been saved.
PollResult PollSignIn(const std::wstring& pollToken, std::wstring* err);

bool SignedIn();
void SignOut();
// The account name, from the server; empty if it would not say.
std::wstring WhoAmI(std::wstring* err);

struct Stream
{
    std::wstring id, title, status;
    std::wstring key;        // only for streams the account owns
    std::wstring rtmpUrl;    // where to publish, without the key
    std::wstring watchUrl;
};
// The account's streams that it can publish to: the ones that come with a key.
bool ListStreams(std::vector<Stream>* out, std::wstring* err);

}   // namespace aaa
