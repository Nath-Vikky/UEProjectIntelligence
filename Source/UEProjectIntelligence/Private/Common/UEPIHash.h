#pragma once

#include "CoreMinimal.h"

namespace UE::ProjectIntelligence
{
	FString Sha256Hex(const void* Data, uint64 ByteSize);
}
