#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "SWIPlayerController.generated.h"

class USWISensorReceiverComponent;
UCLASS()
class SWI_API ASWIPlayerController : public APlayerController
{
	GENERATED_BODY()

public:
	ASWIPlayerController();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void PlayerTick(float DeltaTime) override;

	UFUNCTION(BlueprintCallable, Category = "SWI|Sensor")
	void CalibrateSensor();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Camera")
	float MinPitch = -60.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Camera")
	float MaxPitch = 20.f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "SWI|Debug")
	bool bDebug = false;

private:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USWISensorReceiverComponent> SensorReceiver;
};
