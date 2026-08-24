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
namespace chart {

// Deterministic canonical scale for a PDF page: canonical px per PDF point.
// 2.0 (= 144 DPI) unless the page would exceed kMaxTexDim, in which case the
// scale shrinks to fit.  Every client computes the same value from the same
// page size, so canonical stroke coordinates line up across the crew.
constexpr double kCanonicalDpiScale = 2.0;
constexpr int    kMaxTexDim         = 4096;

inline double canonicalScaleFor(double pageWpt, double pageHpt)
{
    double s = kCanonicalDpiScale;
    double m = (pageWpt > pageHpt) ? pageWpt : pageHpt;
    if (m > 0 && m * s > kMaxTexDim) s = kMaxTexDim / m;
    return s;
}

struct RenderRequest {
    std::string chartId;
    std::string filePath;   // downloaded chart file (PDF or image, sniffed)
    int         page = 0;   // 0-based
};

struct RenderResult {
    std::string chartId;
    int         page      = 0;
    int         pageCount = 1;
    int         w = 0, h = 0;          // canonical pixel size
    double      pageWpt = 0, pageHpt = 0;   // PDF points (= w/h for images)
    double      canonicalScale = 1.0;  // canonical px per PDF point
    std::vector<uint8_t> rgba;         // w*h*4, ready for glTexImage2D
    std::string error;                 // non-empty on failure
};

// Rasterises chart files on a dedicated worker thread.
//
// PDFs: PDFium, loaded at runtime via LoadLibrary("pdfium.dll") from the
// plugin's own folder — never linked, so the .xpl loads fine without the DLL
// (available() gates the UI).  PDFium is not thread-safe: every FPDF_* call,
// including init/destroy, happens on the single worker thread.
// Images (PNG/JPEG): stb_image on the same worker.
// GL texture creation stays with the caller (main thread).
class ChartRenderer {
public:
    using Handle = uint32_t;
    static constexpr Handle INVALID = 0;

    // pluginDir: directory containing win.xpl (and pdfium.dll next to it).
    void init(const std::string& pluginDir);
    void shutdown();
    ~ChartRenderer() { shutdown(); }

    // False when pdfium.dll could not be loaded — PDF charts unavailable
    // (image charts still render).
    bool pdfAvailable() const { return pdfAvailable_; }

    Handle request(const RenderRequest& req);
    bool   poll(Handle h, RenderResult& out);

private:
    struct Job { Handle handle; RenderRequest req; };

    void workerLoop();
    void renderJob(const RenderRequest& req, RenderResult& out);
    void renderPdf(const RenderRequest& req, RenderResult& out);
    void renderImage(const std::vector<uint8_t>& bytes, RenderResult& out);
    bool loadPdfium(const std::string& pluginDir);

    std::thread             worker_;
    std::mutex              mu_;
    std::condition_variable cv_;
    std::deque<Job>         jobs_;
    std::map<Handle, RenderResult> results_;
    Handle                  nextHandle_ = 1;
    bool                    quit_    = false;
    bool                    started_ = false;
    bool                    pdfAvailable_ = false;
    void*                   pdfiumDll_    = nullptr;  // HMODULE
};

}
}
