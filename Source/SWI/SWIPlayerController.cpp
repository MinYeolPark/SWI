#include "SWIPlayerController.h"
#include "SWI/Components/SWIGyroInputReceiverComponent.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"

ASWIPlayerController::ASWIPlayerController()
{
	PrimaryActorTick.bCanEverTick = true;
	GyroReceiver = CreateDefaultSubobject<USWIGyroInputReceiverComponent>(TEXT("GyroReceiver"));
}

void ASWIPlayerController::BeginPlay()
{
	Super::BeginPlay();

	SetIgnoreMoveInput(false);
	SetIgnoreLookInput(false);

	UE_LOG(LogTemp, Log, TEXT("[PC] BeginPlay IgnoreMove=%d IgnoreLook=%d Pawn=%s"),
		IsMoveInputIgnored() ? 1 : 0,
		IsLookInputIgnored() ? 1 : 0,
		*GetNameSafe(GetPawn()));
}

void ASWIPlayerController::PlayerTick(float DeltaTime)
{
	Super::PlayerTick(DeltaTime);

	APawn* P = GetPawn();
	if (!P || !GyroReceiver)
	{
		return;
	}

	FVector2D MoveAxis(0, 0), LookAxis(0, 0);
	const bool bHasGyro = GyroReceiver->GetIAValues(MoveAxis, LookAxis);

	// 연결 끊기면 입력 주지 않음 (Receiver가 stop도 해줌)
	if (!bHasGyro)
	{
		return;
	}

	// ✅ Movement가 DisableMovement 상태면 입력을 먹어도 영원히 안 움직입니다.
	if (ACharacter* C = Cast<ACharacter>(P))
	{
		if (UCharacterMovementComponent* M = C->GetCharacterMovement())
		{
			if (M->MovementMode == MOVE_None)
			{
				M->SetMovementMode(MOVE_Walking);
			}
		}
	}

	ApplyMoveAxis(P, MoveAxis);
	ApplyLookAxis(LookAxis);
}

void ASWIPlayerController::ApplyMoveAxis(APawn* ControlledPawn, const FVector2D& MoveAxis)
{
	const float Forward = MoveAxis.X * MoveScale;
	const float Right = MoveAxis.Y * MoveScale;

	if (FMath::IsNearlyZero(Forward) && FMath::IsNearlyZero(Right))
		return;

	FVector ForwardDir, RightDir;

	if (bMoveByControlYaw)
	{
		const FRotator ControlRot = GetControlRotation();
		const FRotator YawOnly(0.f, ControlRot.Yaw, 0.f);
		ForwardDir = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::X);
		RightDir = FRotationMatrix(YawOnly).GetUnitAxis(EAxis::Y);
	}
	else
	{
		ForwardDir = ControlledPawn->GetActorForwardVector();
		RightDir = ControlledPawn->GetActorRightVector();
	}

	ControlledPawn->AddMovementInput(ForwardDir, Forward, /*bForce=*/true);
	ControlledPawn->AddMovementInput(RightDir, Right,   /*bForce=*/true);
}

void ASWIPlayerController::ApplyLookAxis(const FVector2D& LookAxis)
{
	// IgnoreLookInput 켜져있으면 이것도 무시되니 BeginPlay에서 해제했음
	AddYawInput(LookAxis.X);
	AddPitchInput(LookAxis.Y);
}
