#include "SWIHubServiceSubsystem.h"

#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "JsonObjectConverter.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "TimerManager.h"
#include "WebSocketsModule.h"
#include "Engine/World.h"

static FString TrimSlashEnd(const FString& In)
{
	FString S = In;
	while (S.EndsWith(TEXT("/")))
	{
		S.LeftChopInline(1);
	}
	return S;
}

static bool TryGetNumberAsFloat(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, float& Out)
{
	double D = 0.0;
	if (!Root.IsValid()) return false;
	if (Root->TryGetNumberField(Key, D))
	{
		Out = static_cast<float>(D);
		return true;
	}
	return false;
}

bool USWIHubClientSubsystem::TryParseDeviceInfo(const TSharedPtr<FJsonObject>& Root, FSWIHubDeviceInfo& Out)
{
	if (!Root.IsValid()) return false;

	Root->TryGetStringField(TEXT("uid"), Out.Uid);
	Root->TryGetStringField(TEXT("name"), Out.Name);
	Root->TryGetStringField(TEXT("role"), Out.Role);
	Root->TryGetStringField(TEXT("remote"), Out.Remote);

	return !Out.Uid.IsEmpty() || !Out.Role.IsEmpty();
}

bool USWIHubClientSubsystem::TryParseImuFrame(const TSharedPtr<FJsonObject>& Root, FSWIHubImuFrame& Out)
{
	if (!Root.IsValid()) return false;

	Root->TryGetStringField(TEXT("match_id"), Out.MatchId);
	Root->TryGetStringField(TEXT("matchId"), Out.MatchId); // 서버 키 흔들릴 때 대비
	Root->TryGetStringField(TEXT("uid"), Out.Uid);
	Root->TryGetStringField(TEXT("name"), Out.Name);

	Root->TryGetNumberField(TEXT("ts_ms"), Out.TsMs);
	Root->TryGetNumberField(TEXT("tsMs"), Out.TsMs);
	Root->TryGetNumberField(TEXT("ts"), Out.TsMs);

	TryGetNumberAsFloat(Root, TEXT("yaw"), Out.Yaw);
	TryGetNumberAsFloat(Root, TEXT("pitch"), Out.Pitch);
	TryGetNumberAsFloat(Root, TEXT("roll"), Out.Roll);

	TryGetNumberAsFloat(Root, TEXT("ax"), Out.Ax);
	TryGetNumberAsFloat(Root, TEXT("ay"), Out.Ay);
	TryGetNumberAsFloat(Root, TEXT("az"), Out.Az);

	TryGetNumberAsFloat(Root, TEXT("gx"), Out.Gx);
	TryGetNumberAsFloat(Root, TEXT("gy"), Out.Gy);
	TryGetNumberAsFloat(Root, TEXT("gz"), Out.Gz);

	Root->TryGetNumberField(TEXT("fire"), Out.Fire);

	return !Out.Uid.IsEmpty();
}

void USWIHubClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogTemp, Log, TEXT("[HUB] Subsystem Initialize"));

	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &ThisClass::HandlePostLoadMap
	);

	// 자동 시작(원치 않으면 이 줄 지우고 BP/코드에서 StartHub 호출)
	StartHub();
}

void USWIHubClientSubsystem::Deinitialize()
{
	UE_LOG(LogTemp, Log, TEXT("[HUB] Subsystem Deinitialize"));

	StopHub();

	if (PostLoadMapHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapHandle);
		PostLoadMapHandle.Reset();
	}

	Super::Deinitialize();
}

void USWIHubClientSubsystem::StartHub()
{
	if (bStarted) return;
	bStarted = true;

	UE_LOG(LogTemp, Log, TEXT("[HUB] StartHub"));

	UWorld* World = GetWorld();
	if (IsValidGameWorld(World))
	{
		ActiveWorld = World;
		StartPolling();

		if (!bConnectOnlyWhenPhonePresent)
		{
			ConnectWs();
		}
	}
}

void USWIHubClientSubsystem::StopHub()
{
	if (!bStarted) return;
	bStarted = false;

	UE_LOG(LogTemp, Log, TEXT("[HUB] StopHub"));

	StopPolling();
	DisconnectWs();

	LastPhoneCount = -1;
	ActiveWorld.Reset();
}

bool USWIHubClientSubsystem::IsValidGameWorld(UWorld* World) const
{
	return World && World->IsGameWorld() && !World->bIsTearingDown;
}

void USWIHubClientSubsystem::HandlePostLoadMap(UWorld* World)
{
	if (!bStarted) return;
	if (!IsValidGameWorld(World)) return;

	// PIE/맵전환에서도 “게임 월드” 기준으로 ActiveWorld 갱신
	ActiveWorld = World;

	UE_LOG(LogTemp, Log, TEXT("[HUB] PostLoadMapWithWorld: %s"), *World->GetName());

	StartPolling();

	if (!bConnectOnlyWhenPhonePresent && !bWsConnected)
	{
		ConnectWs();
	}
}

void USWIHubClientSubsystem::StartPolling()
{
	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	auto& TM = World->GetTimerManager();
	if (TM.IsTimerActive(PollTimer)) return;

	// 즉시 1회 실행 후 주기 실행
	PollDevices();
	TM.SetTimer(PollTimer, this, &ThisClass::PollDevices, PollIntervalSec, true);

	UE_LOG(LogTemp, Log, TEXT("[HUB] Polling started: %0.2fs"), PollIntervalSec);
}

void USWIHubClientSubsystem::StopPolling()
{
	UWorld* World = ActiveWorld.Get();
	if (!World) return;

	World->GetTimerManager().ClearTimer(PollTimer);
	World->GetTimerManager().ClearTimer(ReconnectTimer);
}

void USWIHubClientSubsystem::PollDevices()
{
	if (!bStarted) return;

	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	const FString Base = TrimSlashEnd(HubHttpBaseUrl);
	const FString Url = Base + TEXT("/stats");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(Url);
	Req->SetTimeout(2.0f);

	Req->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bOk)
		{
			if (!bStarted) return;

			if (!bOk || !Response.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats failed"));
				return;
			}

			const FString Body = Response->GetContentAsString();

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats JSON parse failed"));
				return;
			}

			int32 PhoneCount = 0;

			const TArray<TSharedPtr<FJsonValue>>* ClientsArr = nullptr;
			if (Root->TryGetArrayField(TEXT("clients"), ClientsArr) && ClientsArr)
			{
				for (const TSharedPtr<FJsonValue>& V : *ClientsArr)
				{
					const TSharedPtr<FJsonObject>* O = nullptr;
					if (!V.IsValid() || !V->TryGetObject(O) || !O || !O->IsValid()) continue;

					FString Role;
					(*O)->TryGetStringField(TEXT("role"), Role);
					if (Role.Equals(TEXT("phone"), ESearchCase::IgnoreCase))
					{
						PhoneCount++;
					}
				}
			}

			if (PhoneCount != LastPhoneCount)
			{
				UE_LOG(LogTemp, Log, TEXT("[HUB] phone_count=%d (prev=%d)"), PhoneCount, LastPhoneCount);
				LastPhoneCount = PhoneCount;
			}

			if (bConnectOnlyWhenPhonePresent)
			{
				if (PhoneCount > 0 && !bWsConnected)
				{
					ConnectWs();
				}
				else if (PhoneCount == 0 && bWsConnected)
				{
					DisconnectWs();
				}
			}
		});

	Req->ProcessRequest();
}

FString USWIHubClientSubsystem::BuildWsUrl() const
{
	if (!HubWsUrlOverride.IsEmpty())
	{
		return HubWsUrlOverride;
	}

	FString Base = TrimSlashEnd(HubHttpBaseUrl);

	// http(s) -> ws(s) 변환
	if (Base.StartsWith(TEXT("https://")))
	{
		Base = TEXT("wss://") + Base.Mid(8);
	}
	else if (Base.StartsWith(TEXT("http://")))
	{
		Base = TEXT("ws://") + Base.Mid(7);
	}
	// 이미 ws:// or wss:// 라면 그대로

	const FString EncUid = FGenericPlatformHttp::UrlEncode(ClientUid);
	const FString EncName = FGenericPlatformHttp::UrlEncode(ClientName);

	return FString::Printf(TEXT("%s/ws?role=ue&uid=%s&name=%s"), *Base, *EncUid, *EncName);
}

void USWIHubClientSubsystem::ConnectWs()
{
	if (!bStarted) return;
	if (bWsConnected) return;

	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	World->GetTimerManager().ClearTimer(ReconnectTimer);

	// 모듈 로드(이게 제일 안전)
	FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");

	const FString WsUrl = BuildWsUrl();
	UE_LOG(LogTemp, Log, TEXT("[HUB] WS connect try: %s"), *WsUrl);

	Socket = FWebSocketsModule::Get().CreateWebSocket(WsUrl);

	Socket->OnConnected().AddLambda([this]()
		{
			bWsConnected = true;
			UE_LOG(LogTemp, Log, TEXT("[HUB] WS Connected"));

			// UE 쪽 hello (서버가 필요하면)
			if (Socket.IsValid())
			{
				Socket->Send(TEXT("{\"type\":\"hello\",\"role\":\"ue\"}"));
			}
		});

	Socket->OnConnectionError().AddLambda([this](const FString& Error)
		{
			bWsConnected = false;
			UE_LOG(LogTemp, Error, TEXT("[HUB] WS ConnectionError: %s"), *Error);
			ScheduleReconnect();
		});

	Socket->OnClosed().AddLambda([this](int32 Code, const FString& Reason, bool bWasClean)
		{
			bWsConnected = false;
			UE_LOG(LogTemp, Warning, TEXT("[HUB] WS Closed code=%d reason=%s clean=%d"), Code, *Reason, bWasClean ? 1 : 0);
			ScheduleReconnect();
		});

	Socket->OnMessage().AddLambda([this](const FString& Msg)
		{
			UE_LOG(LogTemp, Log, TEXT("[HUB] WS OnMessage RAW: %s"), *Msg);

			AsyncTask(ENamedThreads::GameThread, [this, Msg]()
				{
					HandleWsMessage_GameThread(Msg);
				});
		});

	Socket->Connect();
}

void USWIHubClientSubsystem::DisconnectWs()
{
	UWorld* World = ActiveWorld.Get();
	if (World)
	{
		World->GetTimerManager().ClearTimer(ReconnectTimer);
	}

	if (Socket.IsValid())
	{
		Socket->Close();
		Socket.Reset();
	}

	bWsConnected = false;
	UE_LOG(LogTemp, Log, TEXT("[HUB] WS Disconnected"));
}

void USWIHubClientSubsystem::ScheduleReconnect()
{
	if (!bStarted) return;

	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	// 폰 있을 때만 붙는 모드면, 폰이 없으면 재접속 안 함
	if (bConnectOnlyWhenPhonePresent && LastPhoneCount <= 0)
	{
		return;
	}

	auto& TM = World->GetTimerManager();
	if (TM.IsTimerActive(ReconnectTimer)) return;

	TM.SetTimer(ReconnectTimer, this, &ThisClass::ConnectWs, ReconnectDelaySec, false);
	UE_LOG(LogTemp, Log, TEXT("[HUB] WS Reconnect scheduled in %0.2fs"), ReconnectDelaySec);
}

void USWIHubClientSubsystem::HandleWsMessage_GameThread(const FString& Msg)
{
	OnRawMessage.Broadcast(Msg);

	// device_connected 같은 메시지를 받을 예정이면 여기서 파싱
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Msg);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	FString Type;
	Root->TryGetStringField(TEXT("type"), Type);

	if (Type.Equals(TEXT("imu"), ESearchCase::IgnoreCase) ||
		Type.Equals(TEXT("imu_frame"), ESearchCase::IgnoreCase) ||
		Type.Equals(TEXT("imuFrame"), ESearchCase::IgnoreCase))
	{
		FSWIHubImuFrame Frame;
		if (TryParseImuFrame(Root, Frame))
		{
			OnImuFrame.Broadcast(Frame);
		}
		return;
	}

	if (Type == TEXT("device_connected"))
	{
		FSWIHubDeviceInfo D;
		Root->TryGetStringField(TEXT("uid"), D.Uid);
		Root->TryGetStringField(TEXT("name"), D.Name);
		Root->TryGetStringField(TEXT("role"), D.Role);
		Root->TryGetStringField(TEXT("remote"), D.Remote);
		OnDeviceConnected.Broadcast(D);
	}
	else if (Type == TEXT("device_disconnected"))
	{
		FSWIHubDeviceInfo D;
		Root->TryGetStringField(TEXT("uid"), D.Uid);
		Root->TryGetStringField(TEXT("name"), D.Name);
		Root->TryGetStringField(TEXT("role"), D.Role);
		Root->TryGetStringField(TEXT("remote"), D.Remote);
		OnDeviceDisconnected.Broadcast(D);
	}
}
