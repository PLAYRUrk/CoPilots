#pragma once
// Session.h — lobby state: participants, role/zone assignments, authority map.
// Used on both server and client (client has a read-only view, server has full control).

#include "Participant.h"
#include "../config/Zone.h"
#include <vector>
#include <string>
#include <unordered_map>
#include <mutex>
#include <functional>

namespace cp {

// Maps zone_id → participant_id (who owns this zone)
using AuthorityMap = std::unordered_map<std::string, ParticipantId>;

class Session {
public:
    // ── Mutating (server-side / admin operations) ──────────────────────────
    ParticipantId addParticipant(const std::string& nick);
    void          removeParticipant(ParticipantId id);
    bool          setRole(ParticipantId id, const std::string& roleId,
                          const std::vector<Role>& availableRoles);
    bool          assignZones(ParticipantId id, const std::vector<std::string>& zoneIds);
    void          setPhysicsMaster(ParticipantId id);

    // ── Query ──────────────────────────────────────────────────────────────
    const Participant* find(ParticipantId id) const;
    const std::vector<Participant>& participants() const { return participants_; }
    ParticipantId       physicsMasterId() const { return physicsMasterId_; }
    ParticipantId       myId()            const { return myId_; }
    bool                isHost()          const { return isHost_; }
    bool                isPhysicsMaster() const { return myId_ == physicsMasterId_; }

    // Zone authority: who can write to this zone?
    ParticipantId authorityFor(const std::string& zoneId) const;
    bool          iOwnZone(const std::string& zoneId) const;

    // Full authority map (zone_id → participant_id)
    AuthorityMap buildAuthorityMap() const;

    // ── State loading (client receives from server) ────────────────────────
    void setMyId(ParticipantId id) { myId_ = id; }
    void setIsHost(bool v)         { isHost_ = v; }
    void updateFromAuthorityMap(const AuthorityMap& map);

    // Callback fired whenever session state changes (for UI refresh)
    std::function<void()> onChanged;

    void clear();

private:
    std::vector<Participant> participants_;
    ParticipantId            physicsMasterId_ = INVALID_PARTICIPANT_ID;
    ParticipantId            myId_            = INVALID_PARTICIPANT_ID;
    bool                     isHost_          = false;

    mutable std::mutex mu_;

    Participant* findMut(ParticipantId id);
    void         notifyChanged();
    uint8_t      nextId_ = 1;
};

} // namespace cp
