#pragma once

#include "Participant.h"
#include "../config/Zone.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace cp {

using AuthorityMap = std::unordered_map<std::string, ParticipantId>;

class Session {
public:
    ParticipantId addParticipant(const std::string& nick);
    void          removeParticipant(ParticipantId id);
    bool          setRole(ParticipantId id, const std::string& roleId,
                          const std::vector<Role>& availableRoles);
    bool          assignZones(ParticipantId id, const std::vector<std::string>& zoneIds);
    void          setPhysicsMaster(ParticipantId id);
    void          setWeatherMaster(ParticipantId id);

    const Participant* find(ParticipantId id) const;
    const std::vector<Participant>& participants() const { return participants_; }
    ParticipantId       physicsMasterId()  const { return physicsMasterId_; }
    ParticipantId       weatherMasterId()  const { return weatherMasterId_; }
    ParticipantId       myId()             const { return myId_; }
    bool                isHost()           const { return isHost_; }
    bool                isPhysicsMaster()  const { return myId_ == physicsMasterId_; }
    bool                isWeatherMaster()  const { return myId_ == weatherMasterId_; }

    ParticipantId authorityFor(const std::string& zoneId) const;
    bool          iOwnZone(const std::string& zoneId) const;

    AuthorityMap buildAuthorityMap() const;

    void setMyId(ParticipantId id) { myId_ = id; }
    void setIsHost(bool v)         { isHost_ = v; }
    void updateFromAuthorityMap(const AuthorityMap& map);

    std::function<void()> onChanged;

    void clear();

private:
    std::vector<Participant> participants_;
    ParticipantId            physicsMasterId_ = INVALID_PARTICIPANT_ID;
    ParticipantId            weatherMasterId_ = INVALID_PARTICIPANT_ID;
    ParticipantId            myId_            = INVALID_PARTICIPANT_ID;
    bool                     isHost_          = false;

    mutable std::mutex mu_;

    Participant* findMut(ParticipantId id);
    void         notifyChanged();
    uint8_t      nextId_ = 1;
};

}
