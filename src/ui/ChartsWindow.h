#pragma once
#include "XPImguiWindow.h"
#include "../chart/ChartTypes.h"
#include "../chart/ChartFoxAuth.h"
#include "../chart/ChartFoxClient.h"
#include "../chart/ChartRenderer.h"
#include "../chart/ChartCal.h"
#include "../chart/ChartCalCloud.h"
#include "../chart/Georef.h"
#include "../net/HttpFetch.h"
#include "../session/Session.h"

#include <XPLM/XPLMDataAccess.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace cp {
namespace ui {

// ---------------------------------------------------------------------------
// ChartsWindow — ChartFox airport charts with crew-shared annotations.
//
// Modeled on NotepadWindow: tabs (one per airport), one-way Share, the full
// notepad drawing toolset, host-authoritative relay via sendFn / on*().
// Per tab: chart list (grouped by type), the chart canvas with zoom/pan,
// drawing in canonical chart pixels, aircraft position from the chart's
// georeference, METAR for the tab's ICAO (aviationweather.gov).
//
// Chart files are downloaded by each client with its own ChartFox token and
// are never sent over the session (source terms of use); shared tabs carry
// only ICAO, selected chart ID and strokes.
// ---------------------------------------------------------------------------
class ChartsWindow : public XPImguiWindow {
public:
    // pluginDir: folder containing win.xpl (pdfium.dll lives next to it).
    // xpSystemPath: X-Plane root with trailing separator (chart disk cache).
    bool init(const std::string& pluginDir, const std::string& xpSystemPath);
    void shutdown();

    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

    void setSession(const Session* s) { sess_ = s; }

    // ChartFox personal access token — fallback auth (from Prefs / user edit).
    void setToken(const std::string& t);
    // Fired when the user saves a new PAT in the window; plugin persists it.
    std::function<void(const std::string&)> onTokenChanged;

    // ChartFox OAuth (preferred): developer-portal client + per-user tokens.
    void setOauth(const std::string& clientId, const std::string& clientSecret,
                  const std::string& redirectUri, const std::string& scopes,
                  const std::string& accessToken, const std::string& refreshToken,
                  int64_t expiryUnix, const std::string& tokenScopes);

    // Drives the OAuth code exchange and token refresh.  Called from the flight
    // loop, not from rendering: a login started right before the window is
    // closed must still complete, and the token must keep refreshing while the
    // window is hidden.
    void tickBackground();

    // Prefs override for the community upload endpoint (empty = use the one
    // published in the repository).
    void setCalSubmitUrl(const std::string& url) { calCloud_.setSubmitUrlOverride(url); }
    // Fired when the user edits the OAuth client fields; plugin persists them.
    std::function<void(const std::string& clientId, const std::string& clientSecret,
                       const std::string& redirectUri, const std::string& scopes)>
        onOauthClientChanged;
    // Fired whenever access/refresh tokens change (login/refresh/logout).
    std::function<void(const std::string& access, const std::string& refresh,
                       int64_t expiryUnix, const std::string& tokenScopes)>
        onOauthTokensChanged;

    // Push a framed TCP message to the network (same wiring as NotepadWindow).
    std::function<void(std::vector<uint8_t>)> sendFn;

    // ── Inbound chart events (flight loop, main thread) ─────────────────────
    void onTabShare  (chart::NpId tabId, const std::string& icao, const std::string& name);
    void onTabSetAirport(chart::NpId tabId, const std::string& icao);
    void onTabSetChart  (chart::NpId tabId, const std::string& chartId);
    void onStrokeAdd (chart::NpId tabId, const std::string& chartId, uint8_t page,
                      const chart::Stroke& stroke);
    void onStrokeDel (chart::NpId tabId, const std::string& chartId, uint8_t page,
                      chart::NpId strokeId);
    void onTabDel    (chart::NpId tabId);
    void onSnapTab   (chart::NpId tabId, const std::string& icao, const std::string& name,
                      const std::string& activeChartId,
                      const std::string& chartId, uint8_t page, bool isFirstChunk,
                      const std::vector<chart::Stroke>& strokes);
    void onSnapEnd() {}
    // Manual calibration of (chartId, 1-based page); clear=true removes it.
    void onCalSet(const std::string& chartId, uint8_t page1, bool clear,
                  const std::string& icao, bool isOverride,
                  const chart::ChartGeoref& g);

    // Our calibrations — the host answers CF_SNAP_REQ with them.  Community
    // ones are left out on purpose: every client fetches those itself.
    const std::map<std::string, chart::CalEntry>& calibrations() const
    { return cals_.own(); }

    // Drop all shared tabs on disconnect (private tabs are kept).
    void resetShared();

    // Read-only model access (plugin host snapshot).
    const chart::SharedCharts& charts() const { return charts_; }

protected:
    void renderContent() override;

private:
    // Per-tab local view state — never networked.
    struct TabView {
        char icaoBuf[8]   = {};
        char filterBuf[48] = {};

        net::AsyncHttp::Handle listHandle = net::AsyncHttp::INVALID;
        chart::ChartListResult list;
        std::string listIcao;              // ICAO the current list belongs to

        net::AsyncHttp::Handle metarHandle = net::AsyncHttp::INVALID;
        std::string metarText;
        std::string metarIcao;
        double      metarTime = 0.0;       // ImGui time of last successful fetch

        // Active-chart load pipeline.
        std::string pipelineChartId;
        net::AsyncHttp::Handle detailHandle = net::AsyncHttp::INVALID;
        net::AsyncHttp::Handle dlHandle     = net::AsyncHttp::INVALID;
        bool        fileReady = false;
        bool        authRetried = false;   // one 401 -> token-refresh retry
        bool        dlNoAuth    = false;   // retry the file without the bearer
        std::string pipelineError;

        // View transform: screen = origin + (canonical - pan) * zoom.
        float zoom = 0.f;                  // 0 → fit-to-canvas on next draw
        float panX = 0.f, panY = 0.f;
        int   page = 0;                    // 0-based, local per client
        bool  panningRmb = false;
    };

    struct TexKey {
        std::string chartId;
        int         page = 0;
        bool operator<(const TexKey& o) const
        { return chartId != o.chartId ? chartId < o.chartId : page < o.page; }
    };
    struct Tex {
        unsigned int glId = 0;             // 0 while failed (see error)
        int      w = 0, h = 0;
        int      pageCount = 1;
        double   pageWpt = 0, pageHpt = 0;
        double   canonicalScale = 1.0;
        uint64_t lastUsed = 0;
        std::string error;
    };
    struct PendingRender {
        chart::ChartRenderer::Handle handle;
        TexKey key;
    };
    // Where a calibration point's coordinates come from.
    enum class CalSource { Navaid, Aircraft, LatLon };

    // A chart being calibrated by hand: two (page pixel, lat/lon) pairs.
    struct CalPoint {
        bool   placed = false;      // clicked on the chart
        bool   fixed  = false;      // coordinates resolved
        double px = 0, py = 0;      // canonical chart pixels
        double lat = 0, lon = 0;
    };

    // ── Helpers ─────────────────────────────────────────────────────────────
    chart::NpId mintId();
    bool isTabOwner(const chart::ChartTab& tab) const;
    TabView& viewFor(chart::NpId tabId) { return views_[tabId]; }

    void selectChart(chart::ChartTab& tab, TabView& v, const std::string& chartId);
    void loadAirport(chart::ChartTab& tab, TabView& v, const std::string& icao);
    void fetchMetar(TabView& v, const std::string& icao);
    void tickPipeline(chart::ChartTab& tab, TabView& v);
    // Restarts the active chart's pipeline.  purgeFile also deletes the cached
    // file and the rendered pages, for a download that produced garbage.
    void reloadChart(chart::ChartTab& tab, TabView& v, bool purgeFile);
    // True when a 401 was answered by asking for a token refresh; the caller
    // just waits, the pipeline restarts once the new token lands.
    bool handleAuthFailure(TabView& v, int status);
    void pollRenders();
    const Tex* textureFor(const std::string& chartId, int page);
    void requestRenderIfNeeded(TabView& v, const std::string& chartId, int page);
    void evictTextures();
    void dropAllTextures();

    void renderTokenSection();
    void renderTab(chart::ChartTab& tab);
    void renderChartList(chart::ChartTab& tab, TabView& v);
    void renderMetarBox(chart::ChartTab& tab, TabView& v);
    void renderCanvas(chart::ChartTab& tab, TabView& v);
    // Returns the one-line georeference status shown under the chart.
    std::string renderAircraftOverlay(ImDrawList* dl, const chart::ChartTab& tab,
                                      const TabView& v, const Tex& tex,
                                      ImVec2 origin, ImVec2 clipMin, ImVec2 clipMax);

    // The georeference in force for a page.  Ours wins over everything (that
    // is also how a wrong ChartFox georeference gets fixed), then a community
    // calibration made to correct a georeference, then ChartFox itself, and
    // last a community calibration for a chart that has none.
    const chart::ChartGeoref* georefFor(const std::string& chartId, int page0,
                                        chart::CalOrigin* outOrigin) const;

    // ── Hand calibration ────────────────────────────────────────────────────
    void beginCalibration(const chart::ChartTab& tab, const TabView& v);
    void cancelCalibration();
    void renderCalibrationPanel(chart::ChartTab& tab, TabView& v, const Tex* tex);
    // Canvas click while calibrating; px/py are canonical chart pixels.
    void calibrationClick(double px, double py);
    void applyCalibration(const chart::ChartTab& tab, const TabView& v, const Tex& tex);
    void netCalSet(const std::string& chartId, uint8_t page1, bool clear,
                   const std::string& icao, bool isOverride,
                   const chart::ChartGeoref& g);
    // Upload our calibrations for this chart page, or all of them.
    void shareCalibration(const std::string& chartId, int page1);
    void shareAllCalibrations();
    // Send whatever has not reached the shared database yet, by itself.
    void autoShare();
    // Coordinate text as printed on charts: 55.9725 / 55°58'21"N / N55 58 21.
    static bool parseLatLon(const std::string& text, bool isLat, double& out);
    // Airport / VOR / NDB / fix position from X-Plane's nav database.
    static bool navaidLatLon(const std::string& ident, double& lat, double& lon,
                             std::string& outName);

    // Network send helpers (framed via cp::proto::MsgBuilder → sendFn).
    void netTabShare(const chart::ChartTab& tab);
    void netTabSetAirport(chart::NpId tabId, const std::string& icao);
    void netTabSetChart(chart::NpId tabId, const std::string& chartId);
    void netStrokeAdd(chart::NpId tabId, const std::string& chartId, uint8_t page,
                      const chart::Stroke& stroke);
    void netStrokeDel(chart::NpId tabId, const std::string& chartId, uint8_t page,
                      chart::NpId strokeId);
    void netTabDel(chart::NpId tabId);

    // ── State ───────────────────────────────────────────────────────────────
    const Session*       sess_ = nullptr;
    chart::SharedCharts  charts_;
    std::map<chart::NpId, TabView> views_;
    uint32_t             cfCounter_ = 0;   // NpId minting (separate from notepad)

    net::AsyncHttp        http_;
    chart::ChartFoxAuth   auth_;
    chart::ChartFoxClient cfx_;
    chart::ChartRenderer  renderer_;

    std::map<std::string, chart::ChartDetail> detailCache_;
    std::map<TexKey, Tex> texCache_;
    chart::ChartCalStore cals_;
    chart::ChartCalCloud calCloud_;
    int  autoShareTick_ = 0;     // flight loops until the next automatic upload
    bool georefDebug_ = false;   // show the raw georeference numbers on the chart

    // Hand calibration in progress (one chart at a time).
    bool        calMode_ = false;
    std::string calChartId_;
    int         calPage_ = 0;            // 0-based, as TabView::page
    CalPoint    calPts_[2];
    int         calStep_ = 0;            // which point is being placed (0/1)
    CalSource   calSource_ = CalSource::Navaid;
    char        calNavBuf_[16] = {};
    char        calLatBuf_[32] = {};
    char        calLonBuf_[32] = {};
    std::string calMsg_;                 // resolution / validation feedback
    bool        calOverride_ = false;    // this page already had a georeference
    std::vector<PendingRender> pendingRenders_;
    uint64_t frameCounter_ = 0;
    static constexpr size_t kMaxTextures = 8;

    char  tokenBuf_[256] = {};       // PAT input buffer
    std::string patToken_;           // stored PAT (fallback when not logged in)
    bool  hasToken_ = false;         // an effective token (OAuth or PAT) exists

    char  clientIdBuf_[128]     = {};
    char  clientSecretBuf_[128] = {};
    char  redirectBuf_[128]     = {};
    char  scopesBuf_[256]       = {};

    // Recompute the effective token (OAuth access token wins over the PAT),
    // push it into the API client and restart failed fetches.
    void applyEffectiveToken();
    // Effective OAuth client = UI/prefs override, else the built-in app client.
    void applyOauthClientFromBuffers();

    // Drawing state (notepad toolset; coordinates are canonical chart px).
    chart::Stroke  scratchStroke_;
    bool           drawing_     = false;
    chart::NpId    drawingTab_  = chart::INVALID_NPID;
    std::string    drawingChart_;
    int            drawingPage_ = 0;
    std::unordered_set<chart::NpId> erasedThisStroke_;
    cp::notepad::Tool currentTool_ = cp::notepad::Tool::Pen;
    bool           panTool_ = false;       // Pan overrides the notepad tools
    float          currentThickness_ = 3.f;

    // Aircraft position datarefs (single shared aircraft — PhysicsSync keeps
    // every client converged, so local datarefs are the crew position).
    XPLMDataRef drLat_ = nullptr, drLon_ = nullptr, drPsi_ = nullptr;

    // Forced-size window + custom bottom-right resize grip (NotepadWindow's).
    float targetW_ = 0.f, targetH_ = 0.f;
    bool  resizing_ = false;
    float resizeStartMouseX_ = 0.f, resizeStartMouseY_ = 0.f;
    float resizeStartW_ = 0.f, resizeStartH_ = 0.f;
};

}
}
