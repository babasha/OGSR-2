#pragma once

#include "TriPrimitiveCollideClassDef.h"
#include "../ode_include.h"
#include "../MathUtils.h"
#include "dcTriListCollider.h"

struct Triangle;
struct dxBox
{
    dVector3 side; // side lengths (x,y,z)
};

IC float dcTriListCollider::dBoxProj(dxGeom* box, const dReal* normal)
{
    VERIFY(dGeomGetClass(box) == dBoxClass);
    float hside[3];
    dGeomBoxGetLengths(box, hside);
    hside[0] *= .5f;
    hside[1] *= 0.5f;
    hside[2] *= 0.5f;
    const dReal* R = dGeomGetRotation(box);
    return dFabs(dDOT14(normal, R + 0) * hside[0]) + dFabs(dDOT14(normal, R + 1) * hside[1]) + dFabs(dDOT14(normal, R + 2) * hside[2]);
}

IC bool dcTriListCollider::CrossProjLine14(const dReal* pt1, const dReal* vc1, const dReal* pt2, const dReal* vc2, dReal hside, dReal* proj)
{
    dVector3 ac = {pt1[0] - pt2[0], pt1[1] - pt2[1], pt1[2] - pt2[2]};

    // dReal vc2_2=dDOT44(vc2,vc2);
    dReal vc1_vc2 = dDOT41(vc2, vc1);
    dReal vc1_2 = dDOT(vc1, vc1);

    dReal factor = /*vc2_2*/ vc1_2 - vc1_vc2 * vc1_vc2;
    if (factor == 0.f)
    {
        // proj[0]=dInfinity;
        // proj[1]=dInfinity;
        // proj[2]=dInfinity;
        return false;
    }
    dReal ac_vc1 = dDOT(vc1, ac);
    dReal ac_vc2 = dDOT14(ac, vc2);
    dReal t1 = (ac_vc2 * vc1_vc2 - ac_vc1 /*vc2_2*/) / factor;

    if (t1 < 0.f)
        return false;
    if (t1 > 1.f)
        return false;

    dReal t2 = (ac_vc1 * vc1_vc2 - ac_vc2 * vc1_2) / factor;

    dReal nt2 = t2; //*_sqrt(vc2_2);
    if (nt2 > hside || nt2 < -hside)
        return false;

    proj[0] = pt1[0] + vc1[0] * t1;
    proj[1] = pt1[1] + vc1[1] * t1;
    proj[2] = pt1[2] + vc1[2] * t1;

    return true;
}
