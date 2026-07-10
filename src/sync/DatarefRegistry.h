#pragma once

#include "../config/Zone.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

typedef void* XPLMDataRef;

namespace cp {

enum class DrType { UNKNOWN, INT, FLOAT, DOUBLE, INT_ARR, FLOAT_ARR, DATA };

struct RegisteredDataref {
    uint16_t     index;
    std::string  path;       // original path from config (may contain [N] suffix)
    std::string  zoneId;
    SyncMode     mode;
    XPLMDataRef  handle     = nullptr;
    DrType       type       = DrType::UNKNOWN;
    bool         writable   = false;
    // ≥0: index into a float/int array dataref (path had [N] suffix); -1: scalar/whole array
    int          arrayIndex = -1;
};

struct RegisteredCommand {
    uint16_t    index;
    std::string path;
    std::string zoneId;
    void*       handle = nullptr;
    std::function<void(uint16_t)> cb;
};

class DatarefRegistry {
public:
    void build(const std::vector<DatarefEntry>& drEntries,
               const std::vector<CommandEntry>& cmdEntries,
               std::function<void(uint16_t)> onCommandFired = {});

    const RegisteredDataref* getDr(uint16_t index) const;
    const RegisteredCommand* getCmd(uint16_t index) const;

    const std::vector<RegisteredDataref>& datarefs() const { return datarefs_; }
    const std::vector<RegisteredCommand>& commands()  const { return commands_; }

    void clear();

private:
    std::vector<RegisteredDataref> datarefs_;
    std::vector<RegisteredCommand> commands_;
};

}
