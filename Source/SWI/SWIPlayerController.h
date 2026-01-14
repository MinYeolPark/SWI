#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "SWIPlayerController.generated.h"

class USWIGyroInputReceiverComponent;

UCLASS()
class SWI_API ASWIPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ASWIPlayerController();

	virtual void BeginPlay() override;
	virtual void PlayerTick(float DeltaTime) override;

protected:
	UPROPERTY(EditAnywhere, Category = "Gyro|Refs")
	TObjectPtr<USWIGyroInputReceiverComponent> GyroReceiver = nullptr;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	float MoveScale = 1.0f;

	UPROPERTY(EditAnywhere, Category = "Gyro|Move")
	bool bMoveByControlYaw = true;

	void ApplyMoveAxis(APawn* ControlledPawn, const FVector2D& MoveAxis);
	void ApplyLookAxis(const FVector2D& LookAxis);
};
