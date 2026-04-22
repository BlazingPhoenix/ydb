#include "flat_comp_posting.h"

#include <library/cpp/monlib/service/pages/templates.h>

namespace NKikimr {
namespace NTable {
namespace NCompPosting {

namespace {

// Default maximum number of non-empty generations before we request a
// follow-up "hygiene" compaction. Posting tables are scanned across all
// levels for every query, so keeping the level count low is important.
static constexpr ui32 DEFAULT_MAX_LEVELS_FOR_SCAN = 3;

} // anonymous namespace

// ---------------------------------------------------------------------------
// TPostingCompactionStrategy
// ---------------------------------------------------------------------------

TPostingCompactionStrategy::TPostingCompactionStrategy(
        ui32 table,
        ICompactionBackend* backend,
        IResourceBroker* broker,
        ITimeProvider* time,
        NUtil::ILogger* logger,
        TString taskNameSuffix)
    : Table(table)
    , Backend(backend)
    , Logger(logger)
    , Inner(MakeHolder<NCompGen::TGenCompactionStrategy>(
          table, backend, broker, time, logger, std::move(taskNameSuffix)))
{
}

TPostingCompactionStrategy::~TPostingCompactionStrategy()
{
}

void TPostingCompactionStrategy::Start(TCompactionState state) {
    Inner->Start(std::move(state));
}

void TPostingCompactionStrategy::Stop() {
    Inner->Stop();
    HygieneRequested = false;
    CompactionsFinished = 0;
}

void TPostingCompactionStrategy::ReflectSchema() {
    Inner->ReflectSchema();

    // Pick up any posting-specific tunables from the compaction policy.
    // For now we rely on the default; a future extension may read a
    // custom field from TCompactionPolicy.
}

void TPostingCompactionStrategy::ReflectRemovedRowVersions() {
    Inner->ReflectRemovedRowVersions();
}

void TPostingCompactionStrategy::UpdateCompactions() {
    Inner->UpdateCompactions();
}

ui64 TPostingCompactionStrategy::GetBackingSize() {
    return Inner->GetBackingSize();
}

ui64 TPostingCompactionStrategy::GetBackingSize(ui64 ownerTabletId) {
    return Inner->GetBackingSize(ownerTabletId);
}

ui64 TPostingCompactionStrategy::BeginMemCompaction(
        TTaskId taskId, TSnapEdge edge, ui64 forcedCompactionId)
{
    return Inner->BeginMemCompaction(taskId, edge, forcedCompactionId);
}

bool TPostingCompactionStrategy::ScheduleBorrowedCompaction() {
    return Inner->ScheduleBorrowedCompaction();
}

void TPostingCompactionStrategy::AllowBorrowedGarbageCompaction() {
    Inner->AllowBorrowedGarbageCompaction();
}

ui64 TPostingCompactionStrategy::GetLastFinishedForcedCompactionId() const {
    return Inner->GetLastFinishedForcedCompactionId();
}

TInstant TPostingCompactionStrategy::GetLastFinishedForcedCompactionTs() const {
    return Inner->GetLastFinishedForcedCompactionTs();
}

TCompactionChanges TPostingCompactionStrategy::CompactionFinished(
        ui64 compactionId,
        THolder<TCompactionParams> params,
        THolder<TCompactionResult> result)
{
    auto changes = Inner->CompactionFinished(compactionId, std::move(params), std::move(result));

    ++CompactionsFinished;

    if (HygieneRequested) {
        HygieneRequested = false;
        if (auto logl = Logger->Log(NUtil::ELnLev::Debug)) {
            logl << "TPostingCompactionStrategy hygiene compaction finished for "
                 << Backend->OwnerTabletId() << " table " << Table;
        }
    }

    // After every compaction, check whether the posting table would
    // benefit from an additional round.
    MaybeRequestPostingHygiene();

    return changes;
}

void TPostingCompactionStrategy::PartMerged(TPartView part, ui32 level) {
    Inner->PartMerged(std::move(part), level);
}

void TPostingCompactionStrategy::PartMerged(TIntrusiveConstPtr<TColdPart> part, ui32 level) {
    Inner->PartMerged(std::move(part), level);
}

TCompactionChanges TPostingCompactionStrategy::PartsRemoved(TArrayRef<const TLogoBlobID> parts) {
    return Inner->PartsRemoved(parts);
}

TCompactionChanges TPostingCompactionStrategy::ApplyChanges() {
    return Inner->ApplyChanges();
}

TCompactionState TPostingCompactionStrategy::SnapshotState() {
    return Inner->SnapshotState();
}

bool TPostingCompactionStrategy::AllowForcedCompaction() {
    return Inner->AllowForcedCompaction();
}

void TPostingCompactionStrategy::OutputHtml(IOutputStream& out) {
    HTML(out) {
        DIV_CLASS("row") {
            out << "Posting compaction strategy (table " << Table << ")";
            out << ", compactions finished: " << CompactionsFinished;
            if (HygieneRequested) {
                out << " [hygiene requested]";
            }
        }
    }

    // Render the inner generation-based strategy state.
    Inner->OutputHtml(out);
}

void TPostingCompactionStrategy::MaybeRequestPostingHygiene() {
    if (HygieneRequested) {
        return; // already pending
    }

    // Count how many non-empty levels currently exist by looking at the
    // parts the backend reports for our table. Each distinct level with at
    // least one part counts.
    auto parts = Backend->TableParts(Table);
    if (parts.empty()) {
        return;
    }

    // We consider the total number of parts as a proxy for read
    // amplification. If it exceeds the threshold, request a forced
    // compaction so the inner strategy collapses levels.
    const ui32 maxLevels = MaxLevelsForScan > 0
        ? MaxLevelsForScan
        : DEFAULT_MAX_LEVELS_FOR_SCAN;

    // Simple heuristic: if we have more parts than maxLevels, we could
    // benefit from compaction.  The inner strategy already tracks levels
    // properly, but we nudge it by requesting changes when the part count
    // is too high.
    if (parts.size() > maxLevels) {
        if (auto logl = Logger->Log(NUtil::ELnLev::Debug)) {
            logl << "TPostingCompactionStrategy requesting hygiene compaction for "
                 << Backend->OwnerTabletId() << " table " << Table
                 << ": " << parts.size() << " parts, threshold " << maxLevels;
        }
        HygieneRequested = true;
        Backend->RequestChanges(Table);
    }
}

} // namespace NCompPosting
} // namespace NTable
} // namespace NKikimr
