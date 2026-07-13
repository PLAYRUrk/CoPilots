#include "ConfigDownloader.h"
#include "../Log.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wininet.h>
#endif

using json = nlohmann::json;

namespace cp {
namespace net {

// Raw-content base of the project repository (branch: main).
static const char* RAW_BASE =
    "https://raw.githubusercontent.com/PLAYRUrk/CoPilots/main/configs/";

// ── HTTPS GET (blocking; worker thread only) ────────────────────────────────
static bool HttpGet(const std::string& url, std::string& outBody, std::string& outErr)
{
#ifdef _WIN32
    HINTERNET hInet = InternetOpenA("CoPilots-plugin",
                                    INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { outErr = "InternetOpen failed"; return false; }

    HINTERNET hUrl = InternetOpenUrlA(
        hInet, url.c_str(), nullptr, 0,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        outErr = "connection failed (err " + std::to_string(GetLastError()) + ")";
        InternetCloseHandle(hInet);
        return false;
    }

    // Check the HTTP status code — raw.githubusercontent returns 404 as a body too.
    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &len, nullptr);
    if (status != 200) {
        outErr = "HTTP " + std::to_string(status);
        InternetCloseHandle(hUrl);
        InternetCloseHandle(hInet);
        return false;
    }

    outBody.clear();
    char buf[8192];
    DWORD read = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
        outBody.append(buf, read);

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);
    return true;
#else
    (void)url; (void)outBody;
    outErr = "config download is only supported on Windows";
    return false;
#endif
}

static std::string toLower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return s;
}

ConfigDownloader::~ConfigDownloader()
{
    if (thread_.joinable()) thread_.join();
}

bool ConfigDownloader::start(const std::string& aircraftDir, const std::string& acfName)
{
    return launch(aircraftDir, acfName, "", "");
}

bool ConfigDownloader::startFolder(const std::string& aircraftDir, const std::string& folder,
                                   const std::string& aircraftName)
{
    return launch(aircraftDir, "", folder, aircraftName);
}

bool ConfigDownloader::launch(const std::string& aircraftDir, const std::string& acfName,
                              const std::string& forcedFolder, const std::string& forcedName)
{
    State expected = State::IDLE;
    if (!state_.compare_exchange_strong(expected, State::RUNNING)) {
        // Also allow restart from a finished state.
        if (expected != State::SUCCESS && expected != State::FAILED) return false;
        state_ = State::RUNNING;
    }
    if (thread_.joinable()) thread_.join();
    setMessage("Downloading...");
    thread_ = std::thread([this, aircraftDir, acfName, forcedFolder, forcedName]() {
        run(aircraftDir, acfName, forcedFolder, forcedName);
    });
    return true;
}

std::vector<ConfigDownloader::Entry> ConfigDownloader::entries()
{
    std::lock_guard<std::mutex> lk(mu_);
    return entries_;
}

std::string ConfigDownloader::message()
{
    std::lock_guard<std::mutex> lk(mu_);
    return msg_;
}

void ConfigDownloader::acknowledge()
{
    State s = state_.load();
    if (s == State::SUCCESS || s == State::FAILED) state_ = State::IDLE;
}

void ConfigDownloader::setMessage(const std::string& m)
{
    std::lock_guard<std::mutex> lk(mu_);
    msg_ = m;
}

void ConfigDownloader::run(std::string aircraftDir, std::string acfName,
                           std::string forcedFolder, std::string forcedName)
{
    std::string err;

    // 1. Fetch the manifest.
    std::string body;
    if (!HttpGet(std::string(RAW_BASE) + "manifest.json", body, err)) {
        setMessage("Config library unreachable (" + err + ").");
        state_ = State::FAILED;
        return;
    }

    // 2. Parse the manifest; publish the library list for the manual picker,
    //    then match the aircraft (unless the user forced a specific entry).
    std::string folderLeaf = aircraftDir;
    {
        size_t sep = folderLeaf.find_last_of("/\\");
        if (sep != std::string::npos) folderLeaf = folderLeaf.substr(sep + 1);
    }
    std::string haystack = toLower(folderLeaf) + "|" + toLower(acfName);

    std::string cfgFolder = forcedFolder, cfgAircraft = forcedName;
    try {
        json m = json::parse(body);
        std::vector<Entry> list;
        for (const auto& e : m.value("configs", json::array())) {
            Entry en;
            en.folder   = e.value("folder", "");
            en.aircraft = e.value("aircraft", en.folder);
            if (!en.folder.empty()) list.push_back(std::move(en));

            if (cfgFolder.empty()) {
                for (const auto& key : e.value("match", json::array())) {
                    if (haystack.find(toLower(key.get<std::string>())) != std::string::npos) {
                        cfgFolder   = e.value("folder", "");
                        cfgAircraft = e.value("aircraft", "");
                        break;
                    }
                }
            }
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            entries_ = std::move(list);
        }
    } catch (const std::exception& ex) {
        setMessage(std::string("Bad manifest: ") + ex.what());
        state_ = State::FAILED;
        return;
    }

    if (cfgFolder.empty()) {
        setMessage("No config in the library matches '" + folderLeaf +
                   "'. Pick one manually below.");
        state_ = State::FAILED;
        return;
    }

    // 3. Fetch the config and make sure it is valid JSON.
    std::string cfgBody;
    if (!HttpGet(std::string(RAW_BASE) + cfgFolder + "/copilots.json", cfgBody, err)) {
        setMessage("Download failed: " + err);
        state_ = State::FAILED;
        return;
    }
    try {
        (void)json::parse(cfgBody);
    } catch (const std::exception& ex) {
        setMessage(std::string("Downloaded config is not valid JSON: ") + ex.what());
        state_ = State::FAILED;
        return;
    }

    // 4. Back up any existing copilots.json, then install the new one.
    std::string target = aircraftDir + "/copilots.json";
    {
        std::ifstream existing(target, std::ios::binary);
        if (existing.good()) {
            std::ofstream bak(target + ".bak", std::ios::binary | std::ios::trunc);
            bak << existing.rdbuf();
        }
    }
    std::ofstream out(target, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        setMessage("Cannot write " + target);
        state_ = State::FAILED;
        return;
    }
    out << cfgBody;
    out.close();

    Log("ConfigDownloader: installed '%s' config into %s",
        cfgAircraft.c_str(), aircraftDir.c_str());
    setMessage("Installed: " + cfgAircraft + ".");
    state_ = State::SUCCESS;
}

}
}
