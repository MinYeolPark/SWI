#include "SWICharacter.h"

#include "SWI/Components/SWISensorReceiverComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "SWI/SWIHubProtocolTypes.h"

ASWICharacter::ASWICharacter()
{
	PrimaryActorTick.bCanEverTick = true;
}

void ASWICharacter::BeginPlay()
{
	Super::BeginPlay();	
}


void ASWICharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);	
}