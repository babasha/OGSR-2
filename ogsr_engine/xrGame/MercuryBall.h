///////////////////////////////////////////////////////////////
// MercuryBall.h
// MercuryBall - переливающийся и колыхающийся шар
// перекатывается с места на место
///////////////////////////////////////////////////////////////

#pragma once
#include "Artifact.h"

class CMercuryBall : public CArtefact
{
private:
    typedef CArtefact inherited;

public:
    CMercuryBall(void);
    virtual ~CMercuryBall(void);

    virtual void Load(LPCSTR section);

protected:
    virtual void UpdateCLChild();

    //время последнего обновления поведения шара
    ALife::_TIME_ID m_timeLastUpdate;
    //время между апдейтами
    ALife::_TIME_ID m_timeToUpdate;

    //диапазон импульсов катания шара
    float m_fImpulseMin;
    float m_fImpulseMax;
};

