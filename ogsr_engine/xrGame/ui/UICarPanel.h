#pragma once

#include "UIWindow.h"
//#include "uipointergage.h"

class CUICarPanel : public CUIWindow
{
private:
    typedef CUIWindow inherited;

    CUIStatic UIStaticCarHealth;
    CUIProgressBar UICarHealthBar;
    //	CUIPointerGage		UISpeedometer;
    //	CUIPointerGage		UITachometer;
public:
    // Установить
    void SetCarHealth(float value);
    void SetSpeed(float speed);
    void SetRPM(float rmp);
    void Init(float x, float y, float width, float height);

    // Read side for the RmlUi HUD: the car's health as 0..1.
    float HealthFrac() { return clampr(UICarHealthBar.GetProgressPos() / 100.f, 0.f, 1.f); }
};