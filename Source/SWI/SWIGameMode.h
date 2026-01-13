#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "SWI/SWIHubProtocolTypes.h"
#include "SWIGameMode.generated.h"


UCLASS()
class SWI_API ASWIGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
    ASWIGameMode();

    virtual void BeginPlay() override;

    UFUNCTION(BlueprintCallable, Category = "Hub|Match")
    void ReportMatchResultToHub(const FString& WinnerUid, const TArray<FSWIHubPlayerResultRow>& Results, int32 DurationSec);

private:
	void HandleDeviceConnected(const FSWIHubDeviceInfo& Device);
	void HandleDeviceDisconnected(const FSWIHubDeviceInfo& Device);

private:
    UPROPERTY()
    class USWIHubClientSubsystem* Hub = nullptr;

 /*   UPROPERTY()
    AHubGameState* HGS = nullptr;*/

    FString CurrentMatchId;
    //TMap<FString, TWeakObjectPtr<APhoneProxyController>> ProxyByUid;
};
