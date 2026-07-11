#pragma once

#include "CoreMinimal.h"
#include "Templates/SharedPointer.h"

class FJsonObject;
class FJsonValue;
class FProperty;
class UStruct;

namespace UE::ProjectIntelligence
{
	class FUEPIPropertyCodec
	{
	public:
		static TSharedRef<FJsonObject> BuildSchema(const UStruct* Struct, int32 MaxDepth = 8);
		static TSharedRef<FJsonObject> BuildObjectSchema(const UObject* Object, int32 MaxDepth = 8);
		static TSharedPtr<FJsonValue> ReadValue(const FProperty* Property, const void* ValuePtr, int32 Depth = 0, int32 MaxDepth = 8);
		static bool WriteValue(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, FString& OutError, int32 Depth = 0, int32 MaxDepth = 8);
		static bool WriteValueWithMode(const FProperty* Property, void* ValuePtr, const TSharedPtr<FJsonValue>& Value, const FString& Mode, FString& OutError, int32 MaxDepth = 8);
		static bool SetPropertyPath(UObject* Object, const FString& PropertyPath, const TSharedPtr<FJsonValue>& Value, TSharedPtr<FJsonValue>& OutBefore, TSharedPtr<FJsonValue>& OutAfter, FString& OutError, const FString& Mode = TEXT("replace"));
	};
}
