#pragma once

#include <cstdint>
#include <string>

namespace cp {

// Connection-form persistence: saved on every Host/Join click, loaded at plugin
// start, so a crew member whose sim crashed can rejoin with a prefilled form.
// Stored in <X-Plane>/Output/preferences/copilots_prefs.json.
struct Prefs {
    std::string nick     = "Pilot";
    std::string address  = "127.0.0.1:56900";  // join field "ip:port"
    std::string password;                       // lobby password, stored in plain text
    std::string bindIp;                         // host bind interface ("" = all)
    uint16_t    hostPort = 56900;
    bool        requireJoinApproval    = true;
    bool        requireControlApproval = true;
    bool        lastWasHost = true;
    bool        pointerEnabled = false;         // laser pointer opt-in, off by default
    std::string chartfoxToken;                  // ChartFox PAT (fallback), plain text
    // ChartFox OAuth (developer portal client + per-user tokens, plain text).
    std::string chartfoxClientId;
    std::string chartfoxClientSecret;
    std::string chartfoxRedirectUri;            // "" = default localhost callback
    std::string chartfoxScopes;                 // "" = built-in default scope set
    std::string chartfoxAccessToken;
    std::string chartfoxRefreshToken;
    int64_t     chartfoxTokenExpiry = 0;        // unix seconds
    std::string chartfoxTokenScopes;            // scopes the stored token was granted
    // Where "Share to community" uploads chart calibrations.  Empty = use the
    // endpoint published in the project repository (charts/cal/config.json).
    std::string calSubmitUrl;
};

// xpSystemPath: X-Plane root with trailing separator (from XPLMGetSystemPath).
bool LoadPrefs(const std::string& xpSystemPath, Prefs& out);
bool SavePrefs(const std::string& xpSystemPath, const Prefs& p);

}
