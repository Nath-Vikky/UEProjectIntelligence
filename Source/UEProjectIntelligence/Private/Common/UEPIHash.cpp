#include "Common/UEPIHash.h"

THIRD_PARTY_INCLUDES_START
#include <openssl/sha.h>
THIRD_PARTY_INCLUDES_END

namespace UE::ProjectIntelligence
{
	FString Sha256Hex(const void* Data, uint64 ByteSize)
	{
		if (!Data && ByteSize > 0)
		{
			return FString();
		}
		uint8 Digest[SHA256_DIGEST_LENGTH];
		static const uint8 Empty = 0;
		if (!SHA256(static_cast<const uint8*>(Data ? Data : &Empty), static_cast<size_t>(ByteSize), Digest))
		{
			return FString();
		}
		return BytesToHex(Digest, UE_ARRAY_COUNT(Digest)).ToLower();
	}
}
