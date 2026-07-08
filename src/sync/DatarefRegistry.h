#pragma once
// DatarefRegistry.h — resolves XPLM dataref handles and caches their types.
// Also maps dataref index (from config) to its handle and metadata.

#include "../config/Zone.h"
#include <cstdint>
#include <string>
#include <vector>

// Forward-declare XPLM type to avoid including SDK header here
typedef void* XPLMDataRef;

namespace cp {

enum class DrType { UNKNOWN, INT, FLOAT, DOUBLE, INT_ARR, FLOAT_ARR, DATA };

struct RegisteredDataref {
    uint16_t     index;    // config index (used as wire ID)
    std::string  path;
    std::string  zoneId;
    SyncMode     mode;
    XPLMDataRef  handle  = nullptr;
    DrType       type    = DrType::UNKNOWN;
    bool         writable= false;
};

struct RegisteredCommand {
    uint16_t    index;
    std::string path;
    std::string zoneId;
    void*       handle = nullptr;  // XPLMCommandRef
};

class DatarefRegistry {
public:
    // Resolve all datarefs/commands from config. Call on main thread after aircraft load.
    void build(const std::vector<DatarefEntry>& drEntries,
               const std::vector<CommandEntry>& cmdEntries);

    // Lookup by index
    const RegisteredDataref* getDr(uint16_t index) const;
    const RegisteredCommand* getCmd(uint16_t index) const;

    const std::vector<RegisteredDataref>& datarefs() const { return datarefs_; }
    const std::vector<RegisteredCommand>& commands()  const { return commands_; }

    void clear();

private:
    std::vector<RegisteredDataref> datarefs_;
    std::vector<RegisteredCommand> commands_;
};

} // namespace cp
