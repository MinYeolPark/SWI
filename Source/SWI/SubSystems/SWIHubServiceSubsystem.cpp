#include "SWIHubServiceSubsystem.h"
#include "Async/Async.h"
#include "Dom/JsonObject.h"
#include "GenericPlatform/GenericPlatformHttp.h"
#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
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

bool USWIHubClientSubsystem::TryGetNumberAsFloat(const TSharedPtr<FJsonObject>& Root, const TCHAR* Key, float& Out)
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

bool USWIHubClientSubsystem::TryParseDeviceInfo(const TSharedPtr<FJsonObject>& Root, FSWIHubDeviceInfo& Out) const
{
	if (!Root.IsValid()) return false;

	Root->TryGetStringField(TEXT("uid"), Out.Uid);
	Root->TryGetStringField(TEXT("name"), Out.Name);
	Root->TryGetStringField(TEXT("role"), Out.Role);
	Root->TryGetStringField(TEXT("remote"), Out.Remote);

	return !Out.Uid.IsEmpty() || !Out.Role.IsEmpty();
}

bool USWIHubClientSubsystem::TryParseImuFrame(const TSharedPtr<FJsonObject>& Root, FSWIHubImuFrame& Out) const
{
	if (!Root.IsValid()) return false;

	Root->TryGetStringField(TEXT("match_id"), Out.MatchId);
	Root->TryGetStringField(TEXT("matchId"), Out.MatchId);
	Root->TryGetStringField(TEXT("uid"), Out.Uid);
	Root->TryGetStringField(TEXT("name"), Out.Name);

	double Ts = 0.0;
	if (Root->TryGetNumberField(TEXT("ts_ms"), Ts)) Out.TsMs = Ts;
	if (Root->TryGetNumberField(TEXT("tsMs"), Ts))  Out.TsMs = Ts;
	if (Root->TryGetNumberField(TEXT("ts"), Ts))    Out.TsMs = Ts;

	TryGetNumberAsFloat(Root, TEXT("yaw"), Out.Yaw);
	TryGetNumberAsFloat(Root, TEXT("pitch"), Out.Pitch);
	TryGetNumberAsFloat(Root, TEXT("roll"), Out.Roll);

	TryGetNumberAsFloat(Root, TEXT("ax"), Out.Ax);
	TryGetNumberAsFloat(Root, TEXT("ay"), Out.Ay);
	TryGetNumberAsFloat(Root, TEXT("az"), Out.Az);

	TryGetNumberAsFloat(Root, TEXT("gx"), Out.Gx);
	TryGetNumberAsFloat(Root, TEXT("gy"), Out.Gy);
	TryGetNumberAsFloat(Root, TEXT("gz"), Out.Gz);

	int32 Fire = 0;
	Root->TryGetNumberField(TEXT("fire"), Fire);
	Out.Fire = Fire;

	return !Out.Uid.IsEmpty();
}

void USWIHubClientSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	UE_LOG(LogTemp, Log, TEXT("[HUB] Subsystem Initialize"));

	PostLoadMapHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this, &ThisClass::HandlePostLoadMap
	);

	if (bAutoStart)
	{
		StartHub();
	}
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

bool USWIHubClientSubsystem::IsValidGameWorld(UWorld* World) const
{
	return World && World->IsGameWorld() && !World->bIsTearingDown;
}

void USWIHubClientSubsystem::HandlePostLoadMap(UWorld* World)
{
	if (!bStarted) return;
	if (!IsValidGameWorld(World)) return;

	ActiveWorld = World;

	UE_LOG(LogTemp, Log, TEXT("[HUB] PostLoadMapWithWorld: %s"), *World->GetName());

	if (bUseStatsPolling)
	{
		StartPolling();
	}

	if (!bWsConnected)
	{
		ConnectWs();
	}
}

void USWIHubClientSubsystem::StartHub()
{
	if (bStarted) return;
	bStarted = true;

	UWorld* World = GetWorld();
	if (IsValidGameWorld(World))
	{
		ActiveWorld = World;

		if (bUseStatsPolling)
		{
			StartPolling();
		}

		ConnectWs();
	}

	UE_LOG(LogTemp, Log, TEXT("[HUB] StartHub (Polling=%d)"), bUseStatsPolling ? 1 : 0);
}

void USWIHubClientSubsystem::StopHub()
{
	if (!bStarted) return;
	bStarted = false;

	StopPolling();
	DisconnectWs();

	LastPhoneCount = -1;
	ActiveWorld.Reset();

	UE_LOG(LogTemp, Log, TEXT("[HUB] StopHub"));
}

void USWIHubClientSubsystem::StartPolling()
{
	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	auto& TM = World->GetTimerManager();
	if (TM.IsTimerActive(PollTimer)) return;

	// 즉시 1회
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
	if (!bUseStatsPolling) return;
	if (!bStatsEndpointAvailable) return;

	UWorld* World = ActiveWorld.Get();
	if (!IsValidGameWorld(World)) return;

	const FString Base = TrimSlashEnd(HubHttpBaseUrl);
	const FString Url = Base + TEXT("/stats");

	TSharedRef<IHttpRequest, ESPMode::ThreadSafe> Req = FHttpModule::Get().CreateRequest();
	Req->SetVerb(TEXT("GET"));
	Req->SetURL(Url);
	Req->SetTimeout(2.0f);

	Req->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr, FHttpResponsePtr Response, bool bOk)
		{
			if (!bStarted) return;

			if (!bOk || !Response.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats failed (no response)"));
				return;
			}

			const int32 Code = Response->GetResponseCode();
			const FString Body = Response->GetContentAsString();

			if (Code != 200)
			{
				UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats http=%d (body='%s')"), Code, *Body.Left(64));

				if (Code == 404)
				{
					bStatsEndpointAvailable = false;
					UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats not available on server. Disable stats polling (WS-only)."));
					StopPolling();
				}
				return;
			}

			TSharedPtr<FJsonObject> Root;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
			{
				UE_LOG(LogTemp, Warning, TEXT("[HUB] /stats JSON parse failed (body='%s')"), *Body.Left(64));
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

	if (Base.StartsWith(TEXT("https://")))
	{
		Base = TEXT("wss://") + Base.Mid(8);
	}
	else if (Base.StartsWith(TEXT("http://")))
	{
		Base = TEXT("ws://") + Base.Mid(7);
	}

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

	FModuleManager::LoadModuleChecked<FWebSocketsModule>("WebSockets");

	const FString WsUrl = BuildWsUrl();
	UE_LOG(LogTemp, Log, TEXT("[HUB] WS connect try: %s"), *WsUrl);

	Socket = FWebSocketsModule::Get().CreateWebSocket(WsUrl);

	Socket->OnConnected().AddLambda([this]()
		{
			bWsConnected = true;
			UE_LOG(LogTemp, Log, TEXT("[HUB] WS Connected"));

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

	auto& TM = World->GetTimerManager();
	if (TM.IsTimerActive(ReconnectTimer)) return;

	TM.SetTimer(ReconnectTimer, this, &ThisClass::ConnectWs, ReconnectDelaySec, false);
	UE_LOG(LogTemp, Log, TEXT("[HUB] WS Reconnect scheduled in %0.2fs"), ReconnectDelaySec);
}

void USWIHubClientSubsystem::HandleWsMessage_GameThread(const FString& Msg)
{
	OnRawMessage.Broadcast(Msg);

	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Msg);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return;
	}

	FString Type;
	Root->TryGetStringField(TEXT("type"), Type);

	if (Type.Equals(TEXT("imu"), ESearchCase::IgnoreCase))
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
		if (TryParseDeviceInfo(Root, D))
		{
			OnDeviceConnected.Broadcast(D);
		}
		return;
	}

	if (Type == TEXT("device_disconnected"))
	{
		FSWIHubDeviceInfo D;
		if (TryParseDeviceInfo(Root, D))
		{
			OnDeviceDisconnected.Broadcast(D);
		}
		return;
	}

	if (Type == TEXT("device_list"))
	{
		const TArray<TSharedPtr<FJsonValue>>* DevicesArr = nullptr;
		int32 PhoneCount = 0;

		if (Root->TryGetArrayField(TEXT("devices"), DevicesArr) && DevicesArr)
		{
			for (const TSharedPtr<FJsonValue>& V : *DevicesArr)
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
			LastPhoneCount = PhoneCount;
			UE_LOG(LogTemp, Log, TEXT("[HUB] device_list phone_count=%d"), PhoneCount);
		}
	}
}
