#pragma once
#include "../net/HttpFetch.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace cp {
namespace chart {

// ChartFox OAuth 2.0 — authorization-code flow with PKCE (S256).
// Works with a Public client (no secret; ChartFox's recommended setup for
// client-side apps) and with a confidential client (secret included when set).
//
//   authorize:  https://api.chartfox.org/oauth/authorize
//   token:      https://api.chartfox.org/oauth/token
//
// beginLogin() starts a one-shot loopback HTTP listener on the redirect-URI
// port and returns the browser URL; the user approves on chartfox.org, the
// browser redirects to http://localhost:<port>/callback?code=...&state=...,
// the listener catches it, and tick() exchanges the code for access+refresh
// tokens (POST via AsyncHttp — nothing blocks the sim).  tick() also
// refreshes the access token automatically before it expires.
//
// client_id / client_secret come from the ChartFox developer portal
// (chartfox.org/developers, organisation OAuth client).  The redirect URI
// must be registered there and match `redirectUri` exactly.
class ChartFoxAuth {
public:
    enum class State { IDLE, WAITING_CODE, EXCHANGING, REFRESHING };

    static constexpr const char* kDefaultRedirectUri = "http://localhost:58525/callback";

    ~ChartFoxAuth() { cancelLogin(); }

    void init(net::AsyncHttp* http) { http_ = http; }

    void setClient(const std::string& clientId, const std::string& clientSecret,
                   const std::string& redirectUri)
    {
        clientId_     = clientId;
        clientSecret_ = clientSecret;
        redirectUri_  = redirectUri.empty() ? kDefaultRedirectUri : redirectUri;
    }
    const std::string& clientId()     const { return clientId_; }
    const std::string& clientSecret() const { return clientSecret_; }
    const std::string& redirectUri()  const { return redirectUri_; }
    bool hasClient() const { return !clientId_.empty(); }  // secret optional (PKCE)

    // Space-delimited scopes requested at login (must all be granted to the
    // application, or the authorize endpoint rejects the login with 403).
    void setScopes(const std::string& s) { scopes_ = s; }
    const std::string& scopes() const { return scopes_; }

    // Restore persisted tokens (Prefs) at startup.  tokenScopes is the scope
    // set the stored token was actually granted; it can be narrower than
    // scopes() after the app gains new scopes (see scopesSatisfied()).
    void setTokens(const std::string& access, const std::string& refresh,
                   int64_t expiryUnix, const std::string& tokenScopes = std::string())
    { accessToken_ = access; refreshToken_ = refresh; expiryUnix_ = expiryUnix;
      tokenScopes_ = tokenScopes; }

    bool loggedIn() const { return !accessToken_.empty(); }
    const std::string& accessToken() const { return accessToken_; }
    const std::string& tokenScopes() const { return tokenScopes_; }

    // False when the current token predates a scope the plugin now needs, i.e.
    // the user has to sign in again for the new capability to work.  Unknown
    // (empty) token scopes are treated as satisfied — tokens stored by builds
    // that did not record them are not worth nagging about.
    bool scopesSatisfied() const;

    // Marks the access token as due for renewal so the next tick() refreshes
    // it.  Used when the API answers 401 before the token's stated expiry
    // (revoked server-side, or a clock skew).  No-op without a refresh token.
    void expireNow();

    // Starts the loopback listener and returns the authorize URL to open in
    // the browser ("" on error, see lastError()).
    std::string beginLogin();
    void cancelLogin();
    void logout();

    // Main thread, once per frame: drives code exchange and token refresh.
    // Fires onTokensChanged whenever access/refresh tokens change.
    void tick();

    State state() const { return state_; }
    const std::string& lastError() const { return lastError_; }

    // access, refresh, expiryUnix, granted scopes — for persistence.
    std::function<void(const std::string&, const std::string&, int64_t,
                       const std::string&)> onTokensChanged;

private:
    void listenerLoop(int port, std::string expectState);
    void beginTokenPost(const std::string& formBody);
    void handleTokenResponse(const net::HttpResult& res, bool wasRefresh);
    int  redirectPort() const;

    net::AsyncHttp* http_ = nullptr;

    std::string clientId_, clientSecret_;
    std::string redirectUri_ = kDefaultRedirectUri;
    std::string scopes_;

    std::string accessToken_, refreshToken_;
    std::string tokenScopes_;   // scope set the stored token was granted
    int64_t     expiryUnix_ = 0;

    State       state_ = State::IDLE;
    std::string lastError_;
    std::string codeVerifier_;   // PKCE verifier of the login in progress

    // Loopback listener (one-shot).
    std::thread        listener_;
    std::atomic<bool>  listenerStop_{false};
    std::mutex         mu_;
    bool               haveCode_ = false;   // guarded by mu_
    std::string        code_;               // guarded by mu_
    std::string        listenerError_;      // guarded by mu_

    net::AsyncHttp::Handle tokenHandle_ = net::AsyncHttp::INVALID;
};

}
}
