#include "Prefs.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <fstream>

namespace cp {

static std::string prefsPath(const std::string& xpSystemPath)
{
    return xpSystemPath + "Output/preferences/copilots_prefs.json";
}

bool LoadPrefs(const std::string& xpSystemPath, Prefs& out)
{
    std::ifstream f(prefsPath(xpSystemPath));
    if (!f.is_open()) return false;

    nlohmann::json j;
    try {
        f >> j;
    } catch (const std::exception& e) {
        LogWarning("Prefs: failed to parse %s: %s",
                   prefsPath(xpSystemPath).c_str(), e.what());
        return false;
    }

    out.nick     = j.value("nick",     out.nick);
    out.address  = j.value("address",  out.address);
    out.password = j.value("password", out.password);
    out.bindIp   = j.value("bindIp",   out.bindIp);
    out.hostPort = static_cast<uint16_t>(j.value("hostPort", (int)out.hostPort));
    out.requireJoinApproval    = j.value("requireJoinApproval",    out.requireJoinApproval);
    out.requireControlApproval = j.value("requireControlApproval", out.requireControlApproval);
    out.lastWasHost            = j.value("lastWasHost",            out.lastWasHost);
    out.pointerEnabled         = j.value("pointerEnabled",         out.pointerEnabled);
    out.chartfoxToken          = j.value("chartfoxToken",          out.chartfoxToken);
    out.chartfoxClientId       = j.value("chartfoxClientId",       out.chartfoxClientId);
    out.chartfoxClientSecret   = j.value("chartfoxClientSecret",   out.chartfoxClientSecret);
    out.chartfoxRedirectUri    = j.value("chartfoxRedirectUri",    out.chartfoxRedirectUri);
    out.chartfoxScopes         = j.value("chartfoxScopes",         out.chartfoxScopes);
    out.chartfoxTokenScopes    = j.value("chartfoxTokenScopes",    out.chartfoxTokenScopes);
    out.calSubmitUrl           = j.value("calSubmitUrl",           out.calSubmitUrl);
    out.chartfoxAccessToken    = j.value("chartfoxAccessToken",    out.chartfoxAccessToken);
    out.chartfoxRefreshToken   = j.value("chartfoxRefreshToken",   out.chartfoxRefreshToken);
    out.chartfoxTokenExpiry    = j.value("chartfoxTokenExpiry",    out.chartfoxTokenExpiry);
    Log("Prefs: loaded (nick='%s', address='%s')", out.nick.c_str(), out.address.c_str());
    return true;
}

bool SavePrefs(const std::string& xpSystemPath, const Prefs& p)
{
    nlohmann::json j;
    j["nick"]     = p.nick;
    j["address"]  = p.address;
    j["password"] = p.password;
    j["bindIp"]   = p.bindIp;
    j["hostPort"] = (int)p.hostPort;
    j["requireJoinApproval"]    = p.requireJoinApproval;
    j["requireControlApproval"] = p.requireControlApproval;
    j["lastWasHost"]            = p.lastWasHost;
    j["pointerEnabled"]         = p.pointerEnabled;
    j["chartfoxToken"]          = p.chartfoxToken;
    j["chartfoxClientId"]       = p.chartfoxClientId;
    j["chartfoxClientSecret"]   = p.chartfoxClientSecret;
    j["chartfoxRedirectUri"]    = p.chartfoxRedirectUri;
    j["chartfoxScopes"]         = p.chartfoxScopes;
    j["chartfoxTokenScopes"]    = p.chartfoxTokenScopes;
    j["calSubmitUrl"]           = p.calSubmitUrl;
    j["chartfoxAccessToken"]    = p.chartfoxAccessToken;
    j["chartfoxRefreshToken"]   = p.chartfoxRefreshToken;
    j["chartfoxTokenExpiry"]    = p.chartfoxTokenExpiry;

    std::ofstream f(prefsPath(xpSystemPath));
    if (!f.is_open()) {
        LogWarning("Prefs: cannot write %s", prefsPath(xpSystemPath).c_str());
        return false;
    }
    f << j.dump(2) << "\n";
    return true;
}

}
