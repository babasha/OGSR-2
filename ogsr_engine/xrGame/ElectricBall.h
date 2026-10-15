///////////////////////////////////////////////////////////////
// ElectricBall.h
// ElectricBall - артефакт электрический шар
///////////////////////////////////////////////////////////////

#pragma once
#include "Artifact.h"

class CElectricBall : public CArtefact
{
private:
    typedef CArtefact inherited;

public:
    CElectricBall(void);
    virtual ~CElectricBall(void);

    virtual void Load(LPCSTR section);

protected:
    virtual void UpdateCLChild();
};