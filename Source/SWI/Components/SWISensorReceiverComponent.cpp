#include "SWISensorReceiverComponent.h"
#include "SWI/Subsystems/SWIHubServiceSubsystem.h"
#include "Engine/World.h"

namespace
{
	static float Deadzone01(float V, float DZ)
	{
		const float A = FMath::Abs(V);
		if (A <= DZ) return 0.f;
		const float Sign = FMath::Sign(V);
		const float T = (A - DZ) / FMath::Max(1e-6f, (1.f - DZ));
		return Sign * FMath::Clamp(T, 0.f, 1.f);
	}

	static float ExpSmooth(float Current, float Target, float Speed, float Dt)
	{
		const float Alpha = 1.f - FMath::Exp(-Speed * Dt);
		return FMath::Lerp(Current, Target, Alpha);
	}

	static float ClampAbs(float V, float MaxAbs)
	{
		return FMath::Clamp(V, -MaxAbs, MaxAbs);
	}
}

USWISensorReceiverComponent::USWISensorReceiverComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

void USWISensorReceiverComponent::BeginPlay()
{
	Super::BeginPlay();

	if (UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr)
	{
		USWIHubClientSubsystem* Sub = GI->GetSubsystem<USWIHubClientSubsystem>();
		Hub = Sub;
		if (Hub.IsValid())
		{
			Hub->OnImuFrame.AddDynamic(this, &ThisClass::OnImuFrameReceived);
		}
	}
}

void USWISensorReceiverComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (Hub.IsValid())
	{
		Hub->OnImuFrame.RemoveDynamic(this, &ThisClass::OnImuFrameReceived);
	}
	Super::EndPlay(EndPlayReason);
}

void USWISensorReceiverComponent::CalibrateFromLatest()
{
	if (!bHasLatest) return;

	BasePitchDeg = Latest.Pitch;
	BaseRollDeg = Latest.Roll;
	bHasBase = true;

	MoveState = FVector2D::ZeroVector;
	AccumLookDeltaDeg = FVector2D::ZeroVector;
	PendingFire = 0;

	SmoothedYawRateDeg = 0.f;
	SmoothedPitchRateDeg = 0.f;
}

bool USWISensorReceiverComponent::Consume(FVector2D& OutLookDeltaDeg, FVector2D& OutMoveAxis, int32& OutFire)
{
	OutMoveAxis = MoveState;

	OutLookDeltaDeg = AccumLookDeltaDeg;
	OutFire = PendingFire;

	// 한 프레임 소비하면 누적 델타는 리셋
	AccumLookDeltaDeg = FVector2D::ZeroVector;
	PendingFire = 0;

	return (!OutMoveAxis.IsNearlyZero() || !OutLookDeltaDeg.IsNearlyZero() || OutFire != 0);
}

void USWISensorReceiverComponent::OnImuFrameReceived(const FSWIHubImuFrame& Frame)
{
	if (!BoundUid.IsEmpty() && Frame.Uid != BoundUid)
		return;

	Latest = Frame;
	bHasLatest = true;

	// dt (서버는 ms)
	float Dt = 1.f / 60.f;
	if (PrevTsMs > 0.0 && Frame.TsMs > PrevTsMs)
	{
		Dt = (float)((Frame.TsMs - PrevTsMs) / 1000.0);
		Dt = FMath::Clamp(Dt, 1.f / 240.f, 1.f / 10.f);
	}
	PrevTsMs = Frame.TsMs;
	LastDt = Dt;

	if (!bHasBase)
	{
		BasePitchDeg = Frame.Pitch;
		BaseRollDeg = Frame.Roll;
		bHasBase = true;
	}

	// =========================
	// Move (Tilt)
	// =========================
	const float PitchDelta = Frame.Pitch - BasePitchDeg;
	const float RollDelta = Frame.Roll - BaseRollDeg;

	float ForwardSrc = bSwapMoveAxes ? RollDelta : PitchDelta;
	float RightSrc = bSwapMoveAxes ? PitchDelta : RollDelta;

	float RawForward = FMath::Clamp(ForwardSrc / FMath::Max(1.f, MaxTiltDeg), -1.f, 1.f);
	float RawRight = FMath::Clamp(RightSrc / FMath::Max(1.f, MaxTiltDeg), -1.f, 1.f);

	if (bInvertForward) RawForward *= -1.f;
	if (bInvertRight)   RawRight *= -1.f;

	const float TargetF = Deadzone01(RawForward, MoveDeadzone);
	const float TargetR = Deadzone01(RawRight, MoveDeadzone);

	MoveState.X = ExpSmooth(MoveState.X, TargetF, MoveSmoothing, Dt);
	MoveState.Y = ExpSmooth(MoveState.Y, TargetR, MoveSmoothing, Dt);

	// =========================
	// Look (Gyro deg/s integrate)
	// 서버 gyro는 로그상 deg/s로 보임 -> 고정
	// =========================
	float YawRateDeg = Frame.Gz; // 대부분 Z가 yaw
	float PitchRateDeg = Frame.Gy; // 대부분 Y가 pitch

	if (bInvertYaw)   YawRateDeg *= -1.f;
	if (bInvertPitch) PitchRateDeg *= -1.f;

	if (FMath::Abs(YawRateDeg) < GyroRateDeadzoneDegPerSec) YawRateDeg = 0.f;
	if (FMath::Abs(PitchRateDeg) < GyroRateDeadzoneDegPerSec) PitchRateDeg = 0.f;

	SmoothedYawRateDeg = ExpSmooth(SmoothedYawRateDeg, YawRateDeg, GyroRateSmoothing, Dt);
	SmoothedPitchRateDeg = ExpSmooth(SmoothedPitchRateDeg, PitchRateDeg, GyroRateSmoothing, Dt);

	float YawDeltaDeg = SmoothedYawRateDeg * Dt * GyroYawScale;
	float PitchDeltaDeg = SmoothedPitchRateDeg * Dt * GyroPitchScale;

	YawDeltaDeg = ClampAbs(YawDeltaDeg, MaxLookDeltaDegPerFrame);
	PitchDeltaDeg = ClampAbs(PitchDeltaDeg, MaxLookDeltaDegPerFrame);

	// 누적
	AccumLookDeltaDeg.X += YawDeltaDeg;
	AccumLookDeltaDeg.Y += PitchDeltaDeg;
	PendingFire = Frame.Fire;

	// Debug: 0.5초마다 한번
	if (bDebug && GetWorld())
	{
		const double Now = GetWorld()->GetTimeSeconds();
		if (Now >= DebugNextPrintSec)
		{
			DebugNextPrintSec = Now + 0.5;
			UE_LOG(LogTemp, Warning, TEXT("[SWI] Dt=%.3f Move(F=%.3f R=%.3f) Base(P=%.1f R=%.1f) Now(P=%.1f R=%.1f) LookAcc(Y=%.3f P=%.3f) Gyro(gz=%.1f gy=%.1f)"),
				Dt,
				MoveState.X, MoveState.Y,
				BasePitchDeg, BaseRollDeg,
				Frame.Pitch, Frame.Roll,
				AccumLookDeltaDeg.X, AccumLookDeltaDeg.Y,
				Frame.Gz, Frame.Gy
			);
		}
	}

	OnSensorUpdated.Broadcast();
}