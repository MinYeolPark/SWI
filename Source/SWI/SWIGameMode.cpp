#include "SWIGameMode.h"
#include "SWI/Subsystems/SWIHubServiceSubsystem.h"

ASWIGameMode::ASWIGameMode()
{
}

void ASWIGameMode::BeginPlay()
{
	Super::BeginPlay();

	if (!HasAuthority()) return;

	if(UGameInstance* GI = GetGameInstance())
	{
		//Hub = GI->GetSubsystem<USWIHubClientSubsystem>();
		//if (Hub)
		//{
		//	Hub->OnDeviceConnected().AddUObject(this, &ThisClass::HandleDeviceConnected);
		//	Hub->OnDeviceDisconnected().AddUObject(this, &ThisClass::HandleDeviceDisconnected);
		//}
	}
}

void ASWIGameMode::ReportMatchResultToHub(const FString& WinnerUid, const TArray<FSWIHubPlayerResultRow>& Results, int32 DurationSec)
{
}

void ASWIGameMode::HandleDeviceConnected(const FSWIHubDeviceInfo& Device)
{
}

void ASWIGameMode::HandleDeviceDisconnected(const FSWIHubDeviceInfo& Device)
{
}
