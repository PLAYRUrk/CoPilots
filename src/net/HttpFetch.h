#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace cp {
namespace net {

// Result of one finished HTTP request.  `ok` means transport succeeded AND the
// server answered 200; otherwise `error` holds a user-readable reason and
// `status` the HTTP code (0 for transport-level failures).
struct HttpResult {
    bool        ok     = false;
    int         status = 0;
    std::string error;
    std::string body;      // filled by get(); empty for getToFile()
    std::string filePath;  // filled by getToFile()
};

// Non-blocking HTTPS GET on a single worker thread (WinINet, same stack as
// ConfigDownloader).  The main thread submits jobs and polls for results each
// flight loop; the sim never blocks on the network.
//
// Windows-only like ConfigDownloader: on other platforms every request
// completes immediately with an error.
class AsyncHttp {
public:
    using Handle = uint32_t;                 // 0 = invalid
    static constexpr Handle INVALID = 0;

    ~AsyncHttp();

    // headers: full lines without CRLF, e.g. "Authorization: Bearer xyz".
    Handle get(const std::string& url, const std::vector<std::string>& headers);

    // Streams the response body to destPath (parent directories are created).
    // On failure a partial file is removed.
    Handle getToFile(const std::string& url, const std::vector<std::string>& headers,
                     const std::string& destPath);

    // POST with an application/x-www-form-urlencoded body (OAuth token exchange).
    Handle postForm(const std::string& url, const std::vector<std::string>& headers,
                    const std::string& body);

    // POST with an application/json body (community calibration upload).
    Handle postJson(const std::string& url, const std::vector<std::string>& headers,
                    const std::string& body);

    // Returns true once the request finished; the result is handed out exactly
    // once and the handle becomes invalid afterwards.
    bool poll(Handle h, HttpResult& out);

private:
    struct Job {
        Handle                   handle = INVALID;
        std::string              url;
        std::vector<std::string> headers;
        std::string              destPath;  // empty = in-memory body
        bool                     isPost = false;
        std::string              postBody;
        std::string              contentType;  // empty = form-urlencoded
    };

    void ensureWorker();
    void workerLoop();

    std::thread             worker_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<Job>         jobs_;
    std::map<Handle, HttpResult> results_;
    Handle                  nextHandle_ = 1;
    bool                    quit_       = false;
    bool                    started_    = false;
};

// Blocking HTTPS GET (worker threads only).  Generalisation of the
// ConfigDownloader fetch with optional extra request headers.
bool HttpGetBlocking(const std::string& url,
                     const std::vector<std::string>& headers,
                     std::string& outBody, int& outStatus, std::string& outErr);

// Blocking HTTPS POST (worker threads only), form-urlencoded body.
bool HttpPostBlocking(const std::string& url,
                      const std::vector<std::string>& headers,
                      const std::string& body,
                      std::string& outBody, int& outStatus, std::string& outErr,
                      const std::string& contentType = "application/x-www-form-urlencoded");

}
}
