#include "flat_comp_ut_common.h"

#include <ydb/core/tablet_flat/flat_comp_posting.h>

#include <library/cpp/testing/unittest/registar.h>

namespace NKikimr {
namespace NTable {
namespace NCompPosting {

using namespace NTest;

Y_UNIT_TEST_SUITE(TPostingCompaction) {

    constexpr ui32 Table = 1;

    struct Schema : NIceDb::Schema {
        struct Data : Table<1> {
            struct Key : Column<1, NScheme::NTypeIds::Uint64> { };
            struct Value : Column<2, NScheme::NTypeIds::Uint32> { };

            using TKey = TableKey<Key>;
            using TColumns = TableColumns<Key, Value>;
        };

        using TTables = SchemaTables<Data>;
    };

    void InitSchema(TSimpleBackend& backend) {
        auto db = backend.Begin();
        db.Materialize<Schema>();

        TCompactionPolicy policy;
        // Use large thresholds so the gen strategy doesn't auto-compact parts
        policy.Generations.emplace_back(1024ULL * 1024 * 1024, 100, 100, 1024ULL * 1024 * 1024, "compact_gen1", true);
        policy.Generations.emplace_back(1024ULL * 1024 * 1024, 100, 100, 1024ULL * 1024 * 1024, "compact_gen2", true);
        policy.Generations.emplace_back(1024ULL * 1024 * 1024, 100, 100, 1024ULL * 1024 * 1024, "compact_gen3", true);
        backend.DB.Alter().SetCompactionPolicy(Table, policy);

        backend.Commit();
    }

    void InsertRows(TSimpleBackend& backend, ui64 from, ui64 to) {
        auto db = backend.Begin();
        for (ui64 key = from; key < to; ++key) {
            db.Table<Schema::Data>().Key(key).Update<Schema::Data::Value>(42);
        }
        backend.Commit();
    }

    // Create N separate parts by inserting and mem-compacting without strategy.
    // This avoids the inner gen strategy merging parts.
    void CreateParts(TSimpleBackend& backend, int count, int rowsPerPart = 10) {
        for (int i = 0; i < count; ++i) {
            InsertRows(backend, i * rowsPerPart, (i + 1) * rowsPerPart);
            backend.SimpleMemCompaction(Table);
        }
    }

    Y_UNIT_TEST(DelegationBasic) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 0, 64);
        ui64 compactionId = strategy.BeginMemCompaction(0, {0, TEpoch::Max()}, 0);
        UNIT_ASSERT(compactionId != 0);

        auto outcome = backend.RunCompaction(compactionId);
        auto changes = strategy.CompactionFinished(
            compactionId, std::move(outcome.Params), std::move(outcome.Result));
        backend.ApplyChanges(Table, std::move(changes));

        UNIT_ASSERT(backend.TableParts(Table).size() >= 1);
        UNIT_ASSERT(strategy.GetBackingSize() > 0);

        strategy.Stop();
    }

    Y_UNIT_TEST(HygieneRequestedWhenTooManyParts) {
        // DEFAULT_MAX_LEVELS_FOR_SCAN = 3. If parts > 3, hygiene is requested.
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        // Create 4 parts without strategy to avoid auto-merging
        CreateParts(backend, 4);
        UNIT_ASSERT_VALUES_EQUAL(backend.TableParts(Table).size(), 4u);

        // Now start the strategy and do one mem compaction through it.
        // CompactionFinished will call MaybeRequestPostingHygiene.
        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 1000, 1010);
        backend.SimpleMemCompaction(&strategy);

        // With 5 parts (4 pre-existing + 1 new), hygiene should be requested
        UNIT_ASSERT(backend.TableParts(Table).size() > 3);
        UNIT_ASSERT(backend.CheckChangesFlag());

        strategy.Stop();
    }

    Y_UNIT_TEST(NoHygieneWhenFewParts) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        // Create 2 parts without strategy
        CreateParts(backend, 2);
        UNIT_ASSERT_VALUES_EQUAL(backend.TableParts(Table).size(), 2u);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 1000, 1010);
        backend.SimpleMemCompaction(&strategy);

        // 3 parts total, threshold is 3, so parts.size() == 3 => not > 3
        UNIT_ASSERT_VALUES_EQUAL(backend.TableParts(Table).size(), 3u);
        UNIT_ASSERT(!backend.CheckChangesFlag());

        strategy.Stop();
    }

    Y_UNIT_TEST(HygieneReRequestedOnEveryCompactionFinished) {
        // CompactionFinished clears HygieneRequested, then re-evaluates.
        // If parts still > threshold, it re-requests every time.
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        // Create 4 parts
        CreateParts(backend, 4);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // First mem compaction triggers hygiene
        InsertRows(backend, 1000, 1010);
        backend.SimpleMemCompaction(&strategy);
        UNIT_ASSERT(backend.CheckChangesFlag());

        // Second mem compaction: CompactionFinished clears flag then re-evaluates.
        // Parts are still > 3, so it re-requests.
        InsertRows(backend, 2000, 2010);
        backend.SimpleMemCompaction(&strategy);
        UNIT_ASSERT(backend.CheckChangesFlag());

        strategy.Stop();
    }

    Y_UNIT_TEST(HygieneStopsWhenPartsReduced) {
        // Once parts are reduced to <= threshold, hygiene is no longer requested.
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        // Use a policy with small thresholds to allow gen compactions to fire
        {
            auto db = backend.Begin();
            db.Materialize<Schema>();

            TCompactionPolicy policy;
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen1", true);
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen2", true);
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen3", true);
            backend.DB.Alter().SetCompactionPolicy(Table, policy);

            backend.Commit();
        }

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // Create multiple parts through the strategy
        for (int i = 0; i < 5; ++i) {
            InsertRows(backend, i * 50, (i + 1) * 50);
            backend.SimpleMemCompaction(&strategy);
        }

        // Run all pending gen compactions to reduce part count
        ui64 safetyCounter = 0;
        while (broker.HasPending()) {
            UNIT_ASSERT_C(safetyCounter++ < 100, "too many compaction rounds");
            UNIT_ASSERT(broker.RunPending());
            while (!backend.StartedCompactions.empty()) {
                backend.SimpleTableCompaction(Table, &broker, &strategy);
            }
        }

        backend.CheckChangesFlag(); // consume any pending flag

        // Now do one more mem compaction. If parts <= 3, no hygiene should be requested.
        InsertRows(backend, 500, 510);
        backend.SimpleMemCompaction(&strategy);

        size_t parts = backend.TableParts(Table).size();
        if (parts <= 3) {
            UNIT_ASSERT(!backend.CheckChangesFlag());
        } else {
            UNIT_ASSERT(backend.CheckChangesFlag());
        }

        strategy.Stop();
    }

    Y_UNIT_TEST(StopResetsState) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        CreateParts(backend, 4);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // Trigger hygiene
        InsertRows(backend, 1000, 1010);
        backend.SimpleMemCompaction(&strategy);
        UNIT_ASSERT(backend.CheckChangesFlag());

        // Stop resets HygieneRequested
        strategy.Stop();

        // Re-start: a new mem compaction should be able to trigger hygiene again
        strategy.Start({});

        InsertRows(backend, 3000, 3010);
        backend.SimpleMemCompaction(&strategy);

        // Parts should still be > 3, so hygiene is re-requested
        UNIT_ASSERT(backend.TableParts(Table).size() > 3);
        UNIT_ASSERT(backend.CheckChangesFlag());

        strategy.Stop();
    }

    Y_UNIT_TEST(ForcedCompactionDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 0, 64);

        ui64 forcedId = 42;
        ui64 compactionId = strategy.BeginMemCompaction(0, {0, TEpoch::Max()}, forcedId);
        UNIT_ASSERT(compactionId != 0);

        auto outcome = backend.RunCompaction(compactionId);
        auto changes = strategy.CompactionFinished(
            compactionId, std::move(outcome.Params), std::move(outcome.Result));
        backend.ApplyChanges(Table, std::move(changes));

        UNIT_ASSERT_VALUES_EQUAL(strategy.GetLastFinishedForcedCompactionId(), forcedId);
        UNIT_ASSERT(strategy.AllowForcedCompaction());

        strategy.Stop();
    }

    Y_UNIT_TEST(ReflectSchemaDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // Change schema — keep same or more generations to avoid "decreasing levels" error
        {
            backend.Begin();
            TCompactionPolicy policy;
            policy.Generations.emplace_back(512ULL * 1024 * 1024, 50, 50, 512ULL * 1024 * 1024, "gen1", true);
            policy.Generations.emplace_back(512ULL * 1024 * 1024, 50, 50, 512ULL * 1024 * 1024, "gen2", true);
            policy.Generations.emplace_back(512ULL * 1024 * 1024, 50, 50, 512ULL * 1024 * 1024, "gen3", true);
            backend.DB.Alter().SetCompactionPolicy(Table, policy);
            backend.Commit();
        }

        // Should delegate to inner without crash
        strategy.ReflectSchema();

        strategy.Stop();
    }

    Y_UNIT_TEST(SnapshotStateDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 0, 32);
        backend.SimpleMemCompaction(&strategy);

        auto state = strategy.SnapshotState();
        Y_UNUSED(state);

        strategy.Stop();
    }

    Y_UNIT_TEST(OutputHtmlRenders) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 0, 32);
        backend.SimpleMemCompaction(&strategy);

        TStringStream out;
        strategy.OutputHtml(out);
        TString html = out.Str();
        UNIT_ASSERT(html.Contains("Posting compaction strategy"));
        UNIT_ASSERT(html.Contains("compactions finished"));

        strategy.Stop();
    }

    Y_UNIT_TEST(OutputHtmlShowsHygieneRequested) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        CreateParts(backend, 4);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 1000, 1010);
        backend.SimpleMemCompaction(&strategy);
        backend.CheckChangesFlag(); // consume

        TStringStream out;
        strategy.OutputHtml(out);
        TString html = out.Str();
        UNIT_ASSERT(html.Contains("hygiene requested"));

        strategy.Stop();
    }

    Y_UNIT_TEST(GetBackingSizeDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        UNIT_ASSERT_VALUES_EQUAL(strategy.GetBackingSize(), 0u);
        UNIT_ASSERT_VALUES_EQUAL(strategy.GetBackingSize(backend.TabletId), 0u);

        InsertRows(backend, 0, 64);
        backend.SimpleMemCompaction(&strategy);

        UNIT_ASSERT(strategy.GetBackingSize() > 0);
        UNIT_ASSERT(strategy.GetBackingSize(backend.TabletId) > 0);

        strategy.Stop();
    }

    Y_UNIT_TEST(NoHygieneOnEmptyTable) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // Empty table — no parts, no hygiene
        InsertRows(backend, 0, 10);
        backend.SimpleMemCompaction(&strategy);

        // Just 1 part, no hygiene
        UNIT_ASSERT_VALUES_EQUAL(backend.TableParts(Table).size(), 1u);
        UNIT_ASSERT(!backend.CheckChangesFlag());

        strategy.Stop();
    }

    Y_UNIT_TEST(ScheduleBorrowedCompactionDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        bool result = strategy.ScheduleBorrowedCompaction();
        Y_UNUSED(result);
        strategy.AllowBorrowedGarbageCompaction();

        strategy.Stop();
    }

    Y_UNIT_TEST(UpdateCompactionsDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        InsertRows(backend, 0, 64);
        backend.SimpleMemCompaction(&strategy);

        strategy.UpdateCompactions();

        strategy.Stop();
    }

    Y_UNIT_TEST(ReflectRemovedRowVersionsDelegates) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        InitSchema(backend);

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        strategy.ReflectRemovedRowVersions();

        strategy.Stop();
    }

    Y_UNIT_TEST(FullCompactionReducesParts) {
        TSimpleBackend backend;
        TSimpleBroker broker;
        TSimpleLogger logger;
        TSimpleTime time;

        // Use a policy with small thresholds so gen compactions actually trigger
        {
            auto db = backend.Begin();
            db.Materialize<Schema>();

            TCompactionPolicy policy;
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen1", true);
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen2", true);
            policy.Generations.emplace_back(1, 1, 1, 10ULL * 1024 * 1024 * 1024, "gen3", true);
            backend.DB.Alter().SetCompactionPolicy(Table, policy);

            backend.Commit();
        }

        TPostingCompactionStrategy strategy(Table, &backend, &broker, &time, &logger, "posting");
        strategy.Start({});

        // Create many parts through the strategy
        for (int i = 0; i < 6; ++i) {
            InsertRows(backend, i * 50, (i + 1) * 50);
            backend.SimpleMemCompaction(&strategy);
        }

        // Run all pending compactions to completion
        ui64 safetyCounter = 0;
        while (broker.HasPending()) {
            UNIT_ASSERT_C(safetyCounter++ < 100, "too many compaction rounds");
            UNIT_ASSERT(broker.RunPending());
            while (!backend.StartedCompactions.empty()) {
                backend.SimpleTableCompaction(Table, &broker, &strategy);
            }
        }

        // After gen compactions, we should have fewer parts
        size_t finalParts = backend.TableParts(Table).size();
        UNIT_ASSERT_C(finalParts <= 3,
            "Expected at most 3 parts after compaction, got " << finalParts);

        strategy.Stop();
    }
};

} // NCompPosting
} // NTable
} // NKikimr
