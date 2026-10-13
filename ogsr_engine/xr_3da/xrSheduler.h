#pragma once

#include "ISheduled.h"

class ENGINE_API CSheduler
{
private:
    struct Item
    {
        u32 dwTimeForExecute;
        u32 dwTimeOfLastExecute;
        shared_str scheduled_name;
        ISheduled* Object;
    };
    // Порядок очереди: min-куча по dwTimeForExecute. Живёт здесь, а не в .cpp,
    // потому что Item приватный. Обоснование и замер -- в xrSheduler.cpp.
    struct HeapCmp
    {
        // std::*_heap строят МАКСИ-кучу, поэтому "больше" тут значит "позже":
        // вершиной оказывается самый ранний дедлайн.
        bool operator()(const Item& a, const Item& b) const { return a.dwTimeForExecute > b.dwTimeForExecute; }
    };
    struct ItemReg
    {
        BOOL OP;
        BOOL RT;
        ISheduled* Object;
    };

private:
    xr_vector<Item> ItemsRT;
    xr_vector<Item> Items;
    xr_vector<ItemReg> Registration;

    ISheduled* m_current_step_obj{};

    bool m_processing_now;

    void internal_Register(ISheduled* A, BOOL RT = FALSE);
    bool internal_Unregister(const ISheduled* A, BOOL RT);
    void internal_Registration();

public:
    void ProcessStep();
    void Update();

#ifdef DEBUG
    bool Registered(ISheduled* object) const;
#endif // DEBUG
    void Register(ISheduled* A, BOOL RT = FALSE);
    void Unregister(ISheduled* A, bool force = false);

    void Initialize();
    void Destroy();
};
