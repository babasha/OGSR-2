#pragma once

#include "../../xrCore/XML_Parser/xrXMLParser.h"

class CUIXml : public CXml
{
public:
    CUIXml();
    virtual ~CUIXml();

    virtual shared_str correct_file_name(LPCSTR path, LPCSTR fn);
};