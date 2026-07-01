#include "Edit/UEPIEditOperation.h"

#include "ScopedTransaction.h"

namespace UE::ProjectIntelligence
{
	class FUEPIEditTransactionScope
	{
	public:
		FUEPIEditTransactionScope(const FUEPIEditContext& InContext, const FText& Description)
			: Context(InContext)
			, Transaction(InContext.bDryRun ? nullptr : MakeUnique<FScopedTransaction>(Description))
		{
		}

		bool IsDryRun() const
		{
			return Context.bDryRun;
		}

	private:
		const FUEPIEditContext& Context;
		TUniquePtr<FScopedTransaction> Transaction;
	};
}
