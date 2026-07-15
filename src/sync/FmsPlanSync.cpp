#include "FmsPlanSync.h"
#include "../Log.h"

#include <algorithm>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace cp {

static uint32_t fnv1a(const std::vector<uint8_t>& data)
{
    uint32_t h = 2166136261u;
    for (uint8_t b : data) { h ^= b; h *= 16777619u; }
    return h;
}

static bool hasPlanExtension(const std::string& name)
{
    auto dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    return ext == "fms" || ext == "flp";
}

// Windows treats CON/PRN/AUX/NUL/COM1-9/LPT1-9 as devices even with an
// extension ("con.fms" cannot be created as a normal file).
static bool isReservedDeviceName(const std::string& name)
{
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return (char)::tolower(c); });
    if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul")
        return true;
    if (stem.size() == 4 && (stem.compare(0, 3, "com") == 0 ||
                             stem.compare(0, 3, "lpt") == 0)
        && stem[3] >= '1' && stem[3] <= '9')
        return true;
    return false;
}

void FmsPlanSync::init(net::NetThread* net, const std::string& xpSystemPath)
{
    net_ = net;
    dir_ = xpSystemPath + "Output/FMS plans/";
    hashCache_.clear();  // dir may have changed; cache keys are bare names
    reset();
}

void FmsPlanSync::reset()
{
    inventory_.clear();
    requested_.clear();
    announced_   = false;
    scanned_     = false;
    rescanTimer_ = 0.f;
    time_        = 0.f;
}

bool FmsPlanSync::sanitizedName(const std::string& name)
{
    if (name.empty() || name.size() > 128) return false;
    if (name[0] == '.') return false;
    if (name.find('/') != std::string::npos)  return false;
    if (name.find('\\') != std::string::npos) return false;
    if (name.find(':') != std::string::npos)  return false;
    if (isReservedDeviceName(name)) return false;
    return hasPlanExtension(name);
}

std::vector<FmsPlanSync::FileInfo> FmsPlanSync::scanFolder()
{
    std::vector<FileInfo> result;
    std::error_code ec;
    if (!fs::exists(dir_, ec)) return result;

    for (const auto& entry : fs::directory_iterator(dir_, ec)) {
        if (result.size() >= MAX_FILES) break;
        if (!entry.is_regular_file(ec)) continue;

        std::string name = entry.path().filename().string();
        if (!sanitizedName(name)) continue;

        auto sz = entry.file_size(ec);
        if (ec || sz == 0 || sz > FILE_LIMIT) continue;
        auto mt = entry.last_write_time(ec);
        int64_t mtime = ec ? 0 : (int64_t)mt.time_since_epoch().count();

        FileInfo fi;
        fi.name = name;
        fi.size = static_cast<uint32_t>(sz);

        auto it = hashCache_.find(name);
        if (it != hashCache_.end() && mtime != 0 &&
            it->second.size == (uint64_t)sz && it->second.mtime == mtime) {
            fi.hash = it->second.hash;
        } else {
            std::ifstream f(entry.path(), std::ios::binary);
            if (!f.is_open()) continue;
            std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
            fi.size = static_cast<uint32_t>(data.size());
            fi.hash = fnv1a(data);
            hashCache_[name] = { (uint64_t)data.size(), mtime, fi.hash };
        }
        result.push_back(std::move(fi));
    }
    return result;
}

void FmsPlanSync::ensureScanned()
{
    if (scanned_) return;
    scanned_   = true;
    inventory_ = scanFolder();
}

void FmsPlanSync::broadcastList(const std::vector<FileInfo>& inv, uint8_t target)
{
    if (!net_ || inv.empty()) return;
    auto b = proto::MsgBuilder(proto::MsgType::FMS_LIST)
                 .u16(static_cast<uint16_t>(inv.size()));
    for (const auto& fi : inv)
        b.str(fi.name).u32(fi.size).u32(fi.hash);
    net::OutboundMsg out;
    out.target = target;
    out.frame  = b.build();
    net_->outTcp.push(std::move(out));
}

bool FmsPlanSync::haveFile(const std::string& name) const
{
    for (const auto& fi : inventory_)
        if (fi.name == name) return true;
    return false;
}

void FmsPlanSync::tick(float dt, bool connected)
{
    if (!connected) {
        if (announced_ || scanned_) reset();
        return;
    }

    time_ += dt;

    if (!announced_) {
        announced_ = true;
        ensureScanned();
        broadcastList(inventory_);
        Log("FmsPlanSync: announced %zu plan(s) from '%s'",
            inventory_.size(), dir_.c_str());
        return;
    }

    // Periodic rescan: pick up plans the user saved mid-session.
    rescanTimer_ += dt;
    if (rescanTimer_ < RESCAN_S) return;
    rescanTimer_ = 0.f;

    auto fresh = scanFolder();
    std::vector<FileInfo> added;
    for (const auto& fi : fresh) {
        bool known = false;
        for (const auto& old : inventory_)
            if (old.name == fi.name) { known = true; break; }
        if (!known) added.push_back(fi);
    }
    inventory_ = std::move(fresh);
    if (!added.empty()) broadcastList(added);
}

void FmsPlanSync::announce(uint8_t target)
{
    ensureScanned();
    broadcastList(inventory_, target);
}

void FmsPlanSync::onList(proto::MsgReader& r)
{
    if (!net_) return;
    ensureScanned();
    uint16_t count = r.u16();

    std::vector<std::string> missing;
    for (uint16_t i = 0; i < count && r.ok(); ++i) {
        std::string name = r.str();
        (void)r.u32();  // size
        (void)r.u32();  // hash — informational; existing local files are kept as-is
        if (!sanitizedName(name)) continue;
        if (haveFile(name)) continue;
        auto rit = requested_.find(name);
        if (rit != requested_.end() && time_ - rit->second < REREQUEST_S) continue;
        missing.push_back(name);
    }
    if (missing.empty()) return;

    auto b = proto::MsgBuilder(proto::MsgType::FMS_REQUEST)
                 .u16(static_cast<uint16_t>(missing.size()));
    for (const auto& n : missing) {
        b.str(n);
        requested_[n] = time_;
    }
    net::OutboundMsg out;
    out.target = 0xFF;
    out.frame  = b.build();
    net_->outTcp.push(std::move(out));
}

void FmsPlanSync::onRequest(proto::MsgReader& r, std::vector<std::string>* unserved)
{
    if (!net_) return;
    ensureScanned();
    uint16_t count = r.u16();
    for (uint16_t i = 0; i < count && r.ok(); ++i) {
        std::string name = r.str();
        if (!sanitizedName(name)) continue;
        if (!haveFile(name)) {
            if (unserved) unserved->push_back(name);
            continue;
        }

        std::ifstream f(dir_ + name, std::ios::binary);
        if (!f.is_open()) continue;
        std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
        if (data.empty() || data.size() > FILE_LIMIT) continue;

        auto frame = proto::MsgBuilder(proto::MsgType::FMS_FILE)
                         .str(name)
                         .bytes(data.data(), static_cast<uint32_t>(data.size()))
                         .build();
        net::OutboundMsg out;
        out.target = 0xFF;
        out.frame  = std::move(frame);
        net_->outTcp.push(std::move(out));
    }
}

void FmsPlanSync::onFile(proto::MsgReader& r)
{
    std::string name = r.str();
    std::vector<uint8_t> data = r.bytes();

    if (!sanitizedName(name)) return;
    if (data.empty() || data.size() > FILE_LIMIT) return;
    ensureScanned();
    // Never overwrite an existing local plan — first content wins.
    if (haveFile(name)) return;
    {
        std::error_code ec;
        if (fs::exists(dir_ + name, ec)) return;
        fs::create_directories(dir_, ec);
    }

    std::ofstream out(dir_ + name, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        LogWarning("FmsPlanSync: cannot write '%s%s'", dir_.c_str(), name.c_str());
        return;
    }
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
    out.close();

    FileInfo fi;
    fi.name = name;
    fi.size = static_cast<uint32_t>(data.size());
    fi.hash = fnv1a(data);
    {
        std::error_code ec;
        auto mt = fs::last_write_time(dir_ + name, ec);
        int64_t mtime = ec ? 0 : (int64_t)mt.time_since_epoch().count();
        hashCache_[name] = { (uint64_t)data.size(), mtime, fi.hash };
    }
    inventory_.push_back(std::move(fi));
    requested_.erase(name);

    Log("FmsPlanSync: received flight plan '%s' (%zu bytes)", name.c_str(), data.size());
}

}
