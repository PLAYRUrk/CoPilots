#include "ChartRenderer.h"
#include "../Log.h"

#include <cstring>
#include <fstream>

#ifdef _WIN32
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
#endif

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#include <stb/stb_image.h>

namespace cp {
namespace chart {

// ── PDFium C ABI (self-declared) ─────────────────────────────────────────────
// The public FPDF_* C API is ABI-stable; declaring the handful of functions we
// need avoids a build-time dependency on the pdfium headers entirely — the
// only artefact required is pdfium.dll next to win.xpl at runtime.

typedef void* FPDF_DOCUMENT;
typedef void* FPDF_PAGE;
typedef void* FPDF_BITMAP;

static constexpr int FPDFBitmap_BGRA = 4;
static constexpr int FPDF_ANNOT_FLAG = 0x01;   // render annotations too

typedef void          (*PFN_InitLibrary)();
typedef void          (*PFN_DestroyLibrary)();
typedef FPDF_DOCUMENT (*PFN_LoadDocument)(const char* path, const char* password);
typedef void          (*PFN_CloseDocument)(FPDF_DOCUMENT);
typedef int           (*PFN_GetPageCount)(FPDF_DOCUMENT);
typedef FPDF_PAGE     (*PFN_LoadPage)(FPDF_DOCUMENT, int index);
typedef void          (*PFN_ClosePage)(FPDF_PAGE);
typedef double        (*PFN_GetPageWidth)(FPDF_PAGE);
typedef double        (*PFN_GetPageHeight)(FPDF_PAGE);
typedef FPDF_BITMAP   (*PFN_BitmapCreateEx)(int w, int h, int format,
                                            void* firstScan, int stride);
typedef void          (*PFN_BitmapFillRect)(FPDF_BITMAP, int left, int top,
                                            int w, int h, unsigned long color);
typedef void          (*PFN_RenderPageBitmap)(FPDF_BITMAP, FPDF_PAGE,
                                              int startX, int startY,
                                              int sizeX, int sizeY,
                                              int rotate, int flags);
typedef void*         (*PFN_BitmapGetBuffer)(FPDF_BITMAP);
typedef int           (*PFN_BitmapGetStride)(FPDF_BITMAP);
typedef void          (*PFN_BitmapDestroy)(FPDF_BITMAP);

static struct {
    PFN_InitLibrary      InitLibrary      = nullptr;
    PFN_DestroyLibrary   DestroyLibrary   = nullptr;
    PFN_LoadDocument     LoadDocument     = nullptr;
    PFN_CloseDocument    CloseDocument    = nullptr;
    PFN_GetPageCount     GetPageCount     = nullptr;
    PFN_LoadPage         LoadPage         = nullptr;
    PFN_ClosePage        ClosePage        = nullptr;
    PFN_GetPageWidth     GetPageWidth     = nullptr;
    PFN_GetPageHeight    GetPageHeight    = nullptr;
    PFN_BitmapCreateEx   BitmapCreateEx   = nullptr;
    PFN_BitmapFillRect   BitmapFillRect   = nullptr;
    PFN_RenderPageBitmap RenderPageBitmap = nullptr;
    PFN_BitmapGetBuffer  BitmapGetBuffer  = nullptr;
    PFN_BitmapGetStride  BitmapGetStride  = nullptr;
    PFN_BitmapDestroy    BitmapDestroy    = nullptr;
} g_pdf;

bool ChartRenderer::loadPdfium(const std::string& pluginDir)
{
#ifdef _WIN32
    std::string dllPath = pluginDir + "\\pdfium.dll";
    HMODULE h = LoadLibraryExA(dllPath.c_str(), nullptr,
                               LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) {
        LogWarning("ChartRenderer: %s not found (err %lu) — PDF charts disabled",
                   dllPath.c_str(), GetLastError());
        return false;
    }
    auto gp = [h](const char* name) { return GetProcAddress(h, name); };
    g_pdf.InitLibrary      = (PFN_InitLibrary)     gp("FPDF_InitLibrary");
    g_pdf.DestroyLibrary   = (PFN_DestroyLibrary)  gp("FPDF_DestroyLibrary");
    g_pdf.LoadDocument     = (PFN_LoadDocument)    gp("FPDF_LoadDocument");
    g_pdf.CloseDocument    = (PFN_CloseDocument)   gp("FPDF_CloseDocument");
    g_pdf.GetPageCount     = (PFN_GetPageCount)    gp("FPDF_GetPageCount");
    g_pdf.LoadPage         = (PFN_LoadPage)        gp("FPDF_LoadPage");
    g_pdf.ClosePage        = (PFN_ClosePage)       gp("FPDF_ClosePage");
    g_pdf.GetPageWidth     = (PFN_GetPageWidth)    gp("FPDF_GetPageWidth");
    g_pdf.GetPageHeight    = (PFN_GetPageHeight)   gp("FPDF_GetPageHeight");
    g_pdf.BitmapCreateEx   = (PFN_BitmapCreateEx)  gp("FPDFBitmap_CreateEx");
    g_pdf.BitmapFillRect   = (PFN_BitmapFillRect)  gp("FPDFBitmap_FillRect");
    g_pdf.RenderPageBitmap = (PFN_RenderPageBitmap)gp("FPDF_RenderPageBitmap");
    g_pdf.BitmapGetBuffer  = (PFN_BitmapGetBuffer) gp("FPDFBitmap_GetBuffer");
    g_pdf.BitmapGetStride  = (PFN_BitmapGetStride) gp("FPDFBitmap_GetStride");
    g_pdf.BitmapDestroy    = (PFN_BitmapDestroy)   gp("FPDFBitmap_Destroy");

    bool complete = g_pdf.InitLibrary && g_pdf.DestroyLibrary && g_pdf.LoadDocument
                 && g_pdf.CloseDocument && g_pdf.GetPageCount && g_pdf.LoadPage
                 && g_pdf.ClosePage && g_pdf.GetPageWidth && g_pdf.GetPageHeight
                 && g_pdf.BitmapCreateEx && g_pdf.BitmapFillRect
                 && g_pdf.RenderPageBitmap && g_pdf.BitmapGetBuffer
                 && g_pdf.BitmapGetStride && g_pdf.BitmapDestroy;
    if (!complete) {
        LogWarning("ChartRenderer: pdfium.dll is missing expected exports — "
                   "PDF charts disabled");
        FreeLibrary(h);
        return false;
    }
    pdfiumDll_ = h;
    Log("ChartRenderer: pdfium loaded (%s)", dllPath.c_str());
    return true;
#else
    (void)pluginDir;
    return false;
#endif
}

void ChartRenderer::init(const std::string& pluginDir)
{
    if (started_) return;
    pdfAvailable_ = loadPdfium(pluginDir);
    started_ = true;
    worker_  = std::thread([this]() { workerLoop(); });
}

void ChartRenderer::shutdown()
{
    if (!started_) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        quit_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
    started_ = false;
#ifdef _WIN32
    if (pdfiumDll_) { FreeLibrary((HMODULE)pdfiumDll_); pdfiumDll_ = nullptr; }
#endif
    pdfAvailable_ = false;
}

ChartRenderer::Handle ChartRenderer::request(const RenderRequest& req)
{
    std::lock_guard<std::mutex> lk(mu_);
    Handle h = nextHandle_++;
    jobs_.push_back({h, req});
    cv_.notify_one();
    return h;
}

bool ChartRenderer::poll(Handle h, RenderResult& out)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = results_.find(h);
    if (it == results_.end()) return false;
    out = std::move(it->second);
    results_.erase(it);
    return true;
}

void ChartRenderer::workerLoop()
{
    // PDFium lifecycle is confined to this thread (it is not thread-safe).
    if (pdfAvailable_) g_pdf.InitLibrary();

    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lk(mu_);
            cv_.wait(lk, [this]() { return quit_ || !jobs_.empty(); });
            if (quit_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
        }
        RenderResult res;
        res.chartId = job.req.chartId;
        res.page    = job.req.page;
        renderJob(job.req, res);
        {
            std::lock_guard<std::mutex> lk(mu_);
            results_[job.handle] = std::move(res);
        }
    }

    if (pdfAvailable_) g_pdf.DestroyLibrary();
}

void ChartRenderer::renderJob(const RenderRequest& req, RenderResult& out)
{
    std::ifstream f(req.filePath, std::ios::binary);
    if (!f.is_open()) { out.error = "chart file missing: " + req.filePath; return; }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)),
                               std::istreambuf_iterator<char>());
    if (bytes.size() < 8) { out.error = "chart file empty"; return; }

    // Sniff the format from magic bytes (the cache stores a neutral extension).
    if (std::memcmp(bytes.data(), "%PDF", 4) == 0) {
        if (!pdfAvailable_) {
            out.error = "pdfium.dll not found — reinstall the plugin to view PDF charts";
            return;
        }
        renderPdf(req, out);
    } else {
        renderImage(bytes, out);
    }
}

void ChartRenderer::renderPdf(const RenderRequest& req, RenderResult& out)
{
    FPDF_DOCUMENT doc = g_pdf.LoadDocument(req.filePath.c_str(), nullptr);
    if (!doc) { out.error = "PDF failed to open (corrupt or protected)"; return; }

    out.pageCount = g_pdf.GetPageCount(doc);
    int pageIdx = req.page;
    if (pageIdx < 0 || pageIdx >= out.pageCount) pageIdx = 0;
    out.page = pageIdx;

    FPDF_PAGE page = g_pdf.LoadPage(doc, pageIdx);
    if (!page) { g_pdf.CloseDocument(doc); out.error = "PDF page failed to load"; return; }

    out.pageWpt = g_pdf.GetPageWidth(page);
    out.pageHpt = g_pdf.GetPageHeight(page);
    out.canonicalScale = canonicalScaleFor(out.pageWpt, out.pageHpt);
    int w = static_cast<int>(out.pageWpt * out.canonicalScale + 0.5);
    int h = static_cast<int>(out.pageHpt * out.canonicalScale + 0.5);
    if (w < 1) w = 1;
    if (h < 1) h = 1;

    std::vector<uint8_t> bgra(static_cast<size_t>(w) * h * 4);
    FPDF_BITMAP bmp = g_pdf.BitmapCreateEx(w, h, FPDFBitmap_BGRA,
                                           bgra.data(), w * 4);
    if (!bmp) {
        g_pdf.ClosePage(page); g_pdf.CloseDocument(doc);
        out.error = "PDF bitmap allocation failed";
        return;
    }
    g_pdf.BitmapFillRect(bmp, 0, 0, w, h, 0xFFFFFFFFul);  // white background
    g_pdf.RenderPageBitmap(bmp, page, 0, 0, w, h, 0 /*no extra rotation*/,
                           FPDF_ANNOT_FLAG);
    g_pdf.BitmapDestroy(bmp);
    g_pdf.ClosePage(page);
    g_pdf.CloseDocument(doc);

    // BGRA → RGBA for a plain GL_RGBA upload (Windows gl.h is GL 1.1).
    for (size_t i = 0; i < bgra.size(); i += 4)
        std::swap(bgra[i], bgra[i + 2]);

    out.w = w; out.h = h;
    out.rgba = std::move(bgra);
}

void ChartRenderer::renderImage(const std::vector<uint8_t>& bytes, RenderResult& out)
{
    int w = 0, h = 0, comp = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(),
                                              static_cast<int>(bytes.size()),
                                              &w, &h, &comp, 4);
    if (!px) {
        out.error = std::string("image decode failed: ") + stbi_failure_reason();
        return;
    }
    out.pageCount = 1;
    out.page      = 0;
    out.w = w; out.h = h;
    // Canonical space for images is the native pixel grid; georefs address the
    // "page" in points, so pretend 1 pt = 1 px (canonicalScale 1).
    out.pageWpt = w; out.pageHpt = h;
    out.canonicalScale = 1.0;
    out.rgba.assign(px, px + static_cast<size_t>(w) * h * 4);
    stbi_image_free(px);
}

}
}
