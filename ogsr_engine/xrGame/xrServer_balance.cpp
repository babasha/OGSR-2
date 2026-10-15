#include "StdAfx.h"
#include "xrServer.h"

class xrClientData;
class CSE_Abstract;

xrClientData* xrServer::SelectBestClientToMigrateTo(CSE_Abstract* E, BOOL bForceAnother)
{
    return (xrClientData*)SV_Client;
}
