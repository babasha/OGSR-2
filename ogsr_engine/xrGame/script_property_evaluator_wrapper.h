////////////////////////////////////////////////////////////////////////////
//	Module 		: script_property_evaluator_wrapper.h
//	Created 	: 19.03.2004
//  Modified 	: 26.03.2004
//	Author		: Dmitriy Iassenev
//	Description : Script property evaluator wrapper
////////////////////////////////////////////////////////////////////////////

#pragma once

#include "property_evaluator.h"

class CScriptGameObject;

typedef CPropertyEvaluator<CScriptGameObject> CScriptPropertyEvaluator;

class CScriptPropertyEvaluatorWrapper : public CScriptPropertyEvaluator, public luabind::wrap_base
{
public:
    IC CScriptPropertyEvaluatorWrapper(CScriptGameObject* object = 0, LPCSTR evaluator_name = "");
    virtual void setup(CScriptGameObject* object, CPropertyStorage* storage);
    static void setup_static(CScriptPropertyEvaluator* evaluator, CScriptGameObject* object, CPropertyStorage* storage);
    virtual bool evaluate();
    static bool evaluate_static(CScriptPropertyEvaluator* evaluator);

private:
    // ai_eval_cache (script_property_evaluator_wrapper.cpp): the last answer of a listed Lua class, held for N ms
    u32 m_cache_until = 0;
    u32 m_cache_gen = 0;
    bool m_cache_on = false;
    bool m_cache_val = false;
};

// ai_eval_cache "<lua class>,<lua class>" + ai_eval_cache_ms: which script evaluators answer from memory
extern char g_eval_cache_list[512];
extern int g_eval_cache_ms;
extern u32 g_eval_cache_gen; // bumped by the console command: every evaluator looks its class up again

#include "script_property_evaluator_wrapper_inline.h"