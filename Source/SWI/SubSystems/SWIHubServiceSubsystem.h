#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "IWebSocket.h"
#include "SWI/SWIHubProtocolTypes.h"

#include "SWIHubServiceSubsystem.generated.h"


DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubRawMessageSig, const FString&, Message);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubImuEvent, const FSWIHubImuFrame&, Frame);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubDeviceConnectedSig, const FSWIHubDeviceInfo&, Device);
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FSWIHubDeviceDisconnectedSig, const FSWIHubDeviceInfo&, Device);

UCLASS()
class SWI_API USWIHubClientSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	// 시작/중지 (원하면 블루프린트에서 호출 가능)
	UFUNCTION(BlueprintCallable, Category = "SWI|Hub")
	void StartHub();

	UFUNCTION(BlueprintCallable, Category = "SWI|Hub")
	void StopHub();

	// HTTP 베이스 (stats 등)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	FString HubHttpBaseUrl = TEXT("http://127.0.0.1:8080");

	// WS URL 강제 오버라이드 (비워두면 HubHttpBaseUrl에서 ws/wss로 변환)
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	FString HubWsUrlOverride;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	FString ClientUid = TEXT("ue1");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	FString ClientName = TEXT("UE");

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	float PollIntervalSec = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	float ReconnectDelaySec = 2.0f;

	// true면: /stats에서 phone이 1대 이상 있을 때만 WS 연결
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Hub")
	bool bConnectOnlyWhenPhonePresent = true;

	UPROPERTY(BlueprintAssignable, Category = "SWI|Hub")
	FSWIHubRawMessageSig OnRawMessage;

	UPROPERTY(BlueprintAssignable, Category = "SWI|Hub")
	FSWIHubDeviceConnectedSig OnDeviceConnected;

	UPROPERTY(BlueprintAssignable, Category = "SWI|Hub")
	FSWIHubDeviceDisconnectedSig OnDeviceDisconnected;

	UPROPERTY(BlueprintAssignable, Category = "SWI|Hub|Event")
	FSWIHubImuEvent OnImuFrame;

	UPROPERTY(BlueprintReadOnly, Category = "SWI|Hub")
	bool bWsConnected = false;

private:
	void HandlePostLoadMap(UWorld* World);
	bool IsValidGameWorld(UWorld* World) const;

	void StartPolling();
	void StopPolling();

	void PollDevices();     // HTTP GET /stats
	void ConnectWs();       // WS connect
	void DisconnectWs();
	void ScheduleReconnect();

	FString BuildWsUrl() const;
	void HandleWsMessage_GameThread(const FString& Msg);

	bool TryParseDeviceInfo(const TSharedPtr<FJsonObject>& Root, FSWIHubDeviceInfo& Out);
	bool TryParseImuFrame(const TSharedPtr<FJsonObject>& Root, FSWIHubImuFrame& Out);

private:
	bool bStarted = false;
	int32 LastPhoneCount = -1;

	TWeakObjectPtr<UWorld> ActiveWorld;

	FTimerHandle PollTimer;
	FTimerHandle ReconnectTimer;

	FDelegateHandle PostLoadMapHandle;

	TSharedPtr<IWebSocket> Socket;
};
