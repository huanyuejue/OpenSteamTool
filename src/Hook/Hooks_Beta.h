#pragma once

#include "Steam/Types.h"

// ─────────────────────────────────────────────────────────────────
// Beta branch restoration.
//
// Steam only persists the user's beta choice to appmanifest's
// UserConfig/BetaKey when the user clicks the branch in the UI. While
// saving app state it drops UserConfig/BetaKey again, keeping only
// MountedConfig/BetaKey. Since lua-unlocked apps get their licenses
// re-injected on every start, Steam's branch validation then falls
// back to "public" (and queues a downgrade) on the next launch.
//
// This module detects that state from the on-disk manifest and calls
// CUserAppManager::SetActiveBeta (the exact function Steam's own branch
// dropdown calls) once the UI has loaded, restoring the branch for the
// session and letting Steam update along the beta branch as usual.
//
// The function is resolved through PatternLoader ("SetActiveBeta"); if
// the pattern spec has no entry for it, the feature stays disabled and
// the usual missing-signature report surfaces it.
// ─────────────────────────────────────────────────────────────────
namespace Hooks_Beta {
    // Resolve CUserAppManager::SetActiveBeta via PatternLoader. Safe to
    // call when the entry is missing (the feature just stays disabled).
    // May be called again after a steamclient fallback: all runtime
    // state is reset by Uninstall(), so the module re-arms cleanly.
    void Install();
    void Uninstall();

    // Capture the CUserAppManager instance while BuildDepotDependency
    // is running (its first argument is that instance).
    void NotifyUserAppMgr(void* userAppMgr);

    // Called from FillInAppOverview (UI thread) once app information is
    // available; arms the delayed restore.
    void NotifyOverview();

    // Called from CSteamUIAppControllerRunFrame (UI thread). Throttled;
    // restores pending beta branches and then marks itself finished.
    // If NotifyOverview never arrives, it arms itself from the frame
    // callback after a short grace period.
    void OnUIThread();
}
