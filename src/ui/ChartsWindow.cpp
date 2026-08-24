#include "ChartsWindow.h"
#include "Theme.h"
#include "../chart/ChartFoxAppConfig.h"
#include "../chart/Georef.h"
#include "../net/Protocol.h"
#include "../Log.h"

#include <imgui.h>
#include <XPLM/XPLMDisplay.h>
#include <XPLM/XPLMGraphics.h>
#include <XPLM/XPLMNavigation.h>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <shellapi.h>
  #include <GL/gl.h>
#elif defined(__APPLE__)
  #include <OpenGL/gl.h>
#else
  #include <GL/gl.h>
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#ifndef M_PI
#  define M_PI 3.14159265358979323846
#endif

namespace cp {
namespace ui {

using namespace cp::chart;
using cp::notepad::Tool;
using cp::notepad::npOwner;
using cp::notepad::colorForParticipant;

// ── Init / shutdown ──────────────────────────────────────────────────────────

static void openInBrowser(const std::string& url)
{
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    (void)url;
#endif
}

bool ChartsWindow::init(const std::string& pluginDir, const std::string& xpSystemPath)
{
    cfx_.init(&http_, xpSystemPath + "Output/CoPilots/chartfox_cache");
    cals_.init(xpSystemPath + "Output/CoPilots/chart_calibrations.json",
               xpSystemPath + "Output/CoPilots/chart_cal_cache");
    calCloud_.init(&http_, &cals_);
    calCloud_.openBrowser = [](const std::string& url) { openInBrowser(url); };
    renderer_.init(pluginDir);

    auth_.init(&http_);
    auth_.onTokensChanged = [this](const std::string& access, const std::string& refresh,
                                   int64_t expiry, const std::string& tokenScopes) {
        applyEffectiveToken();
        if (onOauthTokensChanged) onOauthTokensChanged(access, refresh, expiry, tokenScopes);
    };

    drLat_ = XPLMFindDataRef("sim/flightmodel/position/latitude");
    drLon_ = XPLMFindDataRef("sim/flightmodel/position/longitude");
    drPsi_ = XPLMFindDataRef("sim/flightmodel/position/psi");

    int scrL, scrT, scrR, scrB;
    XPLMGetScreenBoundsGlobal(&scrL, &scrT, &scrR, &scrB);
    int cx = (scrL + scrR) / 2;
    int cy = (scrB + scrT) / 2;
    int l = cx - 500, r = cx + 500;
    int t = cy + 350, b = cy - 350;
    return xpwInit(l, t, r, b);
}

void ChartsWindow::shutdown()
{
    dropAllTextures();
    renderer_.shutdown();
    xpwShutdown();
}

void ChartsWindow::setToken(const std::string& t)
{
    patToken_ = t;
    snprintf(tokenBuf_, sizeof(tokenBuf_), "%s", t.c_str());
    applyEffectiveToken();
}

void ChartsWindow::setOauth(const std::string& clientId, const std::string& clientSecret,
                            const std::string& redirectUri, const std::string& scopes,
                            const std::string& accessToken, const std::string& refreshToken,
                            int64_t expiryUnix, const std::string& tokenScopes)
{
    // Prefs act as overrides on top of the built-in app client (empty prefs =
    // use the baked-in Client ID / redirect / scopes — the normal crew setup).
    snprintf(clientIdBuf_,     sizeof(clientIdBuf_),     "%s", clientId.c_str());
    snprintf(clientSecretBuf_, sizeof(clientSecretBuf_), "%s", clientSecret.c_str());
    snprintf(redirectBuf_,     sizeof(redirectBuf_),     "%s", redirectUri.c_str());
    snprintf(scopesBuf_,       sizeof(scopesBuf_),       "%s", scopes.c_str());
    applyOauthClientFromBuffers();
    auth_.setTokens(accessToken, refreshToken, expiryUnix, tokenScopes);
    applyEffectiveToken();
}

void ChartsWindow::applyOauthClientFromBuffers()
{
    const char* id  = clientIdBuf_[0] ? clientIdBuf_ : chart::kChartFoxClientId;
    const char* red = redirectBuf_[0] ? redirectBuf_ : chart::kChartFoxRedirectUri;
    const char* sc  = scopesBuf_[0]   ? scopesBuf_   : chart::kChartFoxScopes;
    auth_.setClient(id, clientSecretBuf_, red);
    auth_.setScopes(sc);
}

void ChartsWindow::applyEffectiveToken()
{
    // A user-bound OAuth access token wins; the PAT is the fallback.
    const std::string& tok = auth_.loggedIn() ? auth_.accessToken() : patToken_;
    cfx_.setToken(tok);
    hasToken_ = !tok.empty();
    // Retry everything that failed with the previous credentials.
    detailCache_.clear();
    for (auto& [tid, v] : views_) {
        v.pipelineError.clear();
        v.pipelineChartId.clear();
        v.authRetried = false;
        v.list = {};
        v.listIcao.clear();
    }
}

// ── Helpers ──────────────────────────────────────────────────────────────────

chart::NpId ChartsWindow::mintId()
{
    uint8_t myId = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
    return cp::notepad::makeNpId(myId, cfCounter_++);
}

bool ChartsWindow::isTabOwner(const chart::ChartTab& tab) const
{
    // Same rule as the notepad: private tabs are always ours; shared tabs are
    // ours when we authored them or when they were minted offline (owner 0xFF).
    uint8_t myPid = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0xFF;
    return !tab.shared
        || npOwner(tab.id) == myPid
        || npOwner(tab.id) == static_cast<uint8_t>(cp::INVALID_PARTICIPANT_ID);
}

static std::string toUpperIcao(const char* buf)
{
    std::string s;
    for (const char* p = buf; *p; ++p)
        if (std::isalnum(static_cast<unsigned char>(*p)))
            s += static_cast<char>(std::toupper(static_cast<unsigned char>(*p)));
    if (s.size() > 4) s.resize(4);
    return s;
}

// ── Network send helpers ─────────────────────────────────────────────────────

void ChartsWindow::netTabShare(const chart::ChartTab& tab)
{
    if (!sendFn) return;
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_TAB_SHARE)
           .u32(tab.id).str(tab.icao).str(tab.name).build());
}

void ChartsWindow::netTabSetAirport(chart::NpId tabId, const std::string& icao)
{
    if (!sendFn) return;
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_TAB_SET_AIRPORT)
           .u32(tabId).str(icao).build());
}

void ChartsWindow::netTabSetChart(chart::NpId tabId, const std::string& chartId)
{
    if (!sendFn) return;
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_TAB_SET_CHART)
           .u32(tabId).str(chartId).build());
}

void ChartsWindow::netStrokeAdd(chart::NpId tabId, const std::string& chartId,
                                uint8_t page, const chart::Stroke& stroke)
{
    if (!sendFn || stroke.pts.empty()) return;
    auto b = cp::proto::MsgBuilder(cp::proto::MsgType::CF_STROKE_ADD)
             .u32(tabId).str(chartId).u8(page)
             .u32(stroke.id)
             .u8(static_cast<uint8_t>(stroke.tool))
             .u32(stroke.colorRGBA)
             .f32(stroke.thickness)
             .u16(static_cast<uint16_t>(stroke.pts.size()));
    for (const auto& p : stroke.pts) b.f32(p.x).f32(p.y);
    sendFn(b.build());
}

void ChartsWindow::netStrokeDel(chart::NpId tabId, const std::string& chartId,
                                uint8_t page, chart::NpId strokeId)
{
    if (!sendFn) return;
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_STROKE_DEL)
           .u32(tabId).str(chartId).u8(page).u32(strokeId).build());
}

void ChartsWindow::netCalSet(const std::string& chartId, uint8_t page1, bool clear,
                             const std::string& icao, bool isOverride,
                             const chart::ChartGeoref& g)
{
    // Calibrating alone is perfectly normal, so only speak up in a session.
    if (!sendFn || !sess_ || sess_->myId() == cp::INVALID_PARTICIPANT_ID) return;
    // Not tied to a tab: a calibration belongs to the chart itself, so every
    // client benefits from it whichever tab it opens the chart in.  The airport
    // travels with it so the receiver can share it on, and the override flag so
    // it outranks a ChartFox georeference on their side too.
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_CAL_SET)
           .str(chartId).u8(page1).u8(clear ? 1 : 0)
           .str(icao).u8(isOverride ? 1 : 0)
           .f64(g.k).f64(g.tx).f64(g.ty).f64(g.transformAngle)
           .build());
}

void ChartsWindow::netTabDel(chart::NpId tabId)
{
    if (!sendFn) return;
    sendFn(cp::proto::MsgBuilder(cp::proto::MsgType::CF_TAB_DEL)
           .u32(tabId).build());
}

// ── Inbound events ───────────────────────────────────────────────────────────

void ChartsWindow::onTabShare(chart::NpId tabId, const std::string& icao,
                              const std::string& name)
{
    charts_.ensureSharedTab(tabId, icao, name);
}

void ChartsWindow::onTabSetAirport(chart::NpId tabId, const std::string& icao)
{
    chart::ChartTab* tab = charts_.findTab(tabId);
    if (!tab) return;
    if (tab->icao == icao) return;
    tab->icao = icao;
    tab->name = icao.empty() ? tab->name : icao;
    tab->activeChartId.clear();
}

void ChartsWindow::onTabSetChart(chart::NpId tabId, const std::string& chartId)
{
    chart::ChartTab* tab = charts_.findTab(tabId);
    if (!tab) return;
    tab->activeChartId = chartId;
}

void ChartsWindow::onStrokeAdd(chart::NpId tabId, const std::string& chartId,
                               uint8_t page, const chart::Stroke& stroke)
{
    chart::ChartTab* tab = charts_.findTab(tabId);
    if (!tab) return;
    tab->annotsFor(chartId, page).applyStroke(stroke);  // idempotent
}

void ChartsWindow::onStrokeDel(chart::NpId tabId, const std::string& chartId,
                               uint8_t page, chart::NpId strokeId)
{
    chart::ChartTab* tab = charts_.findTab(tabId);
    if (!tab) return;
    chart::ChartAnnots* a = tab->findAnnots(chartId, page);
    if (a) a->removeStroke(strokeId);
}

void ChartsWindow::onTabDel(chart::NpId tabId)
{
    if (drawingTab_ == tabId) {
        drawing_ = false;
        scratchStroke_ = {};
        erasedThisStroke_.clear();
        drawingTab_ = chart::INVALID_NPID;
    }
    charts_.tabs.erase(
        std::remove_if(charts_.tabs.begin(), charts_.tabs.end(),
                       [tabId](const chart::ChartTab& t){ return t.id == tabId; }),
        charts_.tabs.end());
    views_.erase(tabId);
}

void ChartsWindow::onSnapTab(chart::NpId tabId, const std::string& icao,
                             const std::string& name,
                             const std::string& activeChartId,
                             const std::string& chartId, uint8_t page,
                             bool isFirstChunk,
                             const std::vector<chart::Stroke>& strokes)
{
    chart::ChartTab* tab = charts_.ensureSharedTab(tabId, icao, name);
    tab->activeChartId = activeChartId;
    if (chartId.empty()) return;   // tab-only chunk (no annotations)
    chart::ChartAnnots& a = tab->annotsFor(chartId, page);
    if (isFirstChunk) {
        a.strokes.clear();
        a.strokeIds.clear();
    }
    for (const auto& s : strokes) a.applyStroke(s);
}

void ChartsWindow::resetShared()
{
    for (auto it = charts_.tabs.begin(); it != charts_.tabs.end(); ) {
        if (it->shared) {
            views_.erase(it->id);
            it = charts_.tabs.erase(it);
        } else {
            ++it;
        }
    }
    drawing_ = false;
    scratchStroke_ = {};
    erasedThisStroke_.clear();
    drawingTab_ = chart::INVALID_NPID;
}

// ── Chart selection & pipelines ──────────────────────────────────────────────

void ChartsWindow::loadAirport(chart::ChartTab& tab, TabView& v, const std::string& icao)
{
    if (icao.empty() || icao == tab.icao) return;
    tab.icao = icao;
    tab.name = icao;
    tab.activeChartId.clear();
    v.list = {};
    v.listIcao.clear();
    if (tab.shared) netTabSetAirport(tab.id, icao);
}

void ChartsWindow::selectChart(chart::ChartTab& tab, TabView& v, const std::string& chartId)
{
    if (tab.activeChartId == chartId) {
        // Clicking the chart that failed is the obvious way to ask for another
        // go; without this the error stuck until another chart was picked.
        if (!v.pipelineError.empty()) reloadChart(tab, v, false);
        return;
    }
    tab.activeChartId = chartId;
    if (tab.shared) netTabSetChart(tab.id, chartId);
}

void ChartsWindow::reloadChart(chart::ChartTab& tab, TabView& v, bool purgeFile)
{
    const std::string id = tab.activeChartId;
    if (id.empty()) return;
    if (purgeFile) {
        cfx_.dropCachedFile(id);
        for (auto it = texCache_.begin(); it != texCache_.end(); ) {
            if (it->first.chartId != id) { ++it; continue; }
            if (it->second.glId) {
                GLuint gid = it->second.glId;
                glDeleteTextures(1, &gid);
            }
            it = texCache_.erase(it);
        }
        detailCache_.erase(id);
    }
    // A page that failed to rasterise caches its error forever otherwise.
    for (auto it = texCache_.begin(); it != texCache_.end(); ) {
        if (it->first.chartId == id && !it->second.error.empty())
            it = texCache_.erase(it);
        else
            ++it;
    }
    v.pipelineChartId.clear();   // tickPipeline restarts from scratch
    v.pipelineError.clear();
    v.authRetried = false;
    v.dlNoAuth    = false;
}

bool ChartsWindow::handleAuthFailure(TabView& v, int status)
{
    if (status != 401 || v.authRetried || !auth_.loggedIn()) return false;
    v.authRetried = true;
    auth_.expireNow();   // tick() refreshes; applyEffectiveToken() then retries
    return true;
}

void ChartsWindow::fetchMetar(TabView& v, const std::string& icao)
{
    if (icao.empty() || v.metarHandle != net::AsyncHttp::INVALID) return;
    v.metarIcao   = icao;
    v.metarHandle = http_.get(
        "https://aviationweather.gov/api/data/metar?ids=" + icao + "&format=raw", {});
}

void ChartsWindow::tickPipeline(chart::ChartTab& tab, TabView& v)
{
    // Restart the pipeline whenever the tab's active chart changes (locally or
    // via the network).
    if (v.pipelineChartId != tab.activeChartId) {
        v.pipelineChartId = tab.activeChartId;
        v.detailHandle = net::AsyncHttp::INVALID;
        v.dlHandle     = net::AsyncHttp::INVALID;
        v.fileReady    = false;
        v.authRetried  = false;
        v.dlNoAuth     = false;
        v.pipelineError.clear();
        v.zoom = 0.f;        // re-fit on next draw
        v.page = 0;
    }
    if (tab.activeChartId.empty() || !v.pipelineError.empty()) return;

    // A 401 asked for a token refresh; issuing the request again right now
    // would just reuse the token the server already rejected.  applyEffective-
    // Token() restarts this pipeline once the new token lands.
    if (v.authRetried && auth_.state() != chart::ChartFoxAuth::State::IDLE) return;

    const std::string& id = tab.activeChartId;

    // 1. Chart detail (georefs, file URL, copyright).
    auto dit = detailCache_.find(id);
    if (dit == detailCache_.end()) {
        if (!hasToken_) {
            v.pipelineError = "Not signed in to ChartFox — use the ChartFox account "
                              "section above to log in.";
            return;
        }
        if (v.detailHandle == net::AsyncHttp::INVALID)
            v.detailHandle = cfx_.beginGetChart(id);
        chart::ChartDetailResult res;
        if (cfx_.pollGetChart(v.detailHandle, res)) {
            v.detailHandle = net::AsyncHttp::INVALID;
            if (!res.ok) {
                if (handleAuthFailure(v, res.status)) return;
                v.pipelineError = res.error;
                return;
            }
            dit = detailCache_.emplace(id, std::move(res.detail)).first;
            // Without this a chart that simply has no georeference left no
            // trace anywhere, which made "no aircraft symbol" unexplainable.
            Log("ChartsWindow: chart %s (%s) georefs=%d file=%s preauth=%d",
                id.c_str(), dit->second.info.name.c_str(),
                static_cast<int>(dit->second.georefs.size()),
                dit->second.fileUrl.empty() ? "none" : "yes",
                dit->second.requiresPreauth ? 1 : 0);
        } else {
            return;   // still in flight
        }
    }
    const chart::ChartDetail& detail = dit->second;

    // 2. Chart file (disk-cached; each client downloads with its own token).
    if (!v.fileReady) {
        if (detail.requiresPreauth) {
            v.pipelineError = "This chart requires pre-authorisation on chartfox.org "
                              "and cannot be displayed in the plugin.";
            return;
        }
        if (detail.fileUrl.empty()) {
            v.pipelineError = detail.sourceUrlType == 2
                ? "This chart is published as a web page, not a file — open it "
                  "on chartfox.org."
                : "ChartFox has no downloadable file for this chart.";
            return;
        }
        if (v.dlHandle == net::AsyncHttp::INVALID) {
            bool cached = false;
            v.dlHandle = cfx_.beginDownloadFile(detail, &cached, !v.dlNoAuth);
            if (cached) { v.fileReady = true; v.dlHandle = net::AsyncHttp::INVALID; }
            if (!v.fileReady && v.dlHandle == net::AsyncHttp::INVALID) return;
        }
        if (!v.fileReady) {
            chart::DownloadResult res;
            if (!cfx_.pollDownloadFile(v.dlHandle, res)) return;
            v.dlHandle = net::AsyncHttp::INVALID;
            if (!res.ok) {
                if (handleAuthFailure(v, res.status)) return;
                if (!v.dlNoAuth && (res.status == 400 || res.status == 403)) {
                    v.dlNoAuth = true;   // pre-signed CDN URL: drop the bearer
                    return;
                }
                v.pipelineError = res.error;
                return;
            }
            v.fileReady = true;
        }
    }

    // 3. Rasterise the current page (async; texture appears via pollRenders).
    requestRenderIfNeeded(v, id, v.page);
}

void ChartsWindow::requestRenderIfNeeded(TabView& v, const std::string& chartId, int page)
{
    TexKey key{chartId, page};
    if (texCache_.count(key)) return;
    for (const auto& pr : pendingRenders_)
        if (pr.key.chartId == chartId && pr.key.page == page) return;
    chart::RenderRequest req;
    req.chartId  = chartId;
    req.filePath = cfx_.cachePathFor(chartId);
    req.page     = page;
    pendingRenders_.push_back({renderer_.request(req), key});
    (void)v;
}

void ChartsWindow::pollRenders()
{
    for (auto it = pendingRenders_.begin(); it != pendingRenders_.end(); ) {
        chart::RenderResult res;
        if (!renderer_.poll(it->handle, res)) { ++it; continue; }

        Tex tex;
        tex.pageCount      = res.pageCount;
        tex.pageWpt        = res.pageWpt;
        tex.pageHpt        = res.pageHpt;
        tex.canonicalScale = res.canonicalScale;
        tex.lastUsed       = frameCounter_;
        if (!res.error.empty()) {
            tex.error = res.error;
        } else {
            int glId = 0;
            XPLMGenerateTextureNumbers(&glId, 1);
            XPLMBindTexture2d(glId, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, res.w, res.h, 0,
                         GL_RGBA, GL_UNSIGNED_BYTE, res.rgba.data());
            tex.glId = static_cast<unsigned int>(glId);
            tex.w = res.w;
            tex.h = res.h;
        }
        texCache_[it->key] = std::move(tex);
        it = pendingRenders_.erase(it);
    }
    evictTextures();
}

const ChartsWindow::Tex* ChartsWindow::textureFor(const std::string& chartId, int page)
{
    auto it = texCache_.find({chartId, page});
    if (it == texCache_.end()) return nullptr;
    it->second.lastUsed = frameCounter_;
    return &it->second;
}

void ChartsWindow::evictTextures()
{
    while (texCache_.size() > kMaxTextures) {
        auto lru = texCache_.begin();
        for (auto it = texCache_.begin(); it != texCache_.end(); ++it)
            if (it->second.lastUsed < lru->second.lastUsed) lru = it;
        if (lru->second.lastUsed >= frameCounter_) break;  // everything in use this frame
        if (lru->second.glId) {
            GLuint id = lru->second.glId;
            glDeleteTextures(1, &id);
        }
        texCache_.erase(lru);
    }
}

void ChartsWindow::dropAllTextures()
{
    for (auto& [key, tex] : texCache_) {
        if (tex.glId) {
            GLuint id = tex.glId;
            glDeleteTextures(1, &id);
        }
    }
    texCache_.clear();
    pendingRenders_.clear();
}

// ── UI: token section ────────────────────────────────────────────────────────

void ChartsWindow::renderTokenSection()
{
    using AuthState = chart::ChartFoxAuth::State;

    if (!hasToken_) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
    if (ImGui::CollapsingHeader("ChartFox account")) {

        if (auth_.loggedIn()) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.3f, 1.f),
                               "Connected to ChartFox.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Logout")) auth_.logout();
            // A token issued before the app gained a scope keeps working
            // for everything it already covered, so nothing looks broken
            // - the aircraft position simply never appears.  Say so.
            if (!auth_.scopesSatisfied())
                ImGui::TextColored(ImVec4(0.95f, 0.8f, 0.2f, 1.f),
                                   "ChartFox granted new permissions (aircraft "
                                   "position on charts).\nLog out and sign in "
                                   "again to enable them.");
        } else {
            ImGui::TextWrapped("Sign in with your ChartFox account. Each crew "
                               "member signs in themselves; chart files are "
                               "never sent over the session.");

            // OAuth client override fields.  Normally hidden: the app's Client
            // ID ships built into the plugin (ChartFoxAppConfig.h) and crew
            // members only ever press the Login button.
            auto renderClientFields = [&]() {
                bool clientEdited = false;
                ImGui::SetNextItemWidth(-110.f);
                clientEdited |= ImGui::InputTextWithHint("##cfxcid", "OAuth Client ID",
                                                         clientIdBuf_,
                                                         sizeof(clientIdBuf_));
                ImGui::SameLine(); ImGui::TextDisabled("Client ID");
                ImGui::SetNextItemWidth(-110.f);
                clientEdited |= ImGui::InputTextWithHint("##cfxsec",
                                                         "empty for a Public client",
                                                         clientSecretBuf_,
                                                         sizeof(clientSecretBuf_),
                                                         ImGuiInputTextFlags_Password);
                ImGui::SameLine(); ImGui::TextDisabled("Secret");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Public OAuth clients (recommended) have no "
                                      "secret\n— leave this empty; the login uses "
                                      "PKCE.");
                ImGui::SetNextItemWidth(-110.f);
                clientEdited |= ImGui::InputTextWithHint(
                    "##cfxred", chart::ChartFoxAuth::kDefaultRedirectUri,
                    redirectBuf_, sizeof(redirectBuf_));
                ImGui::SameLine(); ImGui::TextDisabled("Redirect");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Must exactly match a redirect URI registered "
                                      "for\nthe OAuth client at "
                                      "chartfox.org/developers.");
                ImGui::SetNextItemWidth(-110.f);
                clientEdited |= ImGui::InputTextWithHint(
                    "##cfxscopes", chart::kChartFoxScopes,
                    scopesBuf_, sizeof(scopesBuf_));
                ImGui::SameLine(); ImGui::TextDisabled("Scopes");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Space-delimited OAuth scopes. Requesting a "
                                      "scope not granted\nto the application makes "
                                      "the login fail.\nLeave empty to use the set the "
                                      "plugin ships with.");
                if (clientEdited) {
                    applyOauthClientFromBuffers();
                    if (onOauthClientChanged)
                        onOauthClientChanged(clientIdBuf_, clientSecretBuf_,
                                             redirectBuf_, scopesBuf_);
                }
            };

            bool clientConfigured = auth_.hasClient();
            if (!clientConfigured) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f),
                                   "No OAuth client configured for this build:");
                renderClientFields();
            }

            if (auth_.state() == AuthState::WAITING_CODE) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f),
                                   "Waiting for the browser login...");
                ImGui::SameLine();
                if (ImGui::SmallButton("Cancel##oauth")) auth_.cancelLogin();
            } else if (auth_.state() == AuthState::EXCHANGING) {
                ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f),
                                   "Finishing ChartFox login...");
            } else {
                if (ImGui::Button("Login with ChartFox", ImVec2(-1.f, 24.f))) {
                    std::string url = auth_.beginLogin();
                    if (!url.empty()) openInBrowser(url);
                }
            }

            if (clientConfigured
                && ImGui::TreeNode("Advanced: OAuth client override")) {
                renderClientFields();
                ImGui::TreePop();
            }
        }
        if (!auth_.lastError().empty())
            ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.f), "%s",
                               auth_.lastError().c_str());

        // Fallback: a raw personal access token (server-style; OAuth preferred).
        if (!auth_.loggedIn() && ImGui::TreeNode("Use a token directly (fallback)")) {
            ImGui::SetNextItemWidth(-90.f);
            ImGui::InputText("##cfxtoken", tokenBuf_, sizeof(tokenBuf_),
                             ImGuiInputTextFlags_Password);
            ImGui::SameLine();
            if (ImGui::Button("Save", ImVec2(-1.f, 0.f))) {
                std::string t = tokenBuf_;
                setToken(t);
                if (onTokenChanged) onTokenChanged(t);
            }
            ImGui::TreePop();
        }

        if (!hasToken_)
            ImGui::TextColored(ImVec4(1.f, 0.8f, 0.2f, 1.f),
                               "Sign in to browse and view charts.");
        if (!renderer_.pdfAvailable())
            ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.f),
                               "pdfium.dll not found next to win.xpl — PDF charts "
                               "disabled (reinstall the plugin).");

        if (ImGui::TreeNode("Shared calibrations")) {
            ImGui::TextWrapped(
                "Charts ChartFox does not georeference can be pinned to the world"
                " by hand (Calibrate, under a chart).  Those calibrations are"
                " shared: yours can go to the project database, and other"
                " pilots' come back the next time you open an airport.");
            const size_t pending = cals_.unshared().size();
            if (pending == 0) ImGui::BeginDisabled();
            char label[64];
            snprintf(label, sizeof(label), "Share all my calibrations (%zu)", pending);
            if (ImGui::Button(label)) shareAllCalibrations();
            if (pending == 0) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Refresh from GitHub")) {
                calCloud_.refresh();
                for (const auto& t : charts_.tabs)
                    if (!t.icao.empty()) calCloud_.requestAirport(t.icao, true);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Shared calibrations are downloaded once and kept."
                                  "\nThis fetches them again for the airports you"
                                  "\nhave open.");
            if (!calCloud_.status().empty())
                ImGui::TextWrapped("%s", calCloud_.status().c_str());
            ImGui::TreePop();
        }

        if (ImGui::TreeNode("Diagnostics")) {
            ImGui::Checkbox("Show georeference numbers", &georefDebug_);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Prints the chart's georeference parameters and"
                                  "\nthe computed page position over the chart.");
            ImGui::TreePop();
        }
    }
}

// ── UI: chart list (left panel) ──────────────────────────────────────────────

void ChartsWindow::renderChartList(chart::ChartTab& tab, TabView& v)
{
    bool owner = isTabOwner(tab);

    // ICAO entry + Load (airport change is owner-only on shared tabs).
    if (!owner) ImGui::BeginDisabled();
    ImGui::SetNextItemWidth(60.f);
    bool enter = ImGui::InputText("##icao", v.icaoBuf, sizeof(v.icaoBuf),
                                  ImGuiInputTextFlags_CharsUppercase |
                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Load") || enter)) {
        std::string icao = toUpperIcao(v.icaoBuf);
        if (!icao.empty()) loadAirport(tab, v, icao);
    }
    if (!owner) ImGui::EndDisabled();
    ImGui::SameLine();
    if (tab.icao.empty()) ImGui::TextDisabled("enter ICAO");
    else                  ImGui::Text("%s", tab.icao.c_str());

    // Auto-(re)fetch the chart list when the tab's airport changed — covers
    // both the local Load click and a remote CF_TAB_SET_AIRPORT.
    if (!tab.icao.empty() && v.listIcao != tab.icao
        && v.listHandle == net::AsyncHttp::INVALID && hasToken_) {
        v.listHandle = cfx_.beginListCharts(tab.icao);
        v.listIcao   = tab.icao;
        v.list       = {};
        fetchMetar(v, tab.icao);
        calCloud_.requestAirport(tab.icao);
    }
    if (v.listHandle != net::AsyncHttp::INVALID) {
        chart::ChartListResult res;
        if (cfx_.pollListCharts(v.listHandle, res)) {
            v.listHandle = net::AsyncHttp::INVALID;
            // On 401 keep the list empty and say so: applyEffectiveToken()
            // clears listIcao when the refreshed token arrives, which is what
            // re-runs the fetch (an immediate one would reuse the dead token).
            if (!(!res.ok && handleAuthFailure(v, res.status))) {
                v.list = std::move(res);
                // Calibrations made before the airport was recorded (or sent by
                // a crewmate who had none) can be shared once we know where
                // their chart belongs.
                for (const auto& [group, chartsVec] : v.list.groups)
                    for (const auto& ci : chartsVec)
                        cals_.backfillIcao(ci.id, tab.icao);
            }
        }
    }

    if (!hasToken_) {
        ImGui::TextWrapped("Sign in to ChartFox above to load charts.");
    } else if (v.listHandle != net::AsyncHttp::INVALID) {
        ImGui::TextDisabled("Loading chart list...");
    } else if (v.authRetried && !v.list.ok && v.list.error.empty()) {
        ImGui::TextDisabled("Renewing the ChartFox session...");
    } else if (!v.list.ok && !v.list.error.empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(ImVec4(0.95f, 0.3f, 0.3f, 1.f), "%s", v.list.error.c_str());
        ImGui::PopTextWrapPos();
        if (ImGui::SmallButton("Retry##list")) v.listIcao.clear();
    }

    ImGui::SetNextItemWidth(-1.f);
    ImGui::InputTextWithHint("##filter", "filter", v.filterBuf, sizeof(v.filterBuf));

    std::string filter;
    for (const char* p = v.filterBuf; *p; ++p)
        filter += static_cast<char>(std::tolower(static_cast<unsigned char>(*p)));

    ImGui::BeginChild("##chartlist", ImVec2(0, -120.f), false);
    // A successful fetch with nothing in it left the panel blank, which reads
    // as a broken window rather than an airport without published charts.
    if (v.list.ok && v.list.groups.empty())
        ImGui::TextWrapped("No charts published for %s.", tab.icao.c_str());
    for (const auto& [group, chartsVec] : v.list.groups) {
        if (!ImGui::CollapsingHeader(group.c_str(), ImGuiTreeNodeFlags_DefaultOpen))
            continue;
        for (const auto& ci : chartsVec) {
            if (!filter.empty()) {
                std::string hay = ci.code + " " + ci.name + " " + ci.runways;
                std::transform(hay.begin(), hay.end(), hay.begin(),
                               [](unsigned char c){ return (char)std::tolower(c); });
                if (hay.find(filter) == std::string::npos) continue;
            }
            char label[192];
            snprintf(label, sizeof(label), "%s%s%s%s##%s",
                     ci.code.empty() ? "" : (ci.code + "  ").c_str(),
                     ci.name.c_str(),
                     ci.hasGeorefs ? " [G]" : "",
                     ci.runways.empty() ? "" : ("  (" + ci.runways + ")").c_str(),
                     ci.id.c_str());
            bool selected = (tab.activeChartId == ci.id);
            if (ImGui::Selectable(label, selected))
                selectChart(tab, v, ci.id);
        }
    }
    ImGui::EndChild();

    renderMetarBox(tab, v);
}

// ── UI: METAR box ────────────────────────────────────────────────────────────

void ChartsWindow::renderMetarBox(chart::ChartTab& tab, TabView& v)
{
    ImGui::Separator();
    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "METAR");
    ImGui::SameLine();
    bool busy = (v.metarHandle != net::AsyncHttp::INVALID);
    if (busy) ImGui::BeginDisabled();
    if (ImGui::SmallButton("Refresh##metar") && !tab.icao.empty()) {
        v.metarText.clear();
        fetchMetar(v, tab.icao);
    }
    if (busy) ImGui::EndDisabled();
    if (v.metarTime > 0.0) {
        ImGui::SameLine();
        int age = static_cast<int>(ImGui::GetTime() - v.metarTime);
        ImGui::TextDisabled("%dm%02ds ago", age / 60, age % 60);
    }

    if (v.metarHandle != net::AsyncHttp::INVALID) {
        net::HttpResult res;
        if (http_.poll(v.metarHandle, res)) {
            v.metarHandle = net::AsyncHttp::INVALID;
            if (res.ok) {
                // Plain-text response; trim whitespace.
                std::string t = res.body;
                while (!t.empty() && std::isspace((unsigned char)t.back()))  t.pop_back();
                size_t start = 0;
                while (start < t.size() && std::isspace((unsigned char)t[start])) ++start;
                t = t.substr(start);
                v.metarText = t.empty()
                            ? ("No METAR published for " + v.metarIcao + ".")
                            : t;
                v.metarTime = ImGui::GetTime();
            } else {
                v.metarText = "METAR fetch failed: " + res.error;
            }
        }
    }

    ImGui::BeginChild("##metar_text", ImVec2(0, 0), false);
    if (v.metarText.empty())
        ImGui::TextDisabled(tab.icao.empty() ? "Load an airport first."
                                             : "Fetching METAR...");
    else
        ImGui::TextWrapped("%s", v.metarText.c_str());
    ImGui::EndChild();
}

// ── UI: aircraft overlay ─────────────────────────────────────────────────────

// ── Georeference: what ChartFox published, or what the crew calibrated ───────

const chart::ChartGeoref* ChartsWindow::georefFor(const std::string& chartId,
                                                  int page0,
                                                  chart::CalOrigin* outOrigin) const
{
    auto out = [outOrigin](chart::CalOrigin o) { if (outOrigin) *outOrigin = o; };
    out(chart::CalOrigin::None);
    if (chartId.empty()) return nullptr;

    // Ours, or a community entry that was made to correct a georeference.
    if (const chart::CalEntry* c = cals_.findOverriding(chartId, page0 + 1)) {
        out(cals_.findOwn(chartId, page0 + 1) ? chart::CalOrigin::Own
                                              : chart::CalOrigin::Community);
        return &c->g;
    }
    auto it = detailCache_.find(chartId);
    if (it != detailCache_.end()) {
        if (const chart::ChartGeoref* g = it->second.georefForPage(page0 + 1)) {
            out(chart::CalOrigin::ChartFox);
            return g;
        }
    }
    // Last: a community calibration for a chart ChartFox does not georeference.
    if (const chart::CalEntry* c = cals_.findFallback(chartId, page0 + 1)) {
        out(chart::CalOrigin::Community);
        return &c->g;
    }
    return nullptr;
}

std::string ChartsWindow::renderAircraftOverlay(ImDrawList* dl, const chart::ChartTab& tab,
                                                const TabView& v, const Tex& tex,
                                                ImVec2 origin, ImVec2 clipMin,
                                                ImVec2 clipMax)
{
    if (!drLat_ || !drLon_ || !drPsi_) return "Aircraft datarefs unavailable";

    chart::CalOrigin geoSrc = chart::CalOrigin::None;
    const chart::ChartGeoref* g = georefFor(tab.activeChartId, v.page, &geoSrc);
    if (!g) return "No georeference for this page — press Calibrate";

    const double lat = XPLMGetDatad(drLat_);
    const double lon = XPLMGetDatad(drLon_);

    double cxD = 0, cyD = 0;
    const bool onPage = chart::latLonToPagePx(*g, lat, lon, tex.w, tex.h, cxD, cyD);

    if (georefDebug_) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "%s k=%.6g tx=%.2f ty=%.2f angle=%.6f\n"
                 "page %dx%d px  acf %.6f/%.6f -> %.1f,%.1f%s",
                 geoSrc == chart::CalOrigin::Own         ? "manual"
                     : geoSrc == chart::CalOrigin::Community ? "community"
                                                             : "chartfox",
                 g->k, g->tx, g->ty, g->transformAngle,
                 tex.w, tex.h, lat, lon, cxD, cyD, onPage ? "" : "  (off page)");
        ImVec2 sz = ImGui::CalcTextSize(buf);
        ImVec2 p0(clipMin.x + 6.f, clipMin.y + 6.f);
        dl->AddRectFilled(ImVec2(p0.x - 3.f, p0.y - 3.f),
                          ImVec2(p0.x + sz.x + 5.f, p0.y + sz.y + 3.f),
                          IM_COL32(0, 0, 0, 190));
        dl->AddText(p0, IM_COL32(240, 240, 240, 230), buf);
    }

    const char* src = geoSrc == chart::CalOrigin::Own       ? " (manual calibration)"
                    : geoSrc == chart::CalOrigin::Community ? " (community calibration)"
                                                            : "";
    if (!onPage) return std::string("Aircraft is outside this chart") + src;

    double north = 0.0;
    if (!chart::northAngleOnPage(*g, lat, lon, north)) north = 0.0;
    float hdg = static_cast<float>(north + XPLMGetDataf(drPsi_) * M_PI / 180.0);

    float sx = origin.x + (static_cast<float>(cxD) - v.panX) * v.zoom;
    float sy = origin.y + (static_cast<float>(cyD) - v.panY) * v.zoom;
    if (sx < clipMin.x || sx > clipMax.x || sy < clipMin.y || sy > clipMax.y)
        return std::string("Aircraft is outside the visible area") + src;

    // Screen-fixed size triangle pointing along the aircraft heading.
    const float len = 11.f, wid = 7.f;
    float ca = std::cos(hdg), sa = std::sin(hdg);
    auto rot = [&](float x, float y) {
        return ImVec2(sx + x * ca - y * sa, sy + x * sa + y * ca);
    };
    ImVec2 nose = rot(0.f, -len);
    ImVec2 tailL = rot(-wid, len * 0.7f);
    ImVec2 tailR = rot( wid, len * 0.7f);
    dl->AddTriangleFilled(nose, tailL, tailR, IM_COL32(255, 210, 30, 235));
    dl->AddTriangle(nose, tailL, tailR, IM_COL32(20, 20, 20, 255), 1.5f);
    dl->AddCircle(ImVec2(sx, sy), len + 5.f, IM_COL32(255, 210, 30, 120), 24, 1.5f);
    return std::string("Aircraft position shown")
         + (src[0] ? src : " (ChartFox georeference)");
}

// ── Hand calibration ─────────────────────────────────────────────────────────

// Accepts the forms charts actually print: 55.9725, -73.7789, 55d 58m 21s N,
// 037d 24m 47s E — any non-numeric separators, hemisphere letter anywhere.
bool ChartsWindow::parseLatLon(const std::string& text, bool isLat, double& out)
{
    double nums[3] = {0, 0, 0};
    int n = 0, sign = 0;
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty() && cur != "-" && n < 3) nums[n++] = std::atof(cur.c_str());
        cur.clear();
    };
    for (char ch : text) {
        if ((ch >= '0' && ch <= '9') || ch == '.') { cur += ch; continue; }
        if (ch == '-' && cur.empty()) { cur += ch; continue; }
        flush();
        char u = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (u == 'N' || u == 'E') sign = 1;
        else if (u == 'S' || u == 'W') sign = -1;
    }
    flush();
    if (n == 0) return false;

    bool neg = nums[0] < 0;
    double val = neg ? -nums[0] : nums[0];
    if (n >= 2) val += nums[1] / 60.0;
    if (n >= 3) val += nums[2] / 3600.0;
    if (sign < 0 || (sign == 0 && neg)) val = -val;

    const double lim = isLat ? 90.0 : 180.0;
    if (val < -lim || val > lim) return false;
    out = val;
    return true;
}

bool ChartsWindow::navaidLatLon(const std::string& ident, double& lat, double& lon,
                                std::string& outName)
{
    if (ident.size() < 2) return false;
    const XPLMNavType kTypes[] = { xplm_Nav_Airport, xplm_Nav_VOR,
                                   xplm_Nav_NDB, xplm_Nav_Fix };
    for (XPLMNavType t : kTypes) {
        XPLMNavRef ref = XPLMFindNavAid(nullptr, ident.c_str(), nullptr, nullptr,
                                        nullptr, t);
        if (ref == XPLM_NAV_NOT_FOUND) continue;
        float flat = 0.f, flon = 0.f;
        char id[64] = {}, nm[300] = {};
        XPLMGetNavAidInfo(ref, nullptr, &flat, &flon, nullptr, nullptr, nullptr,
                          id, nm, nullptr);
        if (ident != id) continue;      // XPLMFindNavAid matches fragments
        lat = flat;
        lon = flon;
        outName = std::string(id) + (nm[0] ? std::string("  ") + nm : std::string());
        return true;
    }
    return false;
}

void ChartsWindow::beginCalibration(const chart::ChartTab& tab, const TabView& v)
{
    // Calibrating a chart that already has a georeference is a correction of
    // it, and that outranks the georeference everywhere the calibration goes.
    chart::CalOrigin origin = chart::CalOrigin::None;
    georefFor(tab.activeChartId, v.page, &origin);
    calOverride_ = (origin == chart::CalOrigin::ChartFox);

    calMode_     = true;
    calChartId_  = tab.activeChartId;
    calPage_     = v.page;
    calPts_[0]   = CalPoint{};
    calPts_[1]   = CalPoint{};
    calStep_     = 0;
    calSource_   = CalSource::Navaid;
    calMsg_.clear();
    calNavBuf_[0] = calLatBuf_[0] = calLonBuf_[0] = 0;
}

void ChartsWindow::cancelCalibration()
{
    calMode_ = false;
    calChartId_.clear();
    calMsg_.clear();
}

void ChartsWindow::calibrationClick(double px, double py)
{
    if (calStep_ < 0 || calStep_ > 1) return;
    CalPoint& p = calPts_[calStep_];
    p.placed = true;
    p.px = px;
    p.py = py;
    calMsg_.clear();
}

void ChartsWindow::applyCalibration(const chart::ChartTab& tab, const TabView& v,
                                    const Tex& tex)
{
    (void)v;
    if (!calPts_[0].fixed || !calPts_[1].fixed) {
        calMsg_ = "Both points need a position.";
        return;
    }
    const double dpx = calPts_[1].px - calPts_[0].px;
    const double dpy = calPts_[1].py - calPts_[0].py;
    if (std::sqrt(dpx * dpx + dpy * dpy) < tex.h * 0.05) {
        calMsg_ = "The two points are too close together on the page.";
        return;
    }
    // Mercator distance is stretched by 1/cos(lat); undo it for a real check.
    double m1x, m1y, m2x, m2y;
    chart::latLonToMercator(calPts_[0].lat, calPts_[0].lon, m1x, m1y);
    chart::latLonToMercator(calPts_[1].lat, calPts_[1].lon, m2x, m2y);
    const double midLat = 0.5 * (calPts_[0].lat + calPts_[1].lat);
    const double ground = std::sqrt((m2x - m1x) * (m2x - m1x) + (m2y - m1y) * (m2y - m1y))
                        * std::cos(midLat * chart::kPi / 180.0);
    if (ground < 100.0) {
        calMsg_ = "The two positions are less than 100 m apart on the ground.";
        return;
    }

    chart::ChartGeoref g;
    if (!chart::georefFromTwoPoints(calPts_[0].px, calPts_[0].py,
                                    calPts_[0].lat, calPts_[0].lon,
                                    calPts_[1].px, calPts_[1].py,
                                    calPts_[1].lat, calPts_[1].lon,
                                    tex.h, g)) {
        calMsg_ = "Those two references resolve to the same place.";
        return;
    }
    cals_.set(calChartId_, calPage_ + 1, g, tab.icao, calOverride_);
    netCalSet(calChartId_, static_cast<uint8_t>(calPage_ + 1), false,
              tab.icao, calOverride_, g);
    Log("ChartsWindow: calibrated %s page %d (%s%s)  k=%.6g tx=%.2f ty=%.2f angle=%.6f",
        calChartId_.c_str(), calPage_ + 1,
        tab.icao.empty() ? "no airport" : tab.icao.c_str(),
        calOverride_ ? ", overrides ChartFox" : "",
        g.k, g.tx, g.ty, g.transformAngle);
    calMode_ = false;
    calChartId_.clear();
    autoShare();
}

void ChartsWindow::onCalSet(const std::string& chartId, uint8_t page1, bool clear,
                            const std::string& icao, bool isOverride,
                            const chart::ChartGeoref& g)
{
    if (chartId.empty() || page1 == 0) return;
    if (clear) cals_.erase(chartId, page1);
    else       cals_.set(chartId, page1, g, icao, isOverride);
}

void ChartsWindow::tickBackground()
{
    auth_.tick();
    calCloud_.tick();

    // A calibration is worth having in the shared database whether or not
    // anyone remembers to press a button, so it goes on its own.  Every few
    // seconds is often enough: this also picks up calibrations a crewmate sent
    // and ones whose airport was only just filled in, and it retries quietly
    // after a failed upload.
    if (++autoShareTick_ >= 600) {
        autoShareTick_ = 0;
        autoShare();
    }
}

void ChartsWindow::autoShare()
{
    if (calCloud_.uploading() || calCloud_.endpointMissing()) return;
    auto pending = cals_.unshared();
    if (pending.empty()) return;
    calCloud_.submit(std::move(pending), chart::ChartCalCloud::Mode::Auto);
}

void ChartsWindow::shareCalibration(const std::string& chartId, int page1)
{
    const chart::CalEntry* c = cals_.findOwn(chartId, page1);
    if (!c) return;
    if (c->icao.empty()) {
        // Nothing to file it under: the database is one file per airport.
        // Loading the airport's chart list fills this in by itself.
        calCloud_.setStatus("Load this chart's airport first so the calibration "
                            "can be filed under it.", true);
        return;
    }
    calCloud_.submit({ { chart::ChartCalStore::keyFor(chartId, page1), *c } },
                     chart::ChartCalCloud::Mode::Manual);
}

void ChartsWindow::shareAllCalibrations()
{
    calCloud_.submit(cals_.unshared(), chart::ChartCalCloud::Mode::Manual);
}

void ChartsWindow::renderCalibrationPanel(chart::ChartTab& tab, TabView& v, const Tex* tex)
{
    if (!calMode_) return;
    // Switching chart or page mid-calibration would mix pages up.
    if (calChartId_ != tab.activeChartId || calPage_ != v.page) {
        cancelCalibration();
        return;
    }

    ImGui::Separator();
    ImGui::TextColored(ImVec4(kAccentR, kAccentG, kAccentB, 1.f), "Calibration");
    ImGui::SameLine();
    ImGui::Text("point %d of 2:", calStep_ + 1);
    ImGui::SameLine();

    CalPoint& p = calPts_[calStep_];
    if (!p.placed) {
        ImGui::TextDisabled("click the chart where a point you can identify is");
    } else {
        ImGui::Text("placed at %.0f, %.0f px", p.px, p.py);

        int src = static_cast<int>(calSource_);
        ImGui::RadioButton("Navaid / airport", &src, 0);
        ImGui::SameLine();
        ImGui::RadioButton("Aircraft position", &src, 1);
        ImGui::SameLine();
        ImGui::RadioButton("Lat / Lon", &src, 2);
        calSource_ = static_cast<CalSource>(src);

        if (calSource_ == CalSource::Navaid) {
            ImGui::SetNextItemWidth(90.f);
            bool enter = ImGui::InputTextWithHint(
                "##calnav", "ident", calNavBuf_, sizeof(calNavBuf_),
                ImGuiInputTextFlags_CharsUppercase
                | ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::SmallButton("Set##calnav") || enter) {
                double la = 0, lo = 0;
                std::string nm;
                if (navaidLatLon(calNavBuf_, la, lo, nm)) {
                    p.lat = la; p.lon = lo; p.fixed = true;
                    calMsg_ = nm;
                } else {
                    p.fixed = false;
                    calMsg_ = std::string("Not in the X-Plane nav database: ") + calNavBuf_;
                }
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Airport, VOR, NDB or fix printed on the chart.");
        } else if (calSource_ == CalSource::Aircraft) {
            if (ImGui::SmallButton("Take the aircraft position") && drLat_ && drLon_) {
                p.lat = XPLMGetDatad(drLat_);
                p.lon = XPLMGetDatad(drLon_);
                p.fixed = true;
                char b[96];
                snprintf(b, sizeof(b), "%.6f / %.6f", p.lat, p.lon);
                calMsg_ = b;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Park or stop where you clicked on the chart.");
        } else {
            ImGui::SetNextItemWidth(110.f);
            ImGui::InputTextWithHint("##callat", "55 58 21 N",
                                     calLatBuf_, sizeof(calLatBuf_));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(110.f);
            ImGui::InputTextWithHint("##callon", "037 24 47 E",
                                     calLonBuf_, sizeof(calLonBuf_));
            ImGui::SameLine();
            if (ImGui::SmallButton("Set##calll")) {
                double la = 0, lo = 0;
                if (parseLatLon(calLatBuf_, true, la)
                    && parseLatLon(calLonBuf_, false, lo)) {
                    p.lat = la; p.lon = lo; p.fixed = true;
                    char b[96];
                    snprintf(b, sizeof(b), "%.6f / %.6f", la, lo);
                    calMsg_ = b;
                } else {
                    p.fixed = false;
                    calMsg_ = "Could not read those coordinates.";
                }
            }
        }
    }

    if (calStep_ == 1) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Back to point 1")) calStep_ = 0;
    } else if (calPts_[0].fixed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Next point")) calStep_ = 1;
    }
    if (calPts_[0].fixed && calPts_[1].fixed) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Apply") && tex) applyCalibration(tab, v, *tex);
    }
    if (!calMsg_.empty())
        ImGui::TextColored(ImVec4(0.95f, 0.85f, 0.35f, 1.f), "%s", calMsg_.c_str());
}

// ── UI: chart canvas (right panel) ───────────────────────────────────────────

void ChartsWindow::renderCanvas(chart::ChartTab& tab, TabView& v)
{
    using cp::notepad::Point;

    // ── Toolbar ─────────────────────────────────────────────────────────────
    auto toolButton = [&](const char* label, Tool tool, bool pan) {
        bool active = pan ? panTool_ : (!panTool_ && currentTool_ == tool);
        if (active)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(kAccentR, kAccentG, kAccentB, 1.f));
        if (ImGui::SmallButton(label)) {
            panTool_ = pan;
            if (!pan) currentTool_ = tool;
        }
        if (active) ImGui::PopStyleColor();
        ImGui::SameLine();
    };
    toolButton("Pan",     Tool::Pen,     true);
    toolButton("Pen",     Tool::Pen,     false);
    toolButton("Eraser",  Tool::Eraser,  false);
    toolButton("Line",    Tool::Line,    false);
    toolButton("Rect",    Tool::Rect,    false);
    toolButton("Ellipse", Tool::Ellipse, false);

    ImGui::SetNextItemWidth(90.f);
    ImGui::SliderFloat("##thick", &currentThickness_, 1.f, 20.f, "%.0f px");
    ImGui::SameLine();

    uint8_t myPid = sess_ ? static_cast<uint8_t>(sess_->myId()) : 0;
    uint32_t col32 = colorForParticipant(myPid);
    ImGui::ColorButton("##mycolor",
                       ImVec4(((col32 >> 0) & 0xFF) / 255.f,
                              ((col32 >> 8) & 0xFF) / 255.f,
                              ((col32 >> 16) & 0xFF) / 255.f, 1.f),
                       ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoPicker,
                       ImVec2(16, 16));

    const Tex* tex = tab.activeChartId.empty()
                   ? nullptr : textureFor(tab.activeChartId, v.page);

    // Recovery controls: a failed fetch, a rejected download or a chart file
    // that decoded to garbage all used to be dead ends until another chart was
    // selected.
    bool failed = !v.pipelineError.empty() || (tex && !tex->error.empty());
    if (failed && !tab.activeChartId.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Retry")) reloadChart(tab, v, false);
        ImGui::SameLine();
        if (ImGui::SmallButton("Re-download")) reloadChart(tab, v, true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Deletes the cached file and fetches it again.");
    }
    if (!tab.activeChartId.empty()) {
        auto dit = detailCache_.find(tab.activeChartId);
        if (dit != detailCache_.end() && !dit->second.viewUrl.empty()
            && (dit->second.requiresPreauth || dit->second.fileUrl.empty())) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Open on chartfox.org"))
                openInBrowser(dit->second.viewUrl);
        }
    }

    // Manual calibration: roughly three quarters of the charts ChartFox serves
    // carry no georeference, so the crew pins those down themselves.
    if (tex && tex->glId && !tab.activeChartId.empty()) {
        const chart::CalEntry* mine = cals_.findOwn(tab.activeChartId, v.page + 1);
        const bool hasManual = mine != nullptr;
        ImGui::SameLine();
        if (calMode_) {
            if (ImGui::SmallButton("Cancel calibration")) cancelCalibration();
        } else {
            if (ImGui::SmallButton(hasManual ? "Recalibrate" : "Calibrate"))
                beginCalibration(tab, v);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pin this chart to the world with two reference"
                                  "\npoints, so the aircraft symbol works even when"
                                  "\nChartFox publishes no georeference.");
            if (hasManual) {
                ImGui::SameLine();
                bool cleared = false;
                if (ImGui::SmallButton("Clear calibration")) {
                    cals_.erase(tab.activeChartId, v.page + 1);
                    netCalSet(tab.activeChartId,
                              static_cast<uint8_t>(v.page + 1), true,
                              tab.icao, false, chart::ChartGeoref{});
                    cleared = true;   // `mine` is gone with it
                }
                // Sharing costs one click: the plugin holds no write token, so
                // the upload goes through the project's submission endpoint,
                // and falls back to a prefilled issue when there is none.
                if (!cleared) {
                    ImGui::SameLine();
                    const bool alreadyShared = mine->shared;
                    if (alreadyShared) ImGui::BeginDisabled();
                    if (ImGui::SmallButton(alreadyShared ? "Shared" : "Share to community"))
                        shareCalibration(tab.activeChartId, v.page + 1);
                    if (alreadyShared) ImGui::EndDisabled();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip(alreadyShared
                            ? "Already sent to the shared database."
                            : "Send this calibration to the shared database so"
                              "\nother pilots get the aircraft symbol on this chart.");
                }
            }
        }
    }

    // Page selector for multi-page PDFs.
    if (tex && tex->pageCount > 1) {
        ImGui::SameLine();
        if (ImGui::SmallButton("<##pg") && v.page > 0) {
            --v.page;
            v.zoom = 0.f;
        }
        ImGui::SameLine();
        ImGui::Text("p.%d/%d", v.page + 1, tex->pageCount);
        ImGui::SameLine();
        if (ImGui::SmallButton(">##pg") && v.page + 1 < tex->pageCount) {
            ++v.page;
            v.zoom = 0.f;
        }
        // The new page renders lazily via tickPipeline/requestRenderIfNeeded.
    }

    renderCalibrationPanel(tab, v, tex);

    if (!calCloud_.status().empty()) {
        ImGui::PushTextWrapPos();
        ImGui::TextColored(calCloud_.statusIsError() ? ImVec4(0.95f, 0.6f, 0.3f, 1.f)
                                                     : ImVec4(0.5f, 0.85f, 0.5f, 1.f),
                           "%s", calCloud_.status().c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Separator();

    // ── Canvas child ────────────────────────────────────────────────────────
    ImGui::BeginChild("##chart_canvas", ImVec2(0, 0), false,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImVec2 avail  = ImGui::GetContentRegionAvail();
    if (avail.x < 16.f) avail.x = 16.f;
    if (avail.y < 16.f) avail.y = 16.f;
    ImVec2 clipMax(origin.x + avail.x, origin.y + avail.y);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, clipMax, true);
    dl->AddRectFilled(origin, clipMax, 0xFF15171Au);

    auto centeredMsg = [&](const char* msg, ImU32 col) {
        ImVec2 sz = ImGui::CalcTextSize(msg, nullptr, false, avail.x - 40.f);
        dl->AddText(nullptr, 0.f,
                    ImVec2(origin.x + (avail.x - sz.x) * 0.5f,
                           origin.y + (avail.y - sz.y) * 0.5f),
                    col, msg, nullptr, avail.x - 40.f);
    };

    if (tab.activeChartId.empty()) {
        centeredMsg(tab.icao.empty()
                    ? "Enter an airport ICAO on the left."
                    : "Select a chart from the list.",
                    IM_COL32(160, 160, 160, 255));
    } else if (!v.pipelineError.empty()) {
        centeredMsg(v.pipelineError.c_str(), IM_COL32(240, 90, 90, 255));
    } else if (tex && !tex->error.empty()) {
        centeredMsg(tex->error.c_str(), IM_COL32(240, 90, 90, 255));
    } else if (!tex || !tex->glId) {
        centeredMsg("Loading chart...", IM_COL32(160, 160, 160, 255));
    }

    if (tex && tex->glId) {
        // First display of this chart/page: fit and center.
        if (v.zoom <= 0.f) {
            float zx = avail.x / tex->w, zy = avail.y / tex->h;
            v.zoom = (zx < zy ? zx : zy);
            if (v.zoom > 2.f) v.zoom = 2.f;
            if (v.zoom < 0.02f) v.zoom = 0.02f;
            v.panX = (tex->w - avail.x / v.zoom) * 0.5f;
            v.panY = (tex->h - avail.y / v.zoom) * 0.5f;
        }

        ImVec2 img0(origin.x + (0.f - v.panX) * v.zoom,
                    origin.y + (0.f - v.panY) * v.zoom);
        ImVec2 img1(origin.x + (tex->w - v.panX) * v.zoom,
                    origin.y + (tex->h - v.panY) * v.zoom);
        dl->AddImage((ImTextureID)(intptr_t)tex->glId, img0, img1);

        // Committed strokes for this (chart, page), canonical → screen.
        auto toScreen = [&](const Point& p) {
            return ImVec2(origin.x + (p.x - v.panX) * v.zoom,
                          origin.y + (p.y - v.panY) * v.zoom);
        };
        auto drawStroke = [&](const chart::Stroke& s, ImU32 imCol, float thickPx) {
            switch (s.tool) {
            case Tool::Pen:
            case Tool::Eraser: {
                std::vector<ImVec2> verts;
                verts.reserve(s.pts.size());
                for (const auto& p : s.pts) verts.push_back(toScreen(p));
                if (verts.size() >= 2)
                    dl->AddPolyline(verts.data(), (int)verts.size(), imCol, 0, thickPx);
                else if (verts.size() == 1)
                    dl->AddCircleFilled(verts[0], thickPx * 0.5f, imCol);
                break;
            }
            case Tool::Line:
                if (s.pts.size() >= 2)
                    dl->AddLine(toScreen(s.pts[0]), toScreen(s.pts[1]), imCol, thickPx);
                break;
            case Tool::Rect:
                if (s.pts.size() >= 2)
                    dl->AddRect(toScreen(s.pts[0]), toScreen(s.pts[1]), imCol,
                                0.f, 0, thickPx);
                break;
            case Tool::Ellipse:
                if (s.pts.size() >= 2) {
                    ImVec2 a = toScreen(s.pts[0]), b = toScreen(s.pts[1]);
                    dl->AddEllipse(ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f),
                                   std::fabs(b.x - a.x) * 0.5f,
                                   std::fabs(b.y - a.y) * 0.5f,
                                   imCol, 0.f, 48, thickPx);
                }
                break;
            }
        };

        chart::ChartAnnots* annots =
            tab.findAnnots(tab.activeChartId, static_cast<uint8_t>(v.page));
        if (annots) {
            for (const auto& s : annots->strokes) {
                uint32_t c = s.colorRGBA;
                ImU32 imCol = IM_COL32((c >> 0) & 0xFF, (c >> 8) & 0xFF,
                                       (c >> 16) & 0xFF, (c >> 24) & 0xFF);
                drawStroke(s, imCol, s.thickness * v.zoom);
            }
        }

        // In-progress scratch stroke preview.
        if (drawing_ && drawingTab_ == tab.id && drawingChart_ == tab.activeChartId
            && drawingPage_ == v.page && !scratchStroke_.pts.empty()) {
            uint32_t sc = colorForParticipant(myPid);
            drawStroke(scratchStroke_,
                       IM_COL32((sc >> 0) & 0xFF, (sc >> 8) & 0xFF,
                                (sc >> 16) & 0xFF, (sc >> 24) & 0xFF),
                       scratchStroke_.thickness * v.zoom);
        }

        // Reference points being placed by hand.
        if (calMode_ && calChartId_ == tab.activeChartId && calPage_ == v.page) {
            for (int i = 0; i < 2; ++i) {
                if (!calPts_[i].placed) continue;
                ImVec2 sp(origin.x + (static_cast<float>(calPts_[i].px) - v.panX) * v.zoom,
                          origin.y + (static_cast<float>(calPts_[i].py) - v.panY) * v.zoom);
                ImU32 col = calPts_[i].fixed ? IM_COL32(80, 230, 120, 240)
                                             : IM_COL32(255, 130, 60, 240);
                dl->AddCircle(sp, 10.f, col, 20, 2.f);
                dl->AddLine(ImVec2(sp.x - 15.f, sp.y), ImVec2(sp.x + 15.f, sp.y), col, 1.5f);
                dl->AddLine(ImVec2(sp.x, sp.y - 15.f), ImVec2(sp.x, sp.y + 15.f), col, 1.5f);
                const char lb[2] = { static_cast<char>('1' + i), 0 };
                dl->AddText(ImVec2(sp.x + 13.f, sp.y - 20.f), col, lb);
            }
        }

        std::string geoStatus =
            renderAircraftOverlay(dl, tab, v, *tex, origin, origin, clipMax);

        // Attribution (required by chart sources) and, above it, why the
        // aircraft symbol is or is not there - a [G] chart without a symbol
        // used to be unexplained.
        float lineY = clipMax.y - 6.f;
        auto bottomLine = [&](const std::string& text, ImU32 col) {
            if (text.empty()) return;
            ImVec2 sz = ImGui::CalcTextSize(text.c_str());
            ImVec2 p0(origin.x + 4.f, lineY - sz.y);
            dl->AddRectFilled(ImVec2(p0.x - 2.f, p0.y - 2.f),
                              ImVec2(p0.x + sz.x + 4.f, p0.y + sz.y + 2.f),
                              IM_COL32(0, 0, 0, 150));
            dl->AddText(p0, col, text.c_str());
            lineY -= sz.y + 4.f;
        };
        auto dit = detailCache_.find(tab.activeChartId);
        if (dit != detailCache_.end())
            bottomLine(dit->second.copyrightShort, IM_COL32(220, 220, 220, 220));
        bottomLine(geoStatus, IM_COL32(255, 210, 30, 200));
    }

    dl->PopClipRect();

    // ── Input ───────────────────────────────────────────────────────────────
    ImGui::InvisibleButton("##canvas_input", avail);
    bool hovered = ImGui::IsItemHovered();
    bool active  = ImGui::IsItemActive();
    ImGuiIO& io  = ImGui::GetIO();

    if (tex && tex->glId) {
        ImVec2 mp = ImGui::GetMousePos();
        // Canonical coordinates of the cursor.
        float cx = v.panX + (mp.x - origin.x) / v.zoom;
        float cy = v.panY + (mp.y - origin.y) / v.zoom;

        // Wheel zoom about the cursor.
        if (hovered && io.MouseWheel != 0.f) {
            float nz = v.zoom * std::pow(1.15f, io.MouseWheel);
            if (nz < 0.02f) nz = 0.02f;
            if (nz > 8.f)   nz = 8.f;
            v.panX = cx - (mp.x - origin.x) / nz;
            v.panY = cy - (mp.y - origin.y) / nz;
            v.zoom = nz;
        }

        // Right-drag pan (always available; the Pan tool pans with the left button).
        bool panDrag = (v.panningRmb || (hovered && ImGui::IsMouseClicked(1)))
                     && ImGui::IsMouseDown(1);
        bool panLeft = panTool_ && active && ImGui::IsMouseDown(0);
        if (panDrag || panLeft) {
            v.panningRmb = panDrag;
            ImVec2 d = io.MouseDelta;
            v.panX -= d.x / v.zoom;
            v.panY -= d.y / v.zoom;
        } else {
            v.panningRmb = false;
        }

        // Drawing (notepad toolset in canonical coordinates).
        bool canDraw = !panTool_ && !calMode_;
        float clampX = cx < 0.f ? 0.f : (cx > tex->w ? (float)tex->w : cx);
        float clampY = cy < 0.f ? 0.f : (cy > tex->h ? (float)tex->h : cy);

        if (calMode_ && hovered && ImGui::IsMouseClicked(0)
            && calChartId_ == tab.activeChartId && calPage_ == v.page)
            calibrationClick(clampX, clampY);

        // Cancel a gesture whose context disappeared (chart/page switched).
        if (drawing_ && (drawingTab_ != tab.id || drawingChart_ != tab.activeChartId
                         || drawingPage_ != v.page)) {
            drawing_ = false;
            scratchStroke_ = {};
            erasedThisStroke_.clear();
        }

        if (canDraw && hovered && ImGui::IsMouseClicked(0)) {
            drawing_      = true;
            drawingTab_   = tab.id;
            drawingChart_ = tab.activeChartId;
            drawingPage_  = v.page;
            scratchStroke_ = {};
            if (currentTool_ == Tool::Eraser) {
                erasedThisStroke_.clear();
            } else {
                scratchStroke_.tool      = currentTool_;
                // Thickness is canonical so the ink stays glued to the chart:
                // store the on-screen slider value divided by the current zoom.
                scratchStroke_.thickness = currentThickness_ / v.zoom;
                Point p0{clampX, clampY};
                scratchStroke_.pts.push_back(p0);
                if (currentTool_ == Tool::Line || currentTool_ == Tool::Rect
                 || currentTool_ == Tool::Ellipse)
                    scratchStroke_.pts.push_back(p0);
            }
        }

        if (canDraw && drawing_ && active && drawingTab_ == tab.id) {
            if (currentTool_ == Tool::Eraser) {
                chart::ChartAnnots* annots =
                    tab.findAnnots(tab.activeChartId, static_cast<uint8_t>(v.page));
                if (annots) {
                    float eRadius = (currentThickness_ > 4.f ? currentThickness_ : 4.f)
                                  / v.zoom;
                    Point probe{cx, cy};
                    std::vector<chart::NpId> toErase;
                    for (const auto& s : annots->strokes)
                        if (!erasedThisStroke_.count(s.id)
                            && cp::notepad::strokeHit(s, probe, eRadius))
                            toErase.push_back(s.id);
                    for (auto eid : toErase) {
                        erasedThisStroke_.insert(eid);
                        annots->removeStroke(eid);
                        if (tab.shared)
                            netStrokeDel(tab.id, tab.activeChartId,
                                         static_cast<uint8_t>(v.page), eid);
                    }
                }
            } else if (currentTool_ == Tool::Pen) {
                if (scratchStroke_.pts.size() < cp::notepad::Stroke::MAX_POINTS
                    && !scratchStroke_.pts.empty()) {
                    const auto& last = scratchStroke_.pts.back();
                    float ddx = clampX - last.x, ddy = clampY - last.y;
                    float minD = 1.5f / v.zoom;   // same screen-space density as notepad
                    if (ddx * ddx + ddy * ddy >= minD * minD)
                        scratchStroke_.pts.push_back({clampX, clampY});
                }
            } else {
                if (scratchStroke_.pts.size() >= 2)
                    scratchStroke_.pts[1] = {clampX, clampY};
            }
        }

        if (drawing_ && ImGui::IsMouseReleased(0) && drawingTab_ == tab.id) {
            drawing_ = false;
            if (currentTool_ == Tool::Eraser || panTool_) {
                erasedThisStroke_.clear();
            } else if (!scratchStroke_.pts.empty()) {
                scratchStroke_.id        = mintId();
                scratchStroke_.colorRGBA = colorForParticipant(myPid);
                tab.annotsFor(tab.activeChartId, static_cast<uint8_t>(v.page))
                   .applyStroke(scratchStroke_);
                if (tab.shared)
                    netStrokeAdd(tab.id, tab.activeChartId,
                                 static_cast<uint8_t>(v.page), scratchStroke_);
            }
            scratchStroke_ = {};
        }

        // Eraser cursor preview.
        if (canDraw && hovered && currentTool_ == Tool::Eraser) {
            float eRadius = currentThickness_ > 4.f ? currentThickness_ : 4.f;
            ImGui::GetForegroundDrawList()->AddCircle(
                mp, eRadius, IM_COL32(200, 200, 200, 160), 24, 1.5f);
        }
    }

    ImGui::EndChild();
}

// ── UI: one tab ──────────────────────────────────────────────────────────────

void ChartsWindow::renderTab(chart::ChartTab& tab)
{
    TabView& v = viewFor(tab.id);
    bool owner = isTabOwner(tab);

    // Tab header: share state + delete.
    if (!tab.shared) {
        if (!owner) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Share")) {
            tab.shared = true;
            netTabShare(tab);
            if (!tab.activeChartId.empty())
                netTabSetChart(tab.id, tab.activeChartId);
            for (const auto& [key, annots] : tab.annots)
                for (const auto& stroke : annots.strokes)
                    netStrokeAdd(tab.id, key.first, key.second, stroke);
        }
        if (!owner) ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Share this tab with the crew: airport, selected\n"
                              "chart and drawings sync (each member still loads\n"
                              "the chart with their own ChartFox token).");
    } else {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 0.9f, 0.3f, 1.f));
        ImGui::TextUnformatted("[Shared]");
        ImGui::PopStyleColor();
    }
    ImGui::SameLine();

    bool tabDeleted = false;
    if (!owner) ImGui::BeginDisabled();
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.65f, 0.10f, 0.10f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.85f, 0.15f, 0.15f, 1.f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.05f, 0.05f, 1.f));
    if (ImGui::SmallButton("Del Tab")) tabDeleted = true;
    ImGui::PopStyleColor(3);
    if (!owner) ImGui::EndDisabled();

    ImGui::Separator();

    if (tabDeleted) {
        if (tab.shared) netTabDel(tab.id);
        onTabDel(tab.id);
        return;   // tab is gone — do not touch it below
    }

    tickPipeline(tab, v);

    // Left: chart list + METAR.  Right: canvas.
    ImGui::BeginChild("##left_panel", ImVec2(260.f, 0), true);
    renderChartList(tab, v);
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##right_panel", ImVec2(0, 0), false);
    renderCanvas(tab, v);
    ImGui::EndChild();
}

// ── Main render ──────────────────────────────────────────────────────────────

void ChartsWindow::renderContent()
{
    ++frameCounter_;
    // auth_.tick() runs in the flight loop (tickAuth) so a login completes and
    // tokens keep refreshing even while this window is hidden.
    pollRenders();

    if (targetW_ < 1.f) {
        targetW_ = (float)xpwWidth();
        targetH_ = (float)xpwHeight();
    }

    ImGui::SetNextWindowSize(ImVec2(targetW_, targetH_), ImGuiCond_Always);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoMove
                        | ImGuiWindowFlags_NoCollapse
                        | ImGuiWindowFlags_NoResize
                        | ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (!ImGui::Begin("Charts", nullptr, wf)) { xpwEndWindow(); return; }

    // Custom resize grip (bottom-right corner), same as NotepadWindow.
    {
        const float gripSize = 20.f;
        ImVec2 winPos  = ImGui::GetWindowPos();
        ImVec2 winSize = ImGui::GetWindowSize();
        ImVec2 gripMin(winPos.x + winSize.x - gripSize, winPos.y + winSize.y - gripSize);
        ImVec2 gripMax(winPos.x + winSize.x,            winPos.y + winSize.y);

        ImVec2 mouse = ImGui::GetMousePos();
        bool inGrip = !resizing_ &&
                      (mouse.x >= gripMin.x && mouse.x <= gripMax.x &&
                       mouse.y >= gripMin.y && mouse.y <= gripMax.y);

        if (inGrip || resizing_)
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNWSE);

        if (inGrip && ImGui::IsMouseClicked(0)) {
            resizing_          = true;
            resizeStartMouseX_ = mouse.x;
            resizeStartMouseY_ = mouse.y;
            resizeStartW_      = winSize.x;
            resizeStartH_      = winSize.y;
        }
        if (resizing_) {
            if (ImGui::IsMouseDown(0)) {
                float newW = resizeStartW_ + (mouse.x - resizeStartMouseX_);
                float newH = resizeStartH_ + (mouse.y - resizeStartMouseY_);
                newW = (newW < 560.f) ? 560.f : newW;
                newH = (newH < 380.f) ? 380.f : newH;
                targetW_ = newW;
                targetH_ = newH;
                xpwSetGeometry(xpwLeft(), xpwTop(),
                               xpwLeft() + (int)newW,
                               xpwTop()  - (int)newH);
            } else {
                resizing_ = false;
            }
        }

        ImDrawList* fg = ImGui::GetForegroundDrawList();
        ImU32 gripCol = (inGrip || resizing_)
                      ? IM_COL32(140, 180, 255, 220)
                      : IM_COL32(100, 130, 180, 140);
        for (int i = 1; i <= 3; ++i) {
            float off = gripSize * 0.28f * (float)i;
            fg->AddLine(ImVec2(gripMax.x - off, gripMax.y),
                        ImVec2(gripMax.x,        gripMax.y - off),
                        gripCol, 1.5f);
        }
    }

    renderTokenSection();

    if (ImGui::BeginTabBar("##cf_tabs", ImGuiTabBarFlags_AutoSelectNewTabs)) {

        if (ImGui::TabItemButton("+##newcharttab", ImGuiTabItemFlags_Trailing)) {
            chart::ChartTab t;
            t.id     = mintId();
            t.name   = "APT";
            t.shared = false;
            charts_.tabs.push_back(std::move(t));
        }

        for (auto& tab : charts_.tabs) {
            char tabLabel[80];
            snprintf(tabLabel, sizeof(tabLabel), "%s%s##%u",
                     tab.name.empty() ? "APT" : tab.name.c_str(),
                     tab.shared ? " [S]" : "",
                     tab.id);
            if (ImGui::BeginTabItem(tabLabel)) {
                chart::NpId tabId = tab.id;   // renderTab may erase the tab
                renderTab(tab);
                ImGui::EndTabItem();
                if (!charts_.findTab(tabId)) break;   // iterator invalidated
            }
        }

        ImGui::EndTabBar();
    }

    xpwEndWindow();
}

}
}
