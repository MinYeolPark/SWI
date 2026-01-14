#include "SWICharacter.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "GameplayAbilitySpec.h"

ASWICharacter::ASWICharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("ASC"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	// AttributeSet도 필요하면 생성
	//AttributeSet = CreateDefaultSubobject<USWIAttributeSet>(TEXT("AttributeSet"));
}

void ASWICharacter::BeginPlay()
{
	Super::BeginPlay();	

	AbilitySystemComponent->InitAbilityActorInfo(this, this);

	FGameplayEffectContextHandle Ctx = AbilitySystemComponent->MakeEffectContext();
	Ctx.AddSourceObject(this);

	FGameplayEffectSpecHandle Spec = AbilitySystemComponent->MakeOutgoingSpec(DefaultAttributesGE, 1.f, Ctx);
	if (Spec.IsValid())
	{
		AbilitySystemComponent->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
	}

	for (const TSubclassOf<UGameplayAbility>& AbilityClass : StartupAbilities)
	{
		if (!AbilityClass) continue;
		AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(AbilityClass, 1));
	}
}


void ASWICharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);	
}