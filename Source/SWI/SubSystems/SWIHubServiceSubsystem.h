#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "SWI/SWIHubProtocolTypes.h"
#include "IWebSocket.h"
#include "SWIHubServiceSubsystem.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubRawMessageSig, const FString&, Raw);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubImuFrameSig, const FSWIHubImuFrame&, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubDeviceSig, const FSWIHubDeviceInfo&, Device);

UCLASS()
class SWI_API USWIHubClientSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	UFUNCTION(BlueprintCallable, Category = "HUB")
	void StartHub();

	UFUNCTION(BlueprintCallable, Category = "HUB")
	void StopHub();

	UPROPERTY(BlueprintAssignable, Category = "HUB")
	FSWIHubRawMessageSig OnRawMessage;

	UPROPERTY(BlueprintAssignable, Category = "HUB")
	FSWIHubImuFrameSig OnImuFrame;

	UPROPERTY(BlueprintAssignable, Category = "HUB")
	FSWIHubDeviceSig OnDeviceConnected;

	UPROPERTY(BlueprintAssignable, Category = "HUB")
	FSWIHubDeviceSig OnDeviceDisconnected;

private:
	// WebSockets
	void ConnectWs();
	void DisconnectWs();
	void ScheduleReconnect();
	FString BuildWsUrl() const;
	// ~WebSockets

	// Polling
	void StartPolling();
	void StopPolling();
	void PollDevices();
	bool IsValidGameWorld(UWorld* World) const;
	void HandlePostLoadMap(UWorld* World);
	// ~Polling
	 
	// Parse Helper
	static bool TryGetNumberAsFloat(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, float& Out);
	bool TryParseDeviceInfo(const TSharedPtr<FJsonObject>& Root, FSWIHubDeviceInfo& Out) const;
	bool TryParseImuFrame(const TSharedPtr<FJsonObject>& Root, FSWIHubImuFrame& Out) const;
	// ~Parse Helper

	// Message
	void HandleWsMessage_GameThread(const FString& Msg);
	// ~Message

private:
	// Settings
	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	FString HubHttpBaseUrl = TEXT("http://127.0.0.1:8080");

	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	FString HubWsUrlOverride;

	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	FString ClientUid = TEXT("ue");

	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	FString ClientName = TEXT("UE");

	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	float ReconnectDelaySec = 1.0f;

	UPROPERTY(EditAnywhere, Category = "HUB|Config")
	bool bAutoStart = true;

	UPROPERTY(EditAnywhere, Category = "HUB|Polling")
	bool bUseStatsPolling = false;

	UPROPERTY(EditAnywhere, Category = "HUB|Polling", meta = (EditCondition = "bUseStatsPolling"))
	float PollIntervalSec = 0.5f;

	bool bStarted = false;
	bool bWsConnected = false;

	bool bStatsEndpointAvailable = true;
	int32 LastPhoneCount = -1;

	TWeakObjectPtr<UWorld> ActiveWorld;
	FTimerHandle PollTimer;
	FTimerHandle ReconnectTimer;
	FDelegateHandle PostLoadMapHandle;

	TSharedPtr<class IWebSocket> Socket;
};
