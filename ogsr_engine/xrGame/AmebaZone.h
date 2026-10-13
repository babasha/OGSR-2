#pragma once

class CAmebaZone : public CVisualZone, public CPHUpdateObject
{
    typedef CVisualZone inherited;
    float m_fVelocityLimit;

    // The blowout hits at a fixed rate instead of once per frame (see BlowoutState).
    static constexpr float kHitTickMs = 1000.f / 60.f;
    static constexpr u32 kMaxTicksPerUpdate = 6; // a hitch must not land a burst of hits
    float m_fHitTickAccum{};
    u32 m_uHitTicks{1}; // ticks folded into the current Affect() call

public:
    CAmebaZone();
    ~CAmebaZone();
    virtual void Affect(SZoneObjectInfo* O);

protected:
    virtual void PhTune(dReal step);
    virtual void PhDataUpdate(dReal step) { ; }
    virtual bool BlowoutState();
    virtual void SwitchZoneState(EZoneState new_state);
    virtual void Load(LPCSTR section);
    virtual float distance_to_center(CObject* O);
};
