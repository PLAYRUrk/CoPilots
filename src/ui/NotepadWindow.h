#pragma once
#include "XPImguiWindow.h"
#include "Notepad.h"
#include "../session/Session.h"
#include <functional>
#include <vector>
#include <string>

namespace cp {
namespace ui {

// ---------------------------------------------------------------------------
// NotepadWindow — a tabbed drawing board with private and shared sheets.
//
// Private tabs are local-only (never sent over the network).
// When a tab is shared it becomes visible to the whole crew; any member can
// draw on shared sheets; only the sheet owner can delete / resize a sheet.
//
// The window communicates with the network via sendFn_ (push a fully framed
// TCP message) so it has no dependency on NetThread or Protocol.
// ---------------------------------------------------------------------------
class NotepadWindow : public XPImguiWindow {
public:
    bool init();
    void shutdown() { xpwShutdown(); }

    void setVisible(bool v) { xpwSetVisible(v); }
    bool visible()    const { return xpwVisible(); }

    // Set references (call before first draw).
    void setSession(const Session* s) { sess_ = s; }

    // Plugin calls this to push a framed TCP message to the network.
    // Signature matches OutboundMsg::frame (complete header+payload vector).
    std::function<void(std::vector<uint8_t>)> sendFn;

    // ---------------------------------------------------------------------------
    // Inbound notepad events — called from the flight loop (main thread) when the
    // matching TCP message arrives.  All apply idempotently via NpId de-dup.
    // ---------------------------------------------------------------------------
    void onTabShare (cp::notepad::NpId tabId, const std::string& name);
    void onSheetNew (cp::notepad::NpId tabId, cp::notepad::NpId sheetId, float w, float h);
    void onStrokeAdd(cp::notepad::NpId tabId, cp::notepad::NpId sheetId,
                     const cp::notepad::Stroke& stroke);
    void onSheetDel (cp::notepad::NpId tabId, cp::notepad::NpId sheetId);
    void onSheetParam(cp::notepad::NpId tabId, cp::notepad::NpId sheetId, float w, float h);

    // Snapshot receive: called per-chunk (isFirstChunk=true resets the sheet).
    void onSnapSheet(cp::notepad::NpId tabId, const std::string& tabName,
                     cp::notepad::NpId sheetId, float w, float h,
                     bool isFirstChunk,
                     const std::vector<cp::notepad::Stroke>& strokes);

    // Called when the host snapshot is complete.
    void onSnapEnd() { /* no-op — state already applied incrementally */ }

    // Reset all shared-tab state on disconnect (private tabs are kept).
    void resetShared();

    // Access the local notepad model (read-only; used by plugin for host snapshot).
    const cp::notepad::Notepad& notepad() const { return notepad_; }

protected:
    void renderContent() override;

private:
    const Session*        sess_    = nullptr;
    cp::notepad::Notepad  notepad_;

    // Per-participant monotonic counter (mints NpIds for tabs/sheets/strokes).
    uint32_t npCounter_ = 0;

    // In-progress stroke being drawn (local, not yet sent).
    cp::notepad::Stroke   scratchStroke_;
    bool                  drawing_      = false;
    // The sheet on which drawing_ is active (needed so we don't switch mid-stroke).
    cp::notepad::NpId     drawingTab_   = cp::notepad::INVALID_NPID;
    cp::notepad::NpId     drawingSheet_ = cp::notepad::INVALID_NPID;

    // Current tool settings
    cp::notepad::Tool currentTool_      = cp::notepad::Tool::Pen;
    float             currentThickness_ = 2.f;

    // Tab / sheet rename buffer
    char tabNameBuf_[64]   = {};
    char sheetNameBuf_[32] = {};

    // ── Private helpers ──────────────────────────────────────────────────────
    cp::notepad::NpId mintId();  // makeNpId(myId(), npCounter_++)

    // Serialize and send individual notepad messages via sendFn.
    void netTabShare (const cp::notepad::Tab& tab);
    void netSheetNew (cp::notepad::NpId tabId, const cp::notepad::Sheet& sheet);
    void netStrokeAdd(cp::notepad::NpId tabId, cp::notepad::NpId sheetId,
                      const cp::notepad::Stroke& stroke);
    void netSheetDel (cp::notepad::NpId tabId, cp::notepad::NpId sheetId);
    void netSheetParam(cp::notepad::NpId tabId, const cp::notepad::Sheet& sheet);

    // Render one canvas. Returns true if the owner requested the sheet be deleted.
    bool renderCanvas(cp::notepad::Tab& tab, cp::notepad::Sheet& sheet);

};

} // namespace ui
} // namespace cp
