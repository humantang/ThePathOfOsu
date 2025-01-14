// Fill out your copyright notice in the Description page of Project Settings.


#include "PlayerCharacter.h"

#include "EnemyCharacter.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/SpringArmComponent.h"
#include "GameFramework/Controller.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "Interface/InteractableInterface.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetMathLibrary.h"


APlayerCharacter::APlayerCharacter()
{
	// Set size for collision capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// Don't rotate when the controller rotates. Let that just affect the camera.
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true; // Character moves in the direction of input...	
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 500.0f, 0.0f); // ...at this rotation rate

	// Note: For faster iteration times these variables, and many more, can be tweaked in the Character Blueprint
	// instead of recompiling to adjust them
	GetCharacterMovement()->JumpZVelocity = 700.f;
	GetCharacterMovement()->AirControl = 0.35f;
	GetCharacterMovement()->MaxWalkSpeed = 500.f;
	GetCharacterMovement()->MinAnalogWalkSpeed = 20.f;
	GetCharacterMovement()->BrakingDecelerationWalking = 2000.f;
	GetCharacterMovement()->BrakingDecelerationFalling = 1500.0f;

	// Create a camera boom (pulls in towards the player if there is a collision)
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));
	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->TargetArmLength = 250.0f; // The camera follows at this distance behind the character	
	CameraBoom->bUsePawnControlRotation = true; // Rotate the arm based on the controller
	CameraBoom->SetRelativeLocation(FVector(0.f, 0.f, 50.f));
	CameraBoom->SocketOffset = FVector(0.f, 50.f, 0.f);
	CameraBoom->SetUsingAbsoluteRotation(true);

	// Create a follow camera
	FollowCamera = CreateDefaultSubobject<UCameraComponent>(TEXT("FollowCamera"));
	FollowCamera->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	// Attach the camera to the end of the boom and let the boom adjust to match the controller orientation
	FollowCamera->bUsePawnControlRotation = false; // Camera does not rotate relative to arm

	// Note: The skeletal mesh and anim blueprint references on the Mesh component (inherited from Character) 
	// are set in the derived blueprint asset named ThirdPersonCharacter (to avoid direct content references in C++)

	DefaultTargetArmLength = CameraBoom->TargetArmLength;
	DefaultCameraSocketOffset = CameraBoom->SocketOffset;
}

void APlayerCharacter::BeginPlay()
{
	// Call the base class  
	Super::BeginPlay();

	//Add Input Mapping Context
	PlayerController = Cast<APlayerController>(Controller);
	if (PlayerController)
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<
			UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			Subsystem->AddMappingContext(DefaultMappingContext, 0);
		}
	}

	GameInstance = Cast<UOsuGameInstance>(GetGameInstance());

	AnimInstance->OnPlayMontageNotifyBegin.AddDynamic(this, &APlayerCharacter::OnPlayMontageNotifyBegin);

	GetWorldTimerManager().SetTimer(FindInteractableTimerHandle, this,
	                                &APlayerCharacter::FindAndHighlightInteractableObjectNearPlayer, 0.1f, true);

	if (InteractableObjectTypes.IsEmpty())
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red, FString::Printf(
			                                 TEXT("InteractableObjectTypes is empty!, %s"),
			                                 *GetName()));
	}

	WalkSpeed = CharacterMovementComponent->MaxWalkSpeed;
	SetupGunCameraZoomTimeline();
	SetupCrosshairWidget();

	SetAnimationState(EAnimationState::Unarmed);
}

void APlayerCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (IsTargetLocking)
	{
		if (LockTargetEnemy != nullptr)
		{
			FRotator LookAtRotation = UKismetMathLibrary::FindLookAtRotation(
				GetActorLocation(), LockTargetEnemy->GetActorLocation());
			if (PlayerController)
			{
				PlayerController->SetControlRotation(LookAtRotation);
			}
			if (LockTargetEnemy->IsExecutable && LockTargetEnemy->TargetWidget->IsVisible())
			{
				LockTargetEnemy->HideTargetWidget();
			}

			if (!LockTargetEnemy->IsExecutable && !LockTargetEnemy->TargetWidget->IsVisible())
			{
				LockTargetEnemy->ShowTargetWidget();
			}
		}
		if (LockTargetEnemy->IsDead())
		{
			UnlockTarget();
		}
	}
	GunCameraZoomTimeline.TickTimeline(DeltaSeconds);
}


//////////////////////////////////////////////////////////////////////////
// Input

void APlayerCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	// Set up action bindings
	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Jumping
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Started, this, &APlayerCharacter::TryJump);
		EnhancedInputComponent->BindAction(JumpAction, ETriggerEvent::Completed, this, &ACharacter::StopJumping);

		// Moving
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Move);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::OnMoveActionStart);
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Completed, this,
		                                   &APlayerCharacter::OnMoveActionRelease);

		// Looking
		EnhancedInputComponent->BindAction(LookAction, ETriggerEvent::Triggered, this, &APlayerCharacter::Look);

		// Guard or Gun Zoom
		EnhancedInputComponent->BindAction(GuardOrZoomAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryGuardOrZoom);
		EnhancedInputComponent->BindAction(GuardOrZoomAction, ETriggerEvent::Completed, this,
		                                   &APlayerCharacter::TryZoomOut);

		// Attacking
		EnhancedInputComponent->BindAction(AttackAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryAttack);

		EnhancedInputComponent->BindAction(AttackAction, ETriggerEvent::Completed, this,
		                                   &APlayerCharacter::OnAttackActionEnd);

		EnhancedInputComponent->BindAction(TargetLockAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryTargetLock);

		EnhancedInputComponent->BindAction(InteractAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::Interact);

		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::OnSprintStart);

		EnhancedInputComponent->BindAction(SprintAction, ETriggerEvent::Completed, this,
		                                   &APlayerCharacter::OnSprintEnd);

		EnhancedInputComponent->BindAction(CrouchAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryCrouch);

		EnhancedInputComponent->BindAction(DodgeRollAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryDodgeRoll);

		EnhancedInputComponent->BindAction(UseItemAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryUseItem);

		EnhancedInputComponent->BindAction(OsuAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TryOsu);

		EnhancedInputComponent->BindAction(PauseAction, ETriggerEvent::Started, this,
		                                   &APlayerCharacter::TogglePauseGame);
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("Enhanced Input Component not found!"));
	}
}

void APlayerCharacter::Move(const FInputActionValue& Value)
{
	if (!CanMove())
	{
		return;
	}
	// input is a Vector2D
	CurrentMovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// find out which way is forward
		const FRotator Rotation = Controller->GetControlRotation();
		const FRotator YawRotation(0, Rotation.Yaw, 0);

		// get forward vector
		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);

		// get right vector 
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		// add movement 
		AddMovementInput(ForwardDirection, CurrentMovementVector.Y);
		AddMovementInput(RightDirection, CurrentMovementVector.X);
	}
}

void APlayerCharacter::OnMoveActionStart()
{
	IsMovingInputPressing = true;
}

void APlayerCharacter::OnMoveActionRelease()
{
	IsMovingInputPressing = false;
}

void APlayerCharacter::Look(const FInputActionValue& Value)
{
	// input is a Vector2D
	FVector2D LookAxisVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// add yaw and pitch input to controller
		AddControllerYawInput(LookAxisVector.X / 2.0f);
		AddControllerPitchInput(LookAxisVector.Y / 2.0f);
		TurnRate = UKismetMathLibrary::Clamp(LookAxisVector.X, -1, 1);
	}
}

void APlayerCharacter::SearchEnemyInFront(FHitResult& OutHit, bool& IsHit)
{
	FVector StartLocation = GetActorLocation();
	FRotator CameraRotation = FollowCamera->GetComponentRotation();
	FVector EndLocation = StartLocation + CameraRotation.Vector() * 1300.f;
	EndLocation.Z = StartLocation.Z;
	TArray<AActor*> IgnoredActors;
	IgnoredActors.Add(this);
	IsHit = UKismetSystemLibrary::SphereTraceSingleForObjects(GetWorld(), StartLocation, EndLocation, 500.f,
	                                                          TraceEnemyObjectTypes, false, IgnoredActors,
	                                                          EDrawDebugTrace::None, OutHit, true,
	                                                          FLinearColor::Red, FLinearColor::Green, 15.f);
}

void APlayerCharacter::TryJump()
{
	if (!CanCharacterJump())
	{
		return;
	}
	Jump();
}

void APlayerCharacter::Interact()
{
	if (FocusActor)
	{
		IInteractableInterface* InteractableInterface = Cast<IInteractableInterface>(FocusActor);
		if (InteractableInterface)
		{
			if (InteractableInterface->Execute_IsEnable(FocusActor))
			{
				InteractableInterface->Execute_Interact(FocusActor, this);
			}
		}
	}
}

bool APlayerCharacter::CanUseItem()
{
	return Super::CanUseItem() && InventoryData.Num() > 0 && HasItem(CurrentSlotItem) && !IsCrouching;
}

bool APlayerCharacter::CanOsu()
{
	return Super::CanOsu() && !IsCrouching;
}

bool APlayerCharacter::CanCrouch()
{
	return CanMove() && !IsJumping();
}

bool APlayerCharacter::CanUncrouch()
{
	FVector PlayerLocation = GetActorLocation();
	FVector UpVector = UKismetMathLibrary::GetUpVector(GetActorRotation());
	FVector TraceEndLocation = PlayerLocation + UpVector * 100.0f;
	FHitResult TraceHitResult;
	FCollisionQueryParams TraceCollisionParams;
	TraceCollisionParams.AddIgnoredActor(this);
	GetWorld()->LineTraceSingleByChannel(TraceHitResult, PlayerLocation, TraceEndLocation,
	                                     ECC_Visibility,
	                                     TraceCollisionParams);
	bool IsSomethingAbovePlayer = TraceHitResult.bBlockingHit;
	return !IsSomethingAbovePlayer && !IsJumping();
}

bool APlayerCharacter::CanSprint()
{
	return CanMove() && !IsCrouching && !IsJumping();
}

bool APlayerCharacter::CanAttack()
{
	return Super::CanAttack()
		&& !IsSprinting;
}

bool APlayerCharacter::CanPunch()
{
	return Super::CanPunch() && !IsCrouching;
}

bool APlayerCharacter::CanFire()
{
	if (IsCrouching)
	{
		return Super::CanFire() && !IsMoving();
	}
	return Super::CanFire();
}

bool APlayerCharacter::CanGuard()
{
	return Super::CanGuard()
		&& !IsCrouching;
}

bool APlayerCharacter::CanCharacterJump()
{
	return Super::CanCharacterJump()
		&& !IsCrouching && !IsDodging();
}

void APlayerCharacter::OnSprintStart()
{
	if (IsSprinting) return;
	if (!CanSprint()) return;
	IsSprinting = true;
	CharacterMovementComponent->MaxWalkSpeed = SprintSpeed;
	WeaponSystemComponent->StartSprint();
}

void APlayerCharacter::OnSprintEnd()
{
	if (!IsSprinting) return;
	IsSprinting = false;
	CharacterMovementComponent->MaxWalkSpeed = WalkSpeed;
	WeaponSystemComponent->EndSprint();
}

void APlayerCharacter::TryCrouch()
{
	if (!CanCrouch()) return;
	if (IsCrouching)
	{
		if (!CanUncrouch())
		{
			return;
		}
		UnCrouch();
		CharacterMovementComponent->MaxWalkSpeed = WalkSpeed;
		IsCrouching = false;
	}
	else
	{
		Crouch();
		CharacterMovementComponent->MaxWalkSpeed = CrouchSpeed;
		IsCrouching = true;
	}
}


void APlayerCharacter::SetupGunCameraZoomTimeline()
{
	if (!GunCameraZoomCurve)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(TEXT("GunCameraZoomCurve is null!")));
		return;
	}

	FOnTimelineFloat TimelineProgress;
	TimelineProgress.BindUFunction(this, FName("GunCameraZoomTimelineProgress"));
	GunCameraZoomTimeline.AddInterpFloat(GunCameraZoomCurve, TimelineProgress);
	GunCameraZoomTimeline.SetLooping(false);
}

void APlayerCharacter::GunCameraZoomTimelineProgress(float Value)
{
	if (IsCrouching)
	{
		CameraBoom->TargetArmLength = FMath::Lerp(DefaultTargetArmLength, CrouchGunZoomTargetArmLength, Value);
		CameraBoom->SocketOffset = FMath::Lerp(DefaultCameraSocketOffset, CrouchGunZoomCameraSocketOffset, Value);
	}
	else
	{
		CameraBoom->TargetArmLength = FMath::Lerp(DefaultTargetArmLength, GunZoomTargetArmLength, Value);
		CameraBoom->SocketOffset = FMath::Lerp(DefaultCameraSocketOffset, GunZoomCameraSocketOffset, Value);
	}
}

void APlayerCharacter::SetupCrosshairWidget()
{
	if (!CrosshairWidgetClass)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(TEXT("CrosshairWidgetClass is null")));
		return;
	}
	CrosshairWidget = CreateWidget<UUserWidget>(PlayerController, CrosshairWidgetClass);
	if (CrosshairWidget)
	{
		CrosshairWidget->AddToViewport();
		HideCrosshair();
	}
}

void APlayerCharacter::TryDodgeRoll()
{
	if (!CanDodgeRoll()) return;
	if (IsDodging()) return;
	if (!IsTargetLocking && CurrentAnimationState == EAnimationState::Unarmed)
	{
		AnimInstance->Montage_Play(DodgeRollForwardMontage, 1.0f);
		return;
	}
	if (CurrentMovementVector.X > 0)
	{
		AnimInstance->Montage_Play(DodgeRollRightMontage, 1.0f);
		return;
	}
	if (CurrentMovementVector.X < 0)
	{
		AnimInstance->Montage_Play(DodgeRollLeftMontage, 1.0f);
		return;
	}
	if (CurrentMovementVector.Y < 0)
	{
		AnimInstance->Montage_Play(DodgeRollBackwardMontage, 1.0f);
		return;
	}
	AnimInstance->Montage_Play(DodgeRollForwardMontage, 1.0f);
}

void APlayerCharacter::OnAttackActionEnd()
{
	if (CurrentAnimationState != EAnimationState::Unarmed)
	{
		WeaponSystemComponent->OnFireActionEnd();
	}
}

void APlayerCharacter::UnlockTarget()
{
	IsTargetLocking = false;
	LockTargetEnemy->HideTargetWidget();
	CharacterMovementComponent->bOrientRotationToMovement = true;
	bUseControllerRotationYaw = false;
}

void APlayerCharacter::TryTargetLock()
{
	if (CurrentAnimationState != EAnimationState::Unarmed) return;
	
	if (IsTargetLocking)
	{
		UnlockTarget();
	}
	else
	{
		FHitResult OutHit;
		bool IsHit;
		SearchEnemyInFront(OutHit, IsHit);
		if (IsHit)
		{
			LockTargetEnemy = Cast<AEnemyCharacter>(OutHit.GetActor());

			if (LockTargetEnemy)
			{
				// GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::White, FString::Printf(TEXT("Target Actor: %s"),
				// *TargetActor->GetName()));
				LockTargetEnemy->ShowTargetWidget();
				CharacterMovementComponent->bOrientRotationToMovement = false;
				bUseControllerRotationYaw = true;
				IsTargetLocking = true;
			}
		}
	}
}

void APlayerCharacter::TryAttack()
{
	if (!CanAttack())
	{
		return;
	}

	if (CurrentAnimationState == EAnimationState::Unarmed)
	{
		if (CanPunch())
		{
			TryFistAttack();
		}
	}
	else
	{
		if (CanFire())
		{
			OnPlayerFire.Broadcast();
			WeaponSystemComponent->TryFire();
		}
	}
}

bool APlayerCharacter::IsMoveInputBeingPressed()
{
	return IsMovingInputPressing;
}

void APlayerCharacter::TryGuard()
{
	if (!CanGuard()) return;
	Super::TryGuard();
	AnimInstance->Montage_Play(BlockMontage, BlockMontagePlayRate);
}

void APlayerCharacter::TryGuardOrZoom()
{
	if (CurrentAnimationState == EAnimationState::Unarmed)
	{
		TryGuard();
	}
	else
	{
		if (!IsGunZooming)
		{
			GunCameraZoomTimeline.Stop();
			GunZoomInCamera();
		}
	}
}

void APlayerCharacter::TryZoomOut()
{
	if (CurrentAnimationState == EAnimationState::Unarmed)
	{
		return;
	}
	if (IsGunZooming)
	{
		GunCameraZoomTimeline.Stop();
		GunZoomOutCamera();
	}
}

void APlayerCharacter::GunZoomInCamera()
{
	IsGunZooming = true;
	const float CurrentPlaybackPosition = GunCameraZoomTimeline.GetPlaybackPosition();
	GunCameraZoomTimeline.PlayFromStart();
	GunCameraZoomTimeline.SetPlaybackPosition(CurrentPlaybackPosition, true);
	OnGunZoomIn.Broadcast();
}

void APlayerCharacter::GunZoomOutCamera()
{
	IsGunZooming = false;
	const float CurrentPlaybackPosition = GunCameraZoomTimeline.GetPlaybackPosition();
	GunCameraZoomTimeline.ReverseFromEnd();
	GunCameraZoomTimeline.SetPlaybackPosition(CurrentPlaybackPosition, true);
	OnGunZoomOut.Broadcast();
}

void APlayerCharacter::TryFistAttack()
{
	// if (AnimInstance->Montage_IsPlaying(FistAttackMontage)) return;
	if (FistAttackMontages.IsEmpty())
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red, FString::Printf(
			                                 TEXT("FistAttackMontages is empty, %s"),
			                                 *GetName()));
		UE_LOG(LogTemp, Error, TEXT("FistAttackMontages is empty! %s"), *GetName());
		return;
	}


	FHitResult OutHit;
	bool IsHit;
	SearchEnemyInFront(OutHit, IsHit);
	if (IsHit)
	{
		AEnemyCharacter* InFrontEnemy = Cast<AEnemyCharacter>(OutHit.GetActor());
		if (InFrontEnemy)
		{
			bool IsInAttackRange = InFrontEnemy->GetDistanceTo(this) <= 130;
			if (IsInAttackRange && InFrontEnemy->IsExecutable)
			{
				ExecutingTarget = InFrontEnemy;
				AnimInstance->Montage_Play(ExecutePunchAttackMontage, 1.f);
				return;
			}
		}
	}

	if (!IsPlayingFistAttackMontage())
	{
		int32 RandomIndex = FMath::RandRange(0, FistAttackMontages.Num() - 1);
		UAnimMontage* RandomFistAttackMontage = FistAttackMontages[RandomIndex];
		AnimInstance->Montage_Play(RandomFistAttackMontage, 1.0f);
	}
	else
	{
		IsMeleeAttackInputReceived = true;
	}
}


void APlayerCharacter::OnPlayMontageNotifyBegin(FName NotifyName,
                                                const FBranchingPointNotifyPayload& BranchingPointPayload)
{
	// GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::White, FString::Printf(TEXT("Notify Name %s"), *NotifyName.ToString()));
	if (!IsMeleeAttackInputReceived && NotifyName == "ComboContinue")
	{
		AnimInstance->Montage_Stop(0.35f);
	}

	if (NotifyName == "ComboContinue")
	{
		IsMeleeAttackInputReceived = false;
	}

	if (NotifyName == "Execute")
	{
		AController* InstigatorController = GetInstigatorController();
		UClass* DamageTypeClass = UDamageType::StaticClass();
		UGameplayStatics::ApplyDamage(ExecutingTarget, ExecutingTarget->MaxHp, InstigatorController, this,
		                              DamageTypeClass);
	}

	if (NotifyName == "BeginPush")
	{
		OnBeginPush.Broadcast();
	}
}


void APlayerCharacter::OnMontageEnded(UAnimMontage* Montage, bool bInterrupted)
{
	Super::OnMontageEnded(Montage, bInterrupted);

	if (Montage->GetName().Contains("Combo"))
	{
		IsMeleeAttackInputReceived = false;
	}
}

void APlayerCharacter::Die()
{
	Super::Die();
	OnPlayerDeath.Broadcast();
}

void APlayerCharacter::SetAnimationState(EAnimationState NewAnimationState)
{
	Super::SetAnimationState(NewAnimationState);
	switch (CurrentAnimationState)
	{
	case EAnimationState::Unarmed:
		CharacterMovementComponent->bOrientRotationToMovement = true;
		bUseControllerRotationYaw = false;
		HideCrosshair();
		break;
	case EAnimationState::Pistol:
		CharacterMovementComponent->bOrientRotationToMovement = false;
		bUseControllerRotationYaw = true;
		ShowCrosshair();
		break;
	case EAnimationState::Rifle:
		CharacterMovementComponent->bOrientRotationToMovement = false;
		bUseControllerRotationYaw = true;
		ShowCrosshair();
		break;
	}
}

bool APlayerCharacter::GetIsTargetLocking()
{
	return IsTargetLocking;
}

void APlayerCharacter::SetSkipAllAnimationBlueprint(bool bValue)
{
	IsSkipAllAnimationBlueprint = bValue;
}

void APlayerCharacter::BeginFistAttack(bool IsLeftFist)
{
	Super::BeginFistAttack(IsLeftFist);
}

void APlayerCharacter::EndFistAttack(bool IsLeftFist)
{
	Super::EndFistAttack(IsLeftFist);
}

void APlayerCharacter::FindAndHighlightInteractableObjectNearPlayer()
{
	TArray<AActor*> IgnoredActors;
	IgnoredActors.Add(this);
	bool IsHit = UKismetSystemLibrary::SphereOverlapActors(GetWorld(), GetActorLocation(),
	                                                       FindHighlightInteractiveObjectDistance,
	                                                       InteractableObjectTypes, nullptr,
	                                                       IgnoredActors, CloseActors);
	if (IsHit)
	{
		float MinDistance = 1000000;
		AActor* ClosestInteractableObject = nullptr;
		for (AActor* OverlappingActor : CloseActors)
		{
			float Distance = GetDistanceTo(OverlappingActor);
			if (Distance < MinDistance)
			{
				MinDistance = Distance;
				ClosestInteractableObject = OverlappingActor;
			}
		}

		FocusActor = ClosestInteractableObject;
		IInteractableInterface* InteractableInterface = Cast<IInteractableInterface>(ClosestInteractableObject);
		if (InteractableInterface)
		{
			if (!InteractableInterface->Execute_IsInteractiveHUDVisible(ClosestInteractableObject) &&
				InteractableInterface->Execute_IsEnable(ClosestInteractableObject))
			{
				InteractableInterface->Execute_ToggleOutline(ClosestInteractableObject, true);
				InteractableInterface->Execute_StartCheckAndUpdateWidgetVisibleTimer(ClosestInteractableObject);
			}
		}
	}
}

void APlayerCharacter::TogglePauseGame()
{
	GameInstance->TogglePauseGame();
}


float APlayerCharacter::TakeDamage(float DamageAmount, FDamageEvent const& DamageEvent, AController* EventInstigator,
                                   AActor* DamageCauser)
{
	return Super::TakeDamage(DamageAmount, DamageEvent, EventInstigator, DamageCauser);
}

int32 APlayerCharacter::GetInventoryItemCount(UItem* Item) const
{
	if (const int32* FoundItemCount = InventoryData.Find(Item))
	{
		return *FoundItemCount;
	}
	return 0;
}

bool APlayerCharacter::HasItem(UItem* Item)
{
	return InventoryData.Contains(Item);
}

bool APlayerCharacter::AddInventoryItem(UItem* NewItem, int32 ItemCount)
{
	bool IsAddSuccessful;
	if (!NewItem)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(TEXT("AddInventoryItem: Failed trying to add null item!")));
		return false;
	}
	if (ItemCount <= 0)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(TEXT("AddInventoryItem: ItemCount must be greater than 0!")));
		return false;
	}

	int32 MaxCount = NewItem->MaxCount;
	if (MaxCount <= 0)
	{
		MaxCount = MAX_int32;
	}
	if (InventoryData.Contains(NewItem))
	{
		int32 OldItemCount = InventoryData[NewItem];
		InventoryData[NewItem] = FMath::Clamp(OldItemCount + ItemCount, 1, MaxCount);
		IsAddSuccessful = true;
	}
	else
	{
		InventoryData.Add(NewItem, ItemCount);
		IsAddSuccessful = true;
		OnPlayerAddItem.Broadcast(NewItem);
	}
	// Temp set slotItem
	return IsAddSuccessful;
}

bool APlayerCharacter::RemoveInventoryItem(UItem* RemovedItem, int32 RemoveCount)
{
	bool IsRemoveSuccessful;
	if (!RemovedItem)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(
			                                 TEXT("RemoveInventoryItem: Failed trying to remove null item!")));
		return false;
	}

	int32 ItemCount = GetInventoryItemCount(RemovedItem);
	if (ItemCount <= 0)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red,
		                                 FString::Printf(TEXT(
			                                 "RemoveInventoryItem: Failed trying to remove item with 0 count!")));
		return false;
	}

	// If RemoveCount <= 0, delete all
	if (RemoveCount <= 0)
	{
		ItemCount = 0;
	}
	else
	{
		ItemCount = FMath::Clamp(ItemCount - RemoveCount, 0, RemovedItem->MaxCount);
	}

	if (ItemCount > 0)
	{
		InventoryData[RemovedItem] = ItemCount;
		IsRemoveSuccessful = true;
	}
	else
	{
		InventoryData.Remove(RemovedItem);
		IsRemoveSuccessful = true;
	}
	return IsRemoveSuccessful;
}

bool APlayerCharacter::UseItem(UItem* Item)
{
	if (GetInventoryItemCount(Item) <= 0)
	{
		GEngine->AddOnScreenDebugMessage(-1, 15.0f, FColor::Red, FString::Printf(TEXT("No Item in Inventory")));
		return false;
	}

	bool IsRemoveItemSuccessful = RemoveInventoryItem(Item, 1);
	if (IsRemoveItemSuccessful)
	{
		// Vinegar Only
		Heal(MaxHp * 0.5f);
		OnPlayerUseItem.Broadcast();
	}
	return IsRemoveItemSuccessful;
}

bool APlayerCharacter::UseSlotItem()
{
	return UseItem(CurrentSlotItem);
}

void APlayerCharacter::ShowCrosshair()
{
	CrosshairWidget->SetVisibility(ESlateVisibility::Visible);
}

void APlayerCharacter::HideCrosshair()
{
	CrosshairWidget->SetVisibility(ESlateVisibility::Hidden);
}
