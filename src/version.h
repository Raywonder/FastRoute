#pragma once

// The one place the version is written down.
//
// Everything else reads it from here: the VERSIONINFO resource so the file
// properties say something, dist.ps1 so a zip is named for it without being
// told, the About dialog, the continuous build so a release can be labelled,
// and the updater so it can tell whether what is on the server is newer than
// what is running.
//
// Nothing here may be anything but a preprocessor definition: this file is
// included by the resource compiler as well as by C++.

#define FASTROUTE_VERSION       "0.6"
// The same string for C++, which wants it wide. Two steps because the
// preprocessor will not paste L onto a macro in one.
#define FASTROUTE_WIDEN2(x) L##x
#define FASTROUTE_WIDEN(x)  FASTROUTE_WIDEN2(x)
#define FASTROUTE_VERSION_W FASTROUTE_WIDEN(FASTROUTE_VERSION)
// The same again for VERSIONINFO, which wants four numbers rather than a
// string. Keep it in step by hand; there is no arithmetic available here.
#define FASTROUTE_VERSION_COMMA 0, 6, 0, 0

// Which commit this was built from, when the build knew. The continuous build
// passes it in, so a build from the server can be compared exactly rather than
// by version string - two builds of 0.6 are otherwise indistinguishable, and
// between releases that is every build.
#ifndef FASTROUTE_BUILD_COMMIT
#define FASTROUTE_BUILD_COMMIT ""
#endif
