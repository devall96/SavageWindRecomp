// Updates of Savage Wind itself (Windows release folders, Linux release AppImages; runtime/updater.c), from the
// configuration screen's Settings > Prelaunch > Updates. Only when the player asks (or turned on Check for updates when
// the configuration opens): the only server it talks to is GitHub's, for the project's release list and the release's
// file for this system.
//
// Windows:
//   check     the newest release with a Windows zip, newer than this version (GitHub's release list, prereleases too:
//             every Savage Wind release so far is one)
//   download  the zip into <exe folder>/update/, its SHA-256 checked against the one GitHub publishes for the asset,
//             unpacked into update/new/ (paths checked: nothing outside it). Its build-ids.txt (make dist) says whether
//             the game code and the shaders will have to be made again.
//   apply     each shipped file moved into place, the one it replaces moved to update/old/ first (Windows lets a running
//             program and its DLLs be renamed); files the previous release's files.txt lists and the new one doesn't
//             are moved there too. Anything failing moves everything back. Then Savage Wind starts again (--config)
//             and the next start removes update/.
// Linux: the same check, for Savage_Wind-<version>-<arch>.AppImage; the download goes next to the AppImage (one
// filesystem), its SHA-256 checked, and is renamed over it - atomic, and the running copy keeps its file - after a hard
// link keeps the previous one until the new version has started. The new version is started from the AppImage's path
// (--config --updated); the system's libcurl, opened at run time, does the HTTPS.
// Game files, saves, settings and caches are never touched: only files a release ships (files.txt) are moved.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    UPD_IDLE,         // not checked yet
    UPD_CHECKING,
    UPD_UP_TO_DATE,
    UPD_AVAILABLE,    // latest / notes_url / size
    UPD_DOWNLOADING,  // done / total
    UPD_READY,        // unpacked in update/new/: apply
    UPD_FAILED,       // error
} UpdState;

enum { UPD_UNKNOWN = -1, UPD_KEPT = 0, UPD_REDONE = 1 };  // what the update means for the game code, the shaders

typedef struct {
    UpdState state;
    char current[64];     // this version
    char latest[64];      // the release's version (its tag without the v)
    char notes_url[512];  // the release's page
    uint64_t done, total; // download bytes
    int gamecode, shaders;  // UPD_*: from the new release's build-ids.txt (UPD_UNKNOWN without one)
    char error[300];      // UPD_FAILED: what went wrong (English, for the log too)
} UpdStatus;

// A Windows release folder or a Linux release AppImage Savage Wind can write to (not a source build), its own
bool updater_available(void);
// The launcher that installed this folder and updates it (Quiver Launcher, GitHub Launcher, RecompLauncher), NULL: none.
// Then updater_available() is false: no checks, offers or installs of Savage Wind's own.
const char *updater_managed_by(void);
void updater_status(UpdStatus *out);
void updater_check(void);      // on a thread of its own; nothing while one is running
void updater_download(void);   // UPD_AVAILABLE: on a thread of its own
void updater_cancel(void);     // a download running: stopped, back to UPD_AVAILABLE
// UPD_READY: puts the new files in place and starts Savage Wind again (--config --updated: the configuration screen
// builds the game code again from the game files set up before, then_play: and starts the game). Returns only when
// that didn't happen: false, nothing changed (why: what went wrong; every file moved back); true, installed but the
// new version didn't start (why says to start it again).
bool updater_apply(bool then_play, char *why, size_t why_size);
// The game (title menu, Update now): starts the configuration screen on its update (--config --update --then-play,
// after this process has ended: --wait-pid), for the caller to quit. false: it didn't start.
bool updater_start_update(void);
bool updater_wait_pid(unsigned long pid, int timeout_ms);  // true: that process has ended (or never was)
// at start: what an applied update left in update/, and the previous version put back after an install cut short
void updater_cleanup(void);
// (test) SAVAGEWIND_UPDATE_TEST=check | download | apply: those steps one after another, logged, then the exit status
int updater_test(const char *steps);

// version compare: <0 a older than b, 0 the same, >0 newer (a leading "v" ignored)
int updater_version_cmp(const char *a, const char *b);

#ifdef __cplusplus
}
#endif
