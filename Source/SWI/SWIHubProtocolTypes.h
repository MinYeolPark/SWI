#pragma once

#include "CoreMinimal.h"
#include "SWIHubProtocolTypes.generated.h"


USTRUCT(BlueprintType)
struct FSWIHubDeviceInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FString Uid;
    UPROPERTY(BlueprintReadOnly) FString Name;
    UPROPERTY(BlueprintReadOnly) FString Role;
    UPROPERTY(BlueprintReadOnly) FString Remote;
};

USTRUCT(BlueprintType)
struct FHubPlayerInfo
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FString Uid;
    UPROPERTY(BlueprintReadOnly) FString Name;
};

USTRUCT(BlueprintType)
struct FHubMatchStart
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) double ServerTs = 0.0;
    UPROPERTY(BlueprintReadOnly) FString MatchId;
    UPROPERTY(BlueprintReadOnly) TArray<FHubPlayerInfo> Players;
};

USTRUCT(BlueprintType)
struct FSWIHubImuFrame
{
    GENERATED_BODY()

    UPROPERTY(BlueprintReadOnly) FString MatchId;
    UPROPERTY(BlueprintReadOnly) FString Uid;
    UPROPERTY(BlueprintReadOnly) FString Name;

    UPROPERTY(BlueprintReadOnly) double TsMs = 0.0;

    UPROPERTY(BlueprintReadOnly) float Yaw = 0;
    UPROPERTY(BlueprintReadOnly) float Pitch = 0;
    UPROPERTY(BlueprintReadOnly) float Roll = 0;

    UPROPERTY(BlueprintReadOnly) float Ax = 0;
    UPROPERTY(BlueprintReadOnly) float Ay = 0;
    UPROPERTY(BlueprintReadOnly) float Az = 0;

    UPROPERTY(BlueprintReadOnly) float Gx = 0;
    UPROPERTY(BlueprintReadOnly) float Gy = 0;
    UPROPERTY(BlueprintReadOnly) float Gz = 0;

    UPROPERTY(BlueprintReadOnly) int32 Fire = 0;
};

USTRUCT(BlueprintType)
struct FSWIHubPlayerResultRow
{
    GENERATED_BODY()

    UPROPERTY(EditAnywhere, BlueprintReadWrite) FString Uid;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Score = 0;
    UPROPERTY(EditAnywhere, BlueprintReadWrite) int32 Kills = 0;
};
