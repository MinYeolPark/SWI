#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SWI/SWIHubProtocolTypes.h"
#include "SWICharacter.generated.h"

class USWISensorReceiverComponent;

UCLASS()
class SWI_API ASWICharacter : public ACharacter
{
	GENERATED_BODY()

public:
	ASWICharacter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

protected:
	TObjectPtr<USWISensorReceiverComponent> SensorReceiverComp;
};
