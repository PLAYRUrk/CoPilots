#include "DatarefRegistry.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>

namespace cp {

static int cmdHandlerCb(XPLMCommandRef, XPLMCommandPhase phase, void* refcon)
{
    if (phase == xplm_CommandBegin) {
        auto* rc = static_cast<RegisteredCommand*>(refcon);
        if (rc->cb) rc->cb(rc->index);
    }
    return 1; // pass through — don't consume the command
}

static DrType resolveType(XPLMDataRef handle)
{
    XPLMDataTypeID types = XPLMGetDataRefTypes(handle);
    if (types & xplmType_Double)     return DrType::DOUBLE;
    if (types & xplmType_FloatArray) return DrType::FLOAT_ARR;
    if (types & xplmType_IntArray)   return DrType::INT_ARR;
    if (types & xplmType_Float)      return DrType::FLOAT;
    if (types & xplmType_Int)        return DrType::INT;
    if (types & xplmType_Data)       return DrType::DATA;
    return DrType::UNKNOWN;
}

void DatarefRegistry::build(const std::vector<DatarefEntry>& drEntries,
                             const std::vector<CommandEntry>& cmdEntries,
                             std::function<void(uint16_t)> onCommandFired)
{
    clear();

    for (size_t i = 0; i < drEntries.size(); ++i) {
        const auto& e = drEntries[i];
        RegisteredDataref rd;
        rd.index  = static_cast<uint16_t>(i);
        rd.path   = e.path;   // keep original (with [N]) for logging/hashing
        rd.zoneId = e.zoneId;
        rd.mode   = e.mode;

        // Parse optional [N] array-index suffix (e.g. "mixture_ratio[0]").
        // XPLMFindDataRef does not understand [N] — strip it and store the index.
        std::string basePath = e.path;
        {
            size_t bk = e.path.rfind('[');
            if (bk != std::string::npos && e.path.size() > bk + 1
                && e.path.back() == ']')
            {
                int idx = 0;
                bool valid = true;
                for (size_t ci = bk + 1; ci < e.path.size() - 1; ++ci) {
                    char ch = e.path[ci];
                    if (ch < '0' || ch > '9') { valid = false; break; }
                    idx = idx * 10 + (ch - '0');
                }
                if (valid) {
                    rd.arrayIndex = idx;
                    basePath = e.path.substr(0, bk);
                }
            }
        }

        rd.handle = XPLMFindDataRef(basePath.c_str());
        if (!rd.handle) {
            LogWarning("DatarefRegistry: not found: %s", e.path.c_str());
        } else {
            rd.type     = resolveType(rd.handle);
            // If we're indexing into an array, treat the wire value as a scalar element.
            if (rd.arrayIndex >= 0) {
                if (rd.type == DrType::FLOAT_ARR)    rd.type = DrType::FLOAT;
                else if (rd.type == DrType::INT_ARR) rd.type = DrType::INT;
            }
            rd.writable = XPLMCanWriteDataRef(rd.handle) != 0;
        }
        datarefs_.push_back(std::move(rd));
    }
    Log("DatarefRegistry: registered %zu datarefs", datarefs_.size());

    // Reserve so that push_back never reallocates — pointers to elements are
    // passed as refcon to XPLMRegisterCommandHandler and must remain stable.
    commands_.reserve(cmdEntries.size());
    for (size_t i = 0; i < cmdEntries.size(); ++i) {
        const auto& e = cmdEntries[i];
        RegisteredCommand rc;
        rc.index  = static_cast<uint16_t>(i);
        rc.path   = e.path;
        rc.zoneId = e.zoneId;
        rc.handle = XPLMFindCommand(e.path.c_str());
        if (!rc.handle) {
            LogWarning("DatarefRegistry: command not found: %s", e.path.c_str());
        } else if (onCommandFired) {
            rc.cb = onCommandFired;
        }
        commands_.push_back(std::move(rc));
        if (commands_.back().handle && commands_.back().cb) {
            XPLMRegisterCommandHandler(
                static_cast<XPLMCommandRef>(commands_.back().handle),
                cmdHandlerCb, 1 /* before */, &commands_.back());
        }
    }
    Log("DatarefRegistry: registered %zu commands", commands_.size());
}

const RegisteredDataref* DatarefRegistry::getDr(uint16_t index) const
{
    if (index < datarefs_.size()) return &datarefs_[index];
    return nullptr;
}

const RegisteredCommand* DatarefRegistry::getCmd(uint16_t index) const
{
    if (index < commands_.size()) return &commands_[index];
    return nullptr;
}

void DatarefRegistry::clear()
{
    for (auto& rc : commands_) {
        if (rc.handle && rc.cb) {
            XPLMUnregisterCommandHandler(
                static_cast<XPLMCommandRef>(rc.handle),
                cmdHandlerCb, 1, &rc);
        }
    }
    datarefs_.clear();
    commands_.clear();
}

}
