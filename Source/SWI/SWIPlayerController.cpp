#include "SWIPlayerController.h"
#include "SWI/Components/SWISensorReceiverComponent.h"
#include "TimerManager.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"

ASWIPlayerController::ASWIPlayerController()
{
	SensorReceiver = CreateDefaultSubobject<USWISensorReceiverComponent>(TEXT("SensorReceiver"));
}

void ASWIPlayerController::BeginPlay()
{
	Super::BeginPlay();

	SetIgnoreMoveInput(false);
	SetIgnoreLookInput(false);

	if (PlayerCameraManager)
	{
		PlayerCameraManager->ViewPitchMin = MinPitch;
		PlayerCameraManager->ViewPitchMax = MaxPitch;
	}
}

void ASWIPlayerController::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	Super::EndPlay(EndPlayReason);
}

void ASWIPlayerController::CalibrateSensor()
{
	if (SensorReceiver)
	{
		SensorReceiver->CalibrateFromLatest();
	}
}

void ASWIPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	if (!SensorReceiver) return;

	APawn* P = GetPawn();
	if (!P) return;

	// 센서가 “실제로 들어올 때만” 움직이게 하고 싶다 했으니:
	// 누적값이 0이면 아무 것도 안 함
	FVector2D LookDeltaDeg, MoveAxis;
	int32 Fire = 0;
	if (!SensorReceiver->Consume(LookDeltaDeg, MoveAxis, Fire))
	{
		return;
	}

	// ===== Move =====
	const FRotator CR = GetControlRotation();
	const FRotator YawOnly(0.f, CR.Yaw, 0.f);

	const FVector Forward = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X);
	const FVector Right = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y);

	P->AddMovementInput(Forward, MoveAxis.X);
	P->AddMovementInput(Right, MoveAxis.Y);

	// ===== Look =====
	AddYawInput(LookDeltaDeg.X);
	AddPitchInput(LookDeltaDeg.Y);

	if (bDebug)
	{
		UE_LOG(LogTemp, Warning, TEXT("[SWI-PC] Move(%.3f,%.3f) Look(%.3f,%.3f)"),
			MoveAxis.X, MoveAxis.Y, LookDeltaDeg.X, LookDeltaDeg.Y
		);
	}

	// Fire -> 무기/Ability 연결
	// if (Fire) { ... }
}