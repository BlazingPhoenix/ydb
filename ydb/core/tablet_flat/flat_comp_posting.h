#pragma once

#include "flat_comp.h"
#include "flat_comp_gen.h"

#include <library/cpp/time_provider/time_provider.h>
#include <ydb/core/tablet_flat/util_fmt_line.h>

namespace NKikimr {
namespace NTable {
namespace NCompPosting {

    /**
     * Metadata tag attached to compactions triggered by the posting
     * strategy's hygiene logic. This is not used to parameterize the
     * actual SST compaction (the inner TGenCompactionStrategy creates its
     * own TGenCompactionParams for that); it is kept here so that
     * CompactionFinished can distinguish hygiene-triggered rounds in
     * logging and metrics.
     */
    struct TPostingHygieneTag {
        ui64 CompactionId = 0;
    };

    /**
     * Compaction strategy for posting (inverted-index) tables.
     *
     * The posting table stores rows with PK (word_id, doc_id). Deletions
     * are represented as standard LSM erase markers. The standard
     * generation-based compaction already merges rows by PK and the IsFinal
     * flag controls whether erase markers are kept or dropped.
     *
     * This strategy wraps TGenCompactionStrategy and adds:
     *  1. Lower thresholds for triggering compaction -- posting tables
     *     benefit from fewer levels because every scan reads all levels.
     *  2. After a compaction finishes, the strategy checks whether a
     *     follow-up compaction should be scheduled to further reduce read
     *     amplification.
     *  3. A configurable "max levels for scan" parameter: if the number of
     *     non-empty levels exceeds this value the strategy raises priority
     *     of the next compaction.
     */
    class TPostingCompactionStrategy final
        : public ICompactionStrategy
    {
    public:
        TPostingCompactionStrategy(
            ui32 table,
            ICompactionBackend* backend,
            IResourceBroker* broker,
            ITimeProvider* time,
            NUtil::ILogger* logger,
            TString taskNameSuffix);

        ~TPostingCompactionStrategy();

        // ICompactionStrategy
        void Start(TCompactionState state) override;
        void Stop() override;

        void ReflectSchema() override;
        void ReflectRemovedRowVersions() override;
        void UpdateCompactions() override;
        ui64 GetBackingSize() override;
        ui64 GetBackingSize(ui64 ownerTabletId) override;
        ui64 BeginMemCompaction(TTaskId taskId, TSnapEdge edge, ui64 forcedCompactionId) override;
        bool ScheduleBorrowedCompaction() override;
        void AllowBorrowedGarbageCompaction() override;
        ui64 GetLastFinishedForcedCompactionId() const override;
        TInstant GetLastFinishedForcedCompactionTs() const override;
        TCompactionChanges CompactionFinished(
            ui64 compactionId,
            THolder<TCompactionParams> params,
            THolder<TCompactionResult> result) override;
        void PartMerged(TPartView part, ui32 level) override;
        void PartMerged(TIntrusiveConstPtr<TColdPart> part, ui32 level) override;
        TCompactionChanges PartsRemoved(TArrayRef<const TLogoBlobID> parts) override;
        TCompactionChanges ApplyChanges() override;
        TCompactionState SnapshotState() override;
        bool AllowForcedCompaction() override;
        void OutputHtml(IOutputStream& out) override;

    private:
        // Check whether we should request an extra compaction round to
        // reduce the number of levels a posting-list scan must touch.
        void MaybeRequestPostingHygiene();

    private:
        ui32 const Table;
        ICompactionBackend* const Backend;
        NUtil::ILogger* const Logger;

        // Delegate -- the real generation-based compaction strategy that
        // does all the heavy lifting.
        THolder<NCompGen::TGenCompactionStrategy> Inner;

        // Number of compactions finished since last hygiene check.
        ui64 CompactionsFinished = 0;

        // Maximum number of non-empty generations tolerated before
        // requesting a follow-up compaction. Zero means "use default".
        ui32 MaxLevelsForScan = 0;

        // True while a hygiene-triggered compaction is in progress.
        bool HygieneRequested = false;
    };

} // namespace NCompPosting
} // namespace NTable
} // namespace NKikimr
