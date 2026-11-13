#pragma once

#include "Commandlets/Commandlet.h"
#include "UAssetAgentCommandlet.generated.h"

UCLASS()
class UASSETAGENT_API UUAssetAgentCommandlet : public UCommandlet
{
    GENERATED_BODY()
public:
    UUAssetAgentCommandlet();
    virtual int32 Main(const FString& Params) override;
};
