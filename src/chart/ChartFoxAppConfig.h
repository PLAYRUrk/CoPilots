#pragma once

// ---------------------------------------------------------------------------
// Built-in ChartFox OAuth client of the CoPilots application.
//
// The Client ID is NOT a secret — it merely identifies the app (the client is
// a Public one, the login is protected by PKCE), so it ships baked into the
// plugin and crew members never have to enter anything: they just press
// "Login with ChartFox" and approve in the browser.
//
// Fill kChartFoxClientId with the Client ID from chartfox.org/developers
// (OAuth client created as Public).  kChartFoxRedirectUri must be registered
// for that client verbatim.
// ---------------------------------------------------------------------------

namespace cp {
namespace chart {

constexpr const char* kChartFoxClientId    = "019f93f2-92e6-70a9-925a-ed942fd4c418";
constexpr const char* kChartFoxRedirectUri = "http://localhost:58525/callback";

// Space-delimited OAuth scopes requested at login.  Requesting a scope the
// application was NOT granted makes the authorize endpoint reject the whole
// login, so this must stay within the granted set (visible in the developer
// portal).  Granted to the CoPilots organisation: airports:view, charts:index,
// charts:view, charts:view_source_url, charts:files, charts:geos,
// oauth:user:name, oauth:telemetry:view.
//
// Only what the plugin actually consumes is requested:
//   airports:view          /v2/airports/{icao}/charts/grouped
//   charts:index           the grouped chart list itself
//   charts:view            /v2/charts/{id} (names, copyright, metadata)
//   charts:files           files[] — the hosted PDF/IMG the plugin renders
//   charts:view_source_url source_url — fallback when no hosted file exists
//   charts:geos            georefs[] — aircraft position overlay on the chart
constexpr const char* kChartFoxScopes =
    "airports:view charts:index charts:view charts:files "
    "charts:view_source_url charts:geos";

// Scope set requested by builds up to 2026-07 (before charts:geos and
// charts:view_source_url were granted).  A copy of it persisted in
// copilots_prefs.json is a stale default, not a deliberate override, and is
// ignored on load so the built-in set above wins.
constexpr const char* kChartFoxLegacyScopes =
    "airports:view charts:index charts:view charts:files";

}
}
