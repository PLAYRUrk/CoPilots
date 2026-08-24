#include "ChartFoxAuth.h"
#include "ChartFoxAppConfig.h"
#include "../Log.h"

#include <algorithm>

#include <nlohmann/json.hpp>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  #define closesocket close
  typedef int SOCKET;
  #define INVALID_SOCKET (-1)
#endif

using json = nlohmann::json;

namespace cp {
namespace chart {

static const char* AUTHORIZE_URL = "https://api.chartfox.org/oauth/authorize";
static const char* TOKEN_URL     = "https://api.chartfox.org/oauth/token";

static std::string urlEncode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += static_cast<char>(c);
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 0x0F];
        }
    }
    return out;
}

// Percent-decoding for values coming back on the OAuth redirect (error text).
static std::string urlDecode(const std::string& s)
{
    auto hexVal = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') { out += ' '; continue; }
        if (s[i] == '%' && i + 2 < s.size()) {
            int hi = hexVal(s[i + 1]), lo = hexVal(s[i + 2]);
            if (hi >= 0 && lo >= 0) {
                out += static_cast<char>((hi << 4) | lo);
                i += 2;
                continue;
            }
        }
        out += s[i];
    }
    return out;
}

// Server-supplied text goes into the browser page — keep it inert.
static std::string htmlEscape(const std::string& s)
{
    std::string out;
    for (char c : s) {
        switch (c) {
        case '&': out += "&amp;";  break;
        case '<': out += "&lt;";   break;
        case '>': out += "&gt;";   break;
        case '"': out += "&quot;"; break;
        default:  out += c;        break;
        }
    }
    return out;
}

static std::string randomToken(int len)
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    static const char* alphabet = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::uniform_int_distribution<int> dist(0, 61);
    std::string s;
    for (int i = 0; i < len; ++i) s += alphabet[dist(gen)];
    return s;
}

// ── SHA-256 (compact, FIPS 180-4) — for the PKCE S256 code challenge ────────
static void sha256(const uint8_t* data, size_t len, uint8_t out[32])
{
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,
        0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,
        0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,
        0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,
        0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,
        0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,
        0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,
        0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,
        0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};

    std::vector<uint8_t> msg(data, data + len);
    uint64_t bitLen = static_cast<uint64_t>(len) * 8;
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>(bitLen >> (i * 8)));

    auto rotr = [](uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    for (size_t chunk = 0; chunk < msg.size(); chunk += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = (uint32_t(msg[chunk + i*4]) << 24) | (uint32_t(msg[chunk + i*4+1]) << 16)
                 | (uint32_t(msg[chunk + i*4+2]) << 8) |  uint32_t(msg[chunk + i*4+3]);
        for (int i = 16; i < 64; ++i) {
            uint32_t s0 = rotr(w[i-15], 7) ^ rotr(w[i-15], 18) ^ (w[i-15] >> 3);
            uint32_t s1 = rotr(w[i-2], 17) ^ rotr(w[i-2], 19)  ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; ++i) {
            uint32_t S1 = rotr(e,6) ^ rotr(e,11) ^ rotr(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K[i] + w[i];
            uint32_t S0 = rotr(a,2) ^ rotr(a,13) ^ rotr(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; ++i) {
        out[i*4+0] = static_cast<uint8_t>(h[i] >> 24);
        out[i*4+1] = static_cast<uint8_t>(h[i] >> 16);
        out[i*4+2] = static_cast<uint8_t>(h[i] >> 8);
        out[i*4+3] = static_cast<uint8_t>(h[i]);
    }
}

static std::string base64Url(const uint8_t* data, size_t len)
{
    static const char* tbl =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < len; i += 3) {
        uint32_t v = uint32_t(data[i]) << 16;
        if (i + 1 < len) v |= uint32_t(data[i+1]) << 8;
        if (i + 2 < len) v |= uint32_t(data[i+2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        if (i + 1 < len) out += tbl[(v >> 6) & 63];
        if (i + 2 < len) out += tbl[v & 63];
    }
    return out;   // no padding (RFC 7636)
}

static int64_t nowUnix()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
}

int ChartFoxAuth::redirectPort() const
{
    // "http://localhost:58525/callback" → 58525 (default 80 when absent).
    size_t scheme = redirectUri_.find("://");
    if (scheme == std::string::npos) return 0;
    size_t colon = redirectUri_.find(':', scheme + 3);
    if (colon == std::string::npos) return 80;
    return std::atoi(redirectUri_.c_str() + colon + 1);
}

std::string ChartFoxAuth::beginLogin()
{
    lastError_.clear();
    if (!hasClient()) {
        lastError_ = "Enter the OAuth Client ID first (chartfox.org/developers).";
        return "";
    }
    if (state_ != State::IDLE) cancelLogin();

    int port = redirectPort();
    if (port <= 0 || port > 65535) {
        lastError_ = "Bad redirect URI (expected e.g. http://localhost:58525/callback).";
        return "";
    }

    std::string st = randomToken(32);

    // PKCE (RFC 7636): required for a Public client, harmless for a
    // confidential one.
    codeVerifier_ = randomToken(64);
    uint8_t digest[32];
    sha256(reinterpret_cast<const uint8_t*>(codeVerifier_.data()),
           codeVerifier_.size(), digest);
    std::string challenge = base64Url(digest, sizeof(digest));

    {
        std::lock_guard<std::mutex> lk(mu_);
        haveCode_ = false;
        code_.clear();
        listenerError_.clear();
    }
    listenerStop_ = false;
    listener_ = std::thread([this, port, st]() { listenerLoop(port, st); });

    state_ = State::WAITING_CODE;

    std::string url = std::string(AUTHORIZE_URL)
        + "?client_id="     + urlEncode(clientId_)
        + "&redirect_uri="  + urlEncode(redirectUri_)
        + "&response_type=code"
        + "&scope="         + urlEncode(scopes_)
        + "&state="         + urlEncode(st)
        + "&code_challenge=" + urlEncode(challenge)
        + "&code_challenge_method=S256";
    return url;
}

void ChartFoxAuth::cancelLogin()
{
    listenerStop_ = true;
    if (listener_.joinable()) listener_.join();
    if (state_ == State::WAITING_CODE) state_ = State::IDLE;
}

void ChartFoxAuth::logout()
{
    cancelLogin();
    accessToken_.clear();
    refreshToken_.clear();
    tokenScopes_.clear();
    expiryUnix_ = 0;
    state_ = State::IDLE;
    if (onTokensChanged) onTokensChanged("", "", 0, "");
}

void ChartFoxAuth::expireNow()
{
    if (refreshToken_.empty() || !loggedIn()) return;
    int64_t now = nowUnix();
    if (expiryUnix_ > now) expiryUnix_ = now;
}

// Every space-separated scope of scopes_ must appear in tokenScopes_.
bool ChartFoxAuth::scopesSatisfied() const
{
    // Builds before this one did not record the scope set, but they all asked
    // for the same one, so that is what such a token carries.
    const std::string have0 = tokenScopes_.empty() ? std::string(kChartFoxLegacyScopes)
                                                   : tokenScopes_;
    auto split = [](const std::string& in) {
        std::vector<std::string> out;
        size_t i = 0;
        while (i < in.size()) {
            while (i < in.size() && in[i] == ' ') ++i;
            size_t b = i;
            while (i < in.size() && in[i] != ' ') ++i;
            if (i > b) out.push_back(in.substr(b, i - b));
        }
        return out;
    };
    std::vector<std::string> have = split(have0);
    for (const std::string& want : split(scopes_))
        if (std::find(have.begin(), have.end(), want) == have.end()) return false;
    return true;
}

// One-shot loopback HTTP server: accepts connections until a request carrying
// ?code=...&state=<expectState> arrives, answers with a small HTML page, and
// exits.  Runs with 500 ms accept timeouts so listenerStop_ is honoured.
void ChartFoxAuth::listenerLoop(int port, std::string expectState)
{
    SOCKET srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv == INVALID_SOCKET) {
        std::lock_guard<std::mutex> lk(mu_);
        listenerError_ = "socket() failed";
        return;
    }
    int yes = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&yes), sizeof(yes));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(static_cast<uint16_t>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(srv, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0
        || listen(srv, 4) != 0) {
        closesocket(srv);
        std::lock_guard<std::mutex> lk(mu_);
        listenerError_ = "cannot listen on the redirect port "
                         + std::to_string(port) + " (already in use?)";
        return;
    }

    while (!listenerStop_) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        timeval tv{0, 500 * 1000};
        int sel = select(static_cast<int>(srv) + 1, &rfds, nullptr, nullptr, &tv);
        if (sel <= 0) continue;

        SOCKET cli = accept(srv, nullptr, nullptr);
        if (cli == INVALID_SOCKET) continue;

        char buf[4096];
        int n = recv(cli, buf, sizeof(buf) - 1, 0);
        std::string req = (n > 0) ? std::string(buf, n) : std::string();

        // First line: GET /callback?code=...&state=... HTTP/1.1
        auto param = [&](const char* name) -> std::string {
            std::string key = std::string(name) + "=";
            size_t p = req.find(key);
            if (p == std::string::npos) return {};
            p += key.size();
            size_t e = req.find_first_of("& \r\n", p);
            return req.substr(p, e == std::string::npos ? std::string::npos : e - p);
        };
        std::string code  = param("code");
        std::string state = param("state");
        std::string oerr  = param("error");

        // Consent denied, or a scope was requested that the application was
        // not granted: ChartFox redirects with ?error=... and no code.  Without
        // this branch the window waits for a code that never comes.
        std::string failMsg;
        if (!oerr.empty()) {
            std::string desc = urlDecode(param("error_description"));
            failMsg = "ChartFox login failed: " + urlDecode(oerr)
                    + (desc.empty() ? std::string() : " — " + desc);
        }

        bool good = failMsg.empty() && !code.empty() && state == expectState;
        std::string html = good
            ? "<html><body style='font-family:sans-serif'><h2>CoPilots connected to "
              "ChartFox.</h2><p>You can close this window and return to X-Plane.</p>"
              "</body></html>"
            : "<html><body style='font-family:sans-serif'><h2>Login failed.</h2><p>"
              + (failMsg.empty()
                 ? std::string("Missing or mismatched authorization code — try "
                               "again from the plugin.")
                 : htmlEscape(failMsg))
              + "</p></body></html>";
        std::string resp = "HTTP/1.1 200 OK\r\nContent-Type: text/html\r\n"
                           "Content-Length: " + std::to_string(html.size()) +
                           "\r\nConnection: close\r\n\r\n" + html;
        send(cli, resp.c_str(), static_cast<int>(resp.size()), 0);
        closesocket(cli);

        if (good) {
            std::lock_guard<std::mutex> lk(mu_);
            haveCode_ = true;
            code_     = code;
            break;
        }
        if (!failMsg.empty()) {
            std::lock_guard<std::mutex> lk(mu_);
            listenerError_ = failMsg;
            break;
        }
        // Ignore favicon requests / bad hits and keep waiting.
    }
    closesocket(srv);
}

void ChartFoxAuth::beginTokenPost(const std::string& formBody)
{
    tokenHandle_ = http_->postForm(TOKEN_URL, {"Accept: application/json"}, formBody);
}

void ChartFoxAuth::handleTokenResponse(const net::HttpResult& res, bool wasRefresh)
{
    if (!res.ok) {
        std::string detail = res.error;
        try {
            json j = json::parse(res.body);
            if (j.contains("message")) detail = j["message"].get<std::string>();
            else if (j.contains("error_description"))
                detail = j["error_description"].get<std::string>();
            else if (j.contains("error")) detail = j["error"].get<std::string>();
        } catch (...) {}
        state_ = State::IDLE;
        if (wasRefresh) {
            // The refresh token is gone (revoked, expired, or the granted scope
            // set changed).  Drop the session: leaving the stale access token in
            // place keeps loggedIn() true with an expiry in the past, and tick()
            // would fire a fresh POST every single frame.
            accessToken_.clear();
            refreshToken_.clear();
            tokenScopes_.clear();
            expiryUnix_ = 0;
            lastError_  = "ChartFox session expired — please log in again ("
                        + detail + ")";
            Log("ChartFoxAuth: token refresh failed, signed out: %s", detail.c_str());
            if (onTokensChanged) onTokensChanged(accessToken_, refreshToken_, expiryUnix_,
                                                tokenScopes_);
            return;
        }
        lastError_ = "ChartFox login failed: " + detail;
        Log("ChartFoxAuth: token request failed: %s", lastError_.c_str());
        return;
    }
    try {
        json j = json::parse(res.body);
        accessToken_  = j.value("access_token", "");
        // Refresh token is optional in refresh responses — keep the old one.
        std::string rt = j.value("refresh_token", "");
        if (!rt.empty()) refreshToken_ = rt;
        int64_t expiresIn = j.value("expires_in", (int64_t)3600);
        expiryUnix_ = nowUnix() + expiresIn;
        if (accessToken_.empty()) {
            lastError_ = "ChartFox login failed: no access_token in response";
            state_ = State::IDLE;
            return;
        }
        // ChartFox does not echo the granted scope set, so record what this
        // login asked for — that is exactly what the token carries.
        if (!wasRefresh) tokenScopes_ = scopes_;
        lastError_.clear();
        state_ = State::IDLE;
        Log("ChartFoxAuth: token obtained (expires in %lld s)",
            static_cast<long long>(expiresIn));
        if (onTokensChanged) onTokensChanged(accessToken_, refreshToken_, expiryUnix_,
                                                tokenScopes_);
    } catch (const std::exception& e) {
        lastError_ = std::string("ChartFox login failed: bad token response — ") + e.what();
        state_ = State::IDLE;
    }
}

void ChartFoxAuth::tick()
{
    if (!http_) return;

    // 1. Listener produced an authorization code → exchange it.
    if (state_ == State::WAITING_CODE) {
        std::string code, lerr;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (haveCode_) { code = code_; haveCode_ = false; }
            lerr = listenerError_;
        }
        if (!lerr.empty()) {
            lastError_ = lerr;
            cancelLogin();
            state_ = State::IDLE;
            return;
        }
        if (!code.empty()) {
            cancelLogin();  // join the (finished) listener thread
            state_ = State::EXCHANGING;
            std::string body =
                "grant_type=authorization_code"
                "&client_id="     + urlEncode(clientId_) +
                "&redirect_uri="  + urlEncode(redirectUri_) +
                "&code_verifier=" + urlEncode(codeVerifier_) +
                "&code="          + urlEncode(code);
            if (!clientSecret_.empty())          // confidential client only
                body += "&client_secret=" + urlEncode(clientSecret_);
            beginTokenPost(body);
        }
        return;
    }

    // 2. Token POST in flight (exchange or refresh).
    if (state_ == State::EXCHANGING || state_ == State::REFRESHING) {
        net::HttpResult res;
        if (http_->poll(tokenHandle_, res)) {
            tokenHandle_ = net::AsyncHttp::INVALID;
            handleTokenResponse(res, state_ == State::REFRESHING);
        }
        return;
    }

    // 3. Proactive refresh two minutes before expiry.
    if (loggedIn() && !refreshToken_.empty() && hasClient()
        && expiryUnix_ > 0 && nowUnix() > expiryUnix_ - 120) {
        state_ = State::REFRESHING;
        // No "scope" here on purpose: RFC 6749 §6 treats an omitted scope as
        // "the same set the grant already carries", while sending a set wider
        // than the original grant is rejected outright.  When the built-in
        // scope list grows, the old refresh token keeps working until the user
        // signs in again (ChartsWindow prompts them to).
        std::string body =
            "grant_type=refresh_token"
            "&client_id="     + urlEncode(clientId_) +
            "&refresh_token=" + urlEncode(refreshToken_);
        if (!clientSecret_.empty())              // confidential client only
            body += "&client_secret=" + urlEncode(clientSecret_);
        beginTokenPost(body);
    }
}

}
}
