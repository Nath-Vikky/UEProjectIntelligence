#pragma once

#include "CoreMinimal.h"
#include "Edit/UEPIEditOperation.h"

namespace UE::ProjectIntelligence
{
	TSharedRef<IUEPIEditOperation> MakeUEPIAnimationOperation(const FUEPIEditOperationDescriptor& Descriptor);
}
