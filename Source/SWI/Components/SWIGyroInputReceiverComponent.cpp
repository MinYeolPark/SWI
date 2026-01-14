#include "SWIGyroInputReceiverComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

USWIGyroInputReceiverComponent::USWIGyroInputReceiverComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void USWIGyroInputReceiverComponent::BeginPlay()
{
	Super::BeginPlay();

	LastImuRecvRealTime = FPlatformTime::Seconds();

	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		Hub = GI->GetSubsystem<USWIHubClientSubsystem>();
	}

	if (!Hub)
	{
		UE_LOG(LogTemp, Error, TEXT("[GYRO] Hub subsystem is NULL."));
		return;
	}

	Hub->OnImuFrame.AddUniqueDynamic(this, &ThisClass::HandleImu);
	Hub->OnDeviceDisconnected.AddUniqueDynamic(this, &ThisClass::HandleDeviceDisconnected);

	UE_LOG(LogTemp, Log, TEXT("[GYRO] Bound to Hub. Owner=%s"), *GetNameSafe(GetOwner()));
}

void USWIGyroInputReceiverComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Hub)
	{
		Hub->OnImuFrame.RemoveDynamic(this, &ThisClass::HandleImu);
		Hub->OnDeviceDisconnected.RemoveDynamic(this, &ThisClass::HandleDeviceDisconnected);
	}
	Super::EndPlay(EndPlayReason);
}

bool USWIGyroInputReceiverComponent::GetIAValues(FVector2D& OutMove, FVector2D& OutLook) const
{
	if (!bConnected)
	{
		OutMove = FVector2D::ZeroVector;
		OutLook = FVector2D::ZeroVector;
		return false;
	}

	OutMove = CurrentMove;
	OutLook = CurrentLook;
	return true;
}

float USWIGyroInputReceiverComponent::ExpSmoothingAlpha(float DeltaTime, float SmoothingHz)
{
	if (SmoothingHz <= 0.f) return 1.f;
	return 1.f - FMath::Exp(-SmoothingHz * DeltaTime);
}

float USWIGyroInputReceiverComponent::ApplyDeadZone(float v, float deadZone)
{
	const float a = FMath::Abs(v);
	if (a <= deadZone) return 0.f;
	const float sign = FMath::Sign(v);
	const float t = (a - deadZone) / (1.f - deadZone);
	return sign * FMath::Clamp(t, 0.f, 1.f);
}

void USWIGyroInputReceiverComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	const double Now = FPlatformTime::Seconds();
	if (bConnected && (Now - LastImuRecvRealTime) > DisconnectTimeoutSec)
	{
		bConnected = false;
		bHasNeutral = false;
		bHasPrevAngles = false;

		CurrentMove = FVector2D::ZeroVector;
		CurrentLook = FVector2D::ZeroVector;

		UE_LOG(LogTemp, Warning, TEXT("[GYRO] IMU timeout -> stop"));
		ForceStopPawnNow();
	}
}

void USWIGyroInputReceiverComponent::HandleImu(const FSWIHubImuFrame& Frame)
{
	const double Now = FPlatformTime::Seconds();
	LastImuRecvRealTime = Now;
	bConnected = true;

	const float Dt = GetWorld() ? GetWorld()->GetDeltaSeconds() : (1.f / 60.f);

	// ---- MOVE: gravity tilt ----
	const float ax = Frame.Ax;
	const float ay = Frame.Ay;
	const float az = Frame.Az;

	const float RollDeg = FMath::RadiansToDegrees(FMath::Atan2(ax, az));
	const float PitchDeg = FMath::RadiansToDegrees(FMath::Atan2(-ay, FMath::Sqrt(ax * ax + az * az)));

	if (!bHasNeutral)
	{
		NeutralRollDeg = RollDeg;
		NeutralPitchDeg = PitchDeg;
		bHasNeutral = true;
	}

	const float DeltaRoll = FMath::Clamp(FMath::FindDeltaAngleDegrees(NeutralRollDeg, RollDeg), -90.f, 90.f);
	const float DeltaPitch = FMath::Clamp(FMath::FindDeltaAngleDegrees(NeutralPitchDeg, PitchDeg), -90.f, 90.f);

	float Forward = FMath::Clamp((DeltaPitch / MoveMaxTiltDeg) * MoveForwardSign, -1.f, 1.f);
	float Right = FMath::Clamp((DeltaRoll / MoveMaxTiltDeg) * MoveRightSign, -1.f, 1.f);

	Forward = ApplyDeadZone(Forward, MoveDeadZone);
	Right = ApplyDeadZone(Right, MoveDeadZone);

	const float MoveA = ExpSmoothingAlpha(Dt, MoveSmoothingHz);
	SmoothedMove = FMath::Lerp(SmoothedMove, FVector2D(Forward, Right), MoveA);

	// ---- LOOK ----
	float RawYawDeltaDeg = 0.f;
	float RawPitchDeltaDeg = 0.f;

	if (bPreferGyroRate)
	{
		RawYawDeltaDeg = Frame.Gz * Dt;
		RawPitchDeltaDeg = Frame.Gy * Dt;
	}
	else
	{
		const float CurrYaw = Frame.Yaw;
		const float CurrPitch = Frame.Pitch;

		if (!bHasPrevAngles)
		{
			PrevYawDeg = CurrYaw;
			PrevPitchDeg = CurrPitch;
			bHasPrevAngles = true;
		}

		RawYawDeltaDeg = FMath::FindDeltaAngleDegrees(PrevYawDeg, CurrYaw);
		RawPitchDeltaDeg = FMath::FindDeltaAngleDegrees(PrevPitchDeg, CurrPitch);

		PrevYawDeg = CurrYaw;
		PrevPitchDeg = CurrPitch;
	}

	if (bInvertLookPitch) RawPitchDeltaDeg *= -1.f;

	RawYawDeltaDeg = FMath::Clamp(RawYawDeltaDeg, -MaxLookDeltaPerFrame, MaxLookDeltaPerFrame);
	RawPitchDeltaDeg = FMath::Clamp(RawPitchDeltaDeg, -MaxLookDeltaPerFrame, MaxLookDeltaPerFrame);

	const float LookA = ExpSmoothingAlpha(Dt, LookSmoothingHz);
	SmoothedLook = FMath::Lerp(
		SmoothedLook,
		FVector2D(RawYawDeltaDeg * LookYawScale, RawPitchDeltaDeg * LookPitchScale),
		LookA
	);

	CurrentMove = SmoothedMove;
	CurrentLook = SmoothedLook;

	const bool bFire = Frame.Fire ? true : false;
	if(bFire)
	{
		OnSWIFire.Broadcast();
	}

	static double LastPrint = 0.0;
	if ((Now - LastPrint) > 0.5)
	{
		LastPrint = Now;
		UE_LOG(LogTemp, Log, TEXT("[GYRO] Move(%.2f,%.2f) Look(%.2f,%.2f) ax=%.2f ay=%.2f az=%.2f gz=%.2f gy=%.2f"),
			CurrentMove.X, CurrentMove.Y, CurrentLook.X, CurrentLook.Y, ax, ay, az, Frame.Gz, Frame.Gy);
	}
}

void USWIGyroInputReceiverComponent::HandleDeviceDisconnected(const FSWIHubDeviceInfo& Info)
{
	bConnected = false;
	bHasNeutral = false;
	bHasPrevAngles = false;

	CurrentMove = FVector2D::ZeroVector;
	CurrentLook = FVector2D::ZeroVector;

	UE_LOG(LogTemp, Warning, TEXT("[GYRO] device_disconnected -> stop"));
	ForceStopPawnNow();
}

void USWIGyroInputReceiverComponent::ForceStopPawnNow()
{
	APawn* Pawn = nullptr;

	if (AController* C = Cast<AController>(GetOwner()))
	{
		Pawn = C->GetPawn();
	}
	else
	{
		Pawn = Cast<APawn>(GetOwner());
	}

	if (ACharacter* Char = Cast<ACharacter>(Pawn))
	{
		if (UCharacterMovementComponent* Move = Char->GetCharacterMovement())
		{
			Move->StopMovementImmediately();
		}
	}
}
