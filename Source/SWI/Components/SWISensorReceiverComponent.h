#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SWI/SubSystems/SWIHubServiceSubsystem.h"
#include "SWISensorReceiverComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FSWIOnSensorUpdated);
UCLASS(ClassGroup = (SWI), meta = (BlueprintSpawnableComponent))
class SWI_API USWISensorReceiverComponent : public UActorComponent
{
	GENERATED_BODY()
public:
	USWISensorReceiverComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	UPROPERTY(BlueprintAssignable, Category = "SWI|Sensor")
	FSWIOnSensorUpdated OnSensorUpdated;

	// LookDeltaDeg: 이번 프레임에 적용할 (Yaw,Pitch) 델타(도)
	// MoveAxis: 지속 입력값 (Forward,Right) -1~1
	UFUNCTION(BlueprintCallable, Category = "SWI|Sensor")
	bool Consume(FVector2D& OutLookDeltaDeg, FVector2D& OutMoveAxis, int32& OutFire);

	UFUNCTION(BlueprintCallable, Category = "SWI|Sensor")
	void CalibrateFromLatest();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Sensor")
	FString BoundUid;

	// ===== Move (Tilt) =====
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	float MaxTiltDeg = 18.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	float MoveDeadzone = 0.20f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	float MoveSmoothing = 10.f;

	// 폰 방향 때문에 pitch/roll이 뒤집히면 이거 1개로 해결
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	bool bSwapMoveAxes = false; // true면 Forward=Roll, Right=Pitch

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	bool bInvertForward = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Move")
	bool bInvertRight = false;

	// ===== Look (Gyro deg/s integrate) =====
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	float GyroRateDeadzoneDegPerSec = 12.f; // 떨림 줄이기(상향)

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	float GyroRateSmoothing = 35.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	float GyroYawScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	float GyroPitchScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	float MaxLookDeltaDegPerFrame = 6.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	bool bInvertYaw = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Look")
	bool bInvertPitch = true;

	// ===== Debug =====
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Debug")
	bool bDebug = false;

private:
	UFUNCTION()
	void OnImuFrameReceived(const FSWIHubImuFrame& Frame);

private:
	TWeakObjectPtr<USWIHubClientSubsystem> Hub;

	bool bHasLatest = false;
	FSWIHubImuFrame Latest;

	bool bHasBase = false;
	float BasePitchDeg = 0.f;
	float BaseRollDeg = 0.f;

	double PrevTsMs = 0.0;
	float LastDt = 1.f / 60.f;

	// 지속 이동 입력
	FVector2D MoveState = FVector2D::ZeroVector; // X=Forward, Y=Right

	// Look 누적(센서 프레임 여러 번 와도 프레임에서 한번에 소비)
	FVector2D AccumLookDeltaDeg = FVector2D::ZeroVector;
	int32 PendingFire = 0;

	// Gyro smoothing state (deg/s)
	float SmoothedYawRateDeg = 0.f;
	float SmoothedPitchRateDeg = 0.f;

	double DebugNextPrintSec = 0.0;
};