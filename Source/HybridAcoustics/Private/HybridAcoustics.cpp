// Copyright Epic Games, Inc. All Rights Reserved.

#include "HybridAcoustics.h"
#include "HybridSpatialization.h"
#include "Features/IModularFeatures.h"

#define LOCTEXT_NAMESPACE "FHybridAcousticsModule"

static FHybridSpatializationFactory GSpatializationFactory;

void FHybridAcousticsModule::StartupModule()
{
	IModularFeatures::Get().RegisterModularFeature(
	IAudioSpatializationFactory::GetModularFeatureName(), &GSpatializationFactory);
}

void FHybridAcousticsModule::ShutdownModule()
{
	IModularFeatures::Get().UnregisterModularFeature(
	IAudioSpatializationFactory::GetModularFeatureName(), &GSpatializationFactory);
}

#undef LOCTEXT_NAMESPACE
	
IMPLEMENT_MODULE(FHybridAcousticsModule, HybridAcoustics)