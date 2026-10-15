#pragma once
#include "state_defs.h"
#include "control_com_defs.h"

class IStateManagerBase
{
public:
    virtual ~IStateManagerBase(){};
    virtual void reinit() = 0;
    virtual void update() = 0;
    virtual void force_script_state(EMonsterState state) = 0;
    virtual void execute_script_state() = 0;
    virtual void critical_finalize() = 0;
    virtual void remove_links(CObject* O) = 0;
    virtual EMonsterState get_state_type() = 0;

    virtual bool check_control_start_conditions(ControlCom::EControlType type) = 0;
};
