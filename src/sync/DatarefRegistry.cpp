#include "DatarefRegistry.h"
#include "../Log.h"
#include <XPLM/XPLMDataAccess.h>
#include <XPLM/XPLMUtilities.h>

namespace cp {

static DrType resolveType(XPLMDataRef handle)
{
    XPLMDataTypeID types = XPLMGetDataRefTypes(handle);
    // Check from most specific to least
    if (types & xplmType_Double)   return DrType::DOUBLE;
    if (types & xplmType_FloatArray) return DrType::FLOAT_ARR;
    if (types & xplmType_IntArray)   return DrType::INT_ARR;
    if (types & xplmType_Data)       return DrType::DATA;
    if (types & xplmType_Float)    return DrType::FLOAT;
    if (types & xplmType_Int)      return DrType::INT;
    return DrType::UNKNOWN;
}

void DatarefRegistry::build(const std::vector<DatarefEntry>& drEntries,
                             const std::vector<CommandEntry>& cmdEntries)
{
    clear();

    for (size_t i = 0; i < drEntries.size(); ++i) {
        const auto& e = drEntries[i];
        RegisteredDataref rd;
        rd.index  = static_cast<uint16_t>(i);
        rd.path   = e.path;
        rd.zoneId = e.zoneId;
        rd.mode   = e.mode;
        rd.handle = XPLMFindDataRef(e.path.c_str());
        if (!rd.handle) {
            LogWarning("DatarefRegistry: not found: %s", e.path.c_str());
        } else {
            rd.type     = resolveType(rd.handle);
            rd.writable = XPLMCanWriteDataRef(rd.handle) != 0;
        }
        datarefs_.push_back(std::move(rd));
    }
    Log("DatarefRegistry: registered %zu datarefs", datarefs_.size());

    for (size_t i = 0; i < cmdEntries.size(); ++i) {
        const auto& e = cmdEntries[i];
        RegisteredCommand rc;
        rc.index  = static_cast<uint16_t>(i);
        rc.path   = e.path;
        rc.zoneId = e.zoneId;
        rc.handle = XPLMFindCommand(e.path.c_str());
        if (!rc.handle)
            LogWarning("DatarefRegistry: command not found: %s", e.path.c_str());
        commands_.push_back(std::move(rc));
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
    datarefs_.clear();
    commands_.clear();
}

} // namespace cp
