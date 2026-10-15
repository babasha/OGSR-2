//#include "stdafx.h"
#include "ode_include.h"

/* Class ID */

extern int dTriListClass;

/* Per triangle callback */

typedef int dTriCallback(dGeomID TriList, dGeomID RefObject, int TriangleIndex);

/* Per object callback */

typedef void dTriArrayCallback(dGeomID TriList, dGeomID RefObject, const int* TriIndices, int TriCount);

/* Construction */

dxGeom* dCreateTriList(dSpaceID space, dTriCallback* Callback, dTriArrayCallback* ArrayCallback);