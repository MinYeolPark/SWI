#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "SWI/SWIHubProtocolTypes.h"
#include "SWICharacter.generated.h"

class USWISensorReceiverComponent;
class UAbilitySystemComponent;
class UAttributeSet;
class UGameplayEffect;
class UGameplayAbility;

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

protected:
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY()
	TObjectPtr<UAttributeSet> AttributeSet;

	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TSubclassOf<UGameplayEffect> DefaultAttributesGE;

	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TArray<TSubclassOf<UGameplayAbility>> StartupAbilities;
};
