#pragma once

class CInvisibility
{
    u32 m_time_start_blink{};
    u32 m_time_last_blink{};
    bool m_blink{};
    bool m_cur_visibility{};

    // external parameters
    u32 timeBlink{};
    u32 timeBlinkInterval{};

    bool m_manual{};

    bool m_active{}; //
    float m_energy{}; // [0..1]
    float m_speed{}; // energy change speed (external)

protected:
    virtual void reload(LPCSTR section);
    virtual void reinit();
    virtual void frame_update();

    virtual void on_change_visibility(bool b_visibility) {}
    virtual void on_activate() {}
    virtual void on_deactivate() {}

public:
    void activate();
    void deactivate();
    IC float energy() { return m_energy; }
    IC bool active() { return m_active; }

    // manual switching
    void set_manual_control(bool b_man = true);
    void manual_activate();
    void manual_deactivate();

private:
    void start_blink();
    void stop_blink();
    void update_blink();
};
