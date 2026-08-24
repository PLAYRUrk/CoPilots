#include "HttpFetch.h"
#include "../Log.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <wininet.h>
#endif

namespace cp {
namespace net {

bool HttpGetBlocking(const std::string& url,
                     const std::vector<std::string>& headers,
                     std::string& outBody, int& outStatus, std::string& outErr)
{
#ifdef _WIN32
    HINTERNET hInet = InternetOpenA("CoPilots-plugin",
                                    INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { outErr = "InternetOpen failed"; return false; }

    std::string hdr;
    for (const auto& h : headers) { hdr += h; hdr += "\r\n"; }

    HINTERNET hUrl = InternetOpenUrlA(
        hInet, url.c_str(),
        hdr.empty() ? nullptr : hdr.c_str(),
        hdr.empty() ? 0 : static_cast<DWORD>(hdr.size()),
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hUrl) {
        outErr = "connection failed (err " + std::to_string(GetLastError()) + ")";
        InternetCloseHandle(hInet);
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(hUrl, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &len, nullptr);
    outStatus = static_cast<int>(status);

    outBody.clear();
    char buf[8192];
    DWORD read = 0;
    while (InternetReadFile(hUrl, buf, sizeof(buf), &read) && read > 0)
        outBody.append(buf, read);

    InternetCloseHandle(hUrl);
    InternetCloseHandle(hInet);

    if (status != 200) {
        outErr = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
#else
    (void)url; (void)headers; (void)outBody; (void)outStatus;
    outErr = "HTTP requests are only supported on Windows";
    return false;
#endif
}

bool HttpPostBlocking(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      std::string& outBody, int& outStatus, std::string& outErr,
                      const std::string& contentType)
{
#ifdef _WIN32
    // Split "https://host/path" (HTTPS only — all our endpoints are TLS).
    std::string rest;
    if (url.rfind("https://", 0) == 0) rest = url.substr(8);
    else { outErr = "POST requires an https:// URL"; return false; }
    size_t slash = rest.find('/');
    std::string host = (slash == std::string::npos) ? rest : rest.substr(0, slash);
    std::string path = (slash == std::string::npos) ? "/"  : rest.substr(slash);

    HINTERNET hInet = InternetOpenA("CoPilots-plugin",
                                    INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!hInet) { outErr = "InternetOpen failed"; return false; }

    HINTERNET hConn = InternetConnectA(hInet, host.c_str(),
                                       INTERNET_DEFAULT_HTTPS_PORT,
                                       nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
    if (!hConn) {
        outErr = "connect failed (err " + std::to_string(GetLastError()) + ")";
        InternetCloseHandle(hInet);
        return false;
    }

    HINTERNET hReq = HttpOpenRequestA(
        hConn, "POST", path.c_str(), nullptr, nullptr, nullptr,
        INTERNET_FLAG_SECURE | INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE, 0);
    if (!hReq) {
        outErr = "request failed (err " + std::to_string(GetLastError()) + ")";
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);
        return false;
    }

    std::string hdr = "Content-Type: " + contentType + "\r\n";
    for (const auto& h : headers) { hdr += h; hdr += "\r\n"; }

    BOOL sent = HttpSendRequestA(hReq, hdr.c_str(), static_cast<DWORD>(hdr.size()),
                                 const_cast<char*>(body.data()),
                                 static_cast<DWORD>(body.size()));
    if (!sent) {
        outErr = "send failed (err " + std::to_string(GetLastError()) + ")";
        InternetCloseHandle(hReq);
        InternetCloseHandle(hConn);
        InternetCloseHandle(hInet);
        return false;
    }

    DWORD status = 0, len = sizeof(status);
    HttpQueryInfoA(hReq, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER,
                   &status, &len, nullptr);
    outStatus = static_cast<int>(status);

    outBody.clear();
    char buf[8192];
    DWORD read = 0;
    while (InternetReadFile(hReq, buf, sizeof(buf), &read) && read > 0)
        outBody.append(buf, read);

    InternetCloseHandle(hReq);
    InternetCloseHandle(hConn);
    InternetCloseHandle(hInet);

    if (status != 200) {
        outErr = "HTTP " + std::to_string(status);
        return false;
    }
    return true;
#else
    (void)url; (void)headers; (void)body; (void)outBody; (void)outStatus;
    outErr = "HTTP requests are only supported on Windows";
    return false;
#endif
}

AsyncHttp::~AsyncHttp()
{
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void AsyncHttp::ensureWorker()
{
    if (started_) return;
    started_ = true;
    worker_ = std::thread([this]() { workerLoop(); });
}

AsyncHttp::Handle AsyncHttp::get(const std::string& url,
                                 const std::vector<std::string>& headers)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureWorker();
    Handle h = nextHandle_++;
    jobs_.push_back({h, url, headers, ""});
    cv_.notify_one();
    return h;
}

AsyncHttp::Handle AsyncHttp::getToFile(const std::string& url,
                                       const std::vector<std::string>& headers,
                                       const std::string& destPath)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureWorker();
    Handle h = nextHandle_++;
    jobs_.push_back({h, url, headers, destPath});
    cv_.notify_one();
    return h;
}

AsyncHttp::Handle AsyncHttp::postForm(const std::string& url,
                                      const std::vector<std::string>& headers,
                                      const std::string& body)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureWorker();
    Handle h = nextHandle_++;
    Job job{h, url, headers, "", true, body, ""};
    jobs_.push_back(std::move(job));
    cv_.notify_one();
    return h;
}

AsyncHttp::Handle AsyncHttp::postJson(const std::string& url,
                                      const std::vector<std::string>& headers,
                                      const std::string& body)
{
    std::lock_guard<std::mutex> lk(mu_);
    ensureWorker();
    Handle h = nextHandle_++;
    Job job{h, url, headers, "", true, body, "application/json"};
    jobs_.push_back(std::move(job));
    cv_.notify_one();
    return h;
}

bool AsyncHttp::poll(Handle h, HttpResult& out)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = results_.find(h);
    if (it == results_.end()) return false;
    out = std::move(it->second);
    results_.erase(it);
    return true;
}

void AsyncHttp::workerLoop()
{
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this]() { return quit_ || !jobs_.empty(); });
            if (quit_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }

        HttpResult res;
        std::string body, err;
        int status = 0;
        bool ok = job.isPost
                ? HttpPostBlocking(job.url, job.headers, job.postBody, body, status, err,
                                   job.contentType.empty()
                                       ? "application/x-www-form-urlencoded"
                                       : job.contentType)
                : HttpGetBlocking(job.url, job.headers, body, status, err);
        res.ok     = ok;
        res.status = status;
        res.error  = err;

        if (ok && !job.destPath.empty()) {
            std::error_code ec;
            std::filesystem::path p(job.destPath);
            std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream f(job.destPath, std::ios::binary | std::ios::trunc);
            if (f.is_open()) {
                f.write(body.data(), static_cast<std::streamsize>(body.size()));
                f.close();
                res.filePath = job.destPath;
            } else {
                res.ok    = false;
                res.error = "cannot write " + job.destPath;
            }
        } else if (job.destPath.empty()) {
            // Keep the body on failure too — API error responses (e.g. 422)
            // carry a JSON message the caller turns into readable text.
            res.body = std::move(body);
        } else {
            std::error_code ec;
            std::filesystem::remove(job.destPath, ec);  // drop any partial file
            // A failed download still has a body worth reading (an API error
            // document, or an S3 <Error> page after a redirect) — without it a
            // rejected chart file is just "HTTP 403" with no cause.
            res.body = std::move(body);
        }

        {
            std::lock_guard<std::mutex> lk(mu_);
            results_[job.handle] = std::move(res);
        }
    }
}

}
}
