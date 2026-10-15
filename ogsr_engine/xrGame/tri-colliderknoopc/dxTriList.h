//#include "stdafx.h"
#ifndef __DXTRILIST_INCLUDED__

#define __DXTRILIST_INCLUDED__

#include "../ode_include.h"

/* Class ID */

extern int dTriListClass;

/* Per triangle callback */

typedef int dTriCallback(dGeomID TriList, dGeomID RefObject, int TriangleIndex);

/* Per object callback */

typedef void dTriArrayCallback(dGeomID TriList, dGeomID RefObject, const int* TriIndices, int TriCount);

/* Construction */

dxGeom* dCreateTriList(dSpaceID space, dTriCallback* Callback, dTriArrayCallback* ArrayCallback);

/* Internal types */

class dcTriListCollider;

struct dxTriList
{
    dReal p[4]; // dxPlane

    dTriCallback* Callback;

    dTriArrayCallback* ArrayCallback;

    dcTriListCollider* Collider;
};

#endif //__DXTRILIST_INCLUDED__