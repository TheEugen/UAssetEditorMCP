#pragma once

#include "Dom/JsonObject.h"
#include "Modules/ModuleInterface.h"

class IUAssetAgentNiagara : public IModuleInterface
{
public:
    virtual void Describe(UObject* Asset, FJsonObject& Response) = 0;
    virtual UObject* RepairInterface(UObject* Asset, FName Parameter, FString& Error) = 0;
    virtual bool CompileSystem(UObject* Asset) = 0;
};
