#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "SWI/Subsystems/SWIHubServiceSubsystem.h"
#include "SWIGyroInputReceiverComponent.generated.h"

DECLARE_DYNAMIC_MULTICAST_DELEGATE(FOnSWIFire);

UCLASS(ClassGroup = (Custom), meta = (BlueprintSpawnableComponent))
class SWI_API USWIGyroInputReceiverComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	USWIGyroInputReceiverComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	bool GetIAValues(FVector2D& OutMove, FVector2D& OutLook) const;

	UPROPERTY(EditAnywhere, Category = "Gyro|Device")
	float DisconnectTimeoutSec = 0.25f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveMaxTiltDeg = 18.0f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveDeadZone = 0.08f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveSmoothingHz = 12.0f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveRightSign = 1.0f; 

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveForwardSign = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	bool bPreferGyroRate = true;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	float LookYawScale = 1.8f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	float LookPitchScale = 1.2f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	bool bInvertLookPitch = true;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	float LookSmoothingHz = 18.0f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Look")
	float MaxLookDeltaPerFrame = 8.0f;

	UPROPERTY(BlueprintAssignable, Category = "Gyro|Fire")
	FOnSWIFire OnSWIFire;

private:
	UPROPERTY()
	TObjectPtr<USWIHubClientSubsystem> Hub = nullptr;

	FVector2D CurrentMove = FVector2D::ZeroVector;
	FVector2D CurrentLook = FVector2D::ZeroVector;

	FVector2D SmoothedMove = FVector2D::ZeroVector;
	FVector2D SmoothedLook = FVector2D::ZeroVector;

	double LastImuRecvRealTime = 0.0;
	bool bConnected = false;

	bool bHasNeutral = false;
	float NeutralPitchDeg = 0.f;
	float NeutralRollDeg = 0.f;

	bool bHasPrevAngles = false;
	float PrevYawDeg = 0.f;
	float PrevPitchDeg = 0.f;

	UFUNCTION()
	void HandleImu(const FSWIHubImuFrame& Frame);

	UFUNCTION()
	void HandleDeviceDisconnected(const FSWIHubDeviceInfo& Info);

	void ForceStopPawnNow();

	static float ExpSmoothingAlpha(float DeltaTime, float SmoothingHz);
	static float ApplyDeadZone(float v, float deadZone);
};
