#pragma once

class ENGINE_API CObject;

namespace Feel
{
class ENGINE_API Sound
{
public:
    virtual void feel_sound_new(CObject* who, int type, CSound_UserDataPtr user_data, const Fvector& Position, float power){};
    // how far this listener hears the sound, as a share of its own max_ai_distance:
    // a stalker who has not yet noticed the player is half deaf to him
    virtual float feel_sound_range(CObject* who, int type) { return 1.f; }
};
}; // namespace Feel
