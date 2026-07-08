#include "Session.h"
#include "../Log.h"
#include <algorithm>

namespace cp {

void Session::clear()
{
    std::lock_guard<std::mutex> lk(mu_);
    participants_.clear();
    physicsMasterId_ = INVALID_PARTICIPANT_ID;
    weatherMasterId_ = INVALID_PARTICIPANT_ID;
    myId_            = INVALID_PARTICIPANT_ID;
    isHost_          = false;
    nextId_          = 1;
}

ParticipantId Session::addParticipant(const std::string& nick)
{
    std::lock_guard<std::mutex> lk(mu_);
    Participant p;
    p.id   = nextId_++;
    p.nick = nick;
    if (participants_.empty()) {
        physicsMasterId_ = p.id;
        weatherMasterId_ = p.id;
    }
    participants_.push_back(p);
    Log("Session: added participant %u '%s'", p.id, nick.c_str());
    notifyChanged();
    return p.id;
}

void Session::removeParticipant(ParticipantId id)
{
    std::lock_guard<std::mutex> lk(mu_);
    auto it = std::find_if(participants_.begin(), participants_.end(),
                           [id](const Participant& p){ return p.id == id; });
    if (it == participants_.end()) return;
    Log("Session: removed participant %u '%s'", id, it->nick.c_str());
    participants_.erase(it);
    if (physicsMasterId_ == id)
        physicsMasterId_ = participants_.empty()
                           ? INVALID_PARTICIPANT_ID : participants_[0].id;
    if (weatherMasterId_ == id)
        weatherMasterId_ = participants_.empty()
                           ? INVALID_PARTICIPANT_ID : participants_[0].id;
    notifyChanged();
}

bool Session::setRole(ParticipantId id, const std::string& roleId,
                      const std::vector<Role>& availableRoles)
{
    std::lock_guard<std::mutex> lk(mu_);
    Participant* p = findMut(id);
    if (!p) return false;

    p->roleId = roleId;
    p->zoneIds.clear();

    for (const auto& r : availableRoles) {
        if (r.id == roleId) {
            p->zoneIds = r.zoneIds;
            break;
        }
    }
    Log("Session: participant %u assigned role '%s' (%zu zones)",
        id, roleId.c_str(), p->zoneIds.size());
    notifyChanged();
    return true;
}

bool Session::assignZones(ParticipantId id, const std::vector<std::string>& zoneIds)
{
    std::lock_guard<std::mutex> lk(mu_);
    Participant* p = findMut(id);
    if (!p) return false;
    p->zoneIds = zoneIds;
    notifyChanged();
    return true;
}

void Session::setPhysicsMaster(ParticipantId id)
{
    std::lock_guard<std::mutex> lk(mu_);
    physicsMasterId_ = id;
    for (auto& p : participants_)
        p.isPhysicsMaster = (p.id == id);
    Log("Session: physics master → participant %u", id);
    notifyChanged();
}

void Session::setWeatherMaster(ParticipantId id)
{
    std::lock_guard<std::mutex> lk(mu_);
    weatherMasterId_ = id;
    for (auto& p : participants_)
        p.isWeatherMaster = (p.id == id);
    Log("Session: weather master → participant %u", id);
    notifyChanged();
}

const Participant* Session::find(ParticipantId id) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : participants_)
        if (p.id == id) return &p;
    return nullptr;
}

ParticipantId Session::authorityFor(const std::string& zoneId) const
{
    std::lock_guard<std::mutex> lk(mu_);
    for (const auto& p : participants_)
        for (const auto& z : p.zoneIds)
            if (z == zoneId) return p.id;
    return INVALID_PARTICIPANT_ID;
}

bool Session::iOwnZone(const std::string& zoneId) const
{
    return authorityFor(zoneId) == myId_;
}

AuthorityMap Session::buildAuthorityMap() const
{
    std::lock_guard<std::mutex> lk(mu_);
    AuthorityMap m;
    for (const auto& p : participants_)
        for (const auto& z : p.zoneIds)
            if (!m.count(z))   // first participant in the list wins; no duplicate ownership
                m[z] = p.id;
    return m;
}

void Session::updateFromAuthorityMap(const AuthorityMap& map)
{
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& p : participants_) p.zoneIds.clear();
    for (const auto& [zone, pid] : map) {
        Participant* p = findMut(pid);
        if (p) p->zoneIds.push_back(zone);
    }
    notifyChanged();
}

Participant* Session::findMut(ParticipantId id)
{
    for (auto& p : participants_)
        if (p.id == id) return &p;
    return nullptr;
}

void Session::notifyChanged()
{
    if (onChanged) onChanged();
}

}
