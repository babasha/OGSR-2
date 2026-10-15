#pragma once

// Split out of GameTaskDefs.h so the enum can be included where it used to be forward-declared
// (an opaque unscoped enum without a fixed underlying type is an MSVC extension).
enum ETaskState
{
    eTaskStateFail = 0,
    eTaskStateInProgress,
    eTaskStateCompleted,
    eTaskStateSkiped,
    //.	eTaskUserDefined,
    eTaskStateDummy = u32(-1)
};
