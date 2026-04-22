#include "posting_list.h"
#include "posting_list_iterator.h"

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/vector.h>
#include <util/generic/string.h>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

namespace {

TString BuildBasicList(const TVector<ui64>& docIds, TVector<TSkipPointer>& skips, ui32 skipInterval = 128) {
    TPostingListWriter writer(EPostingListType::Basic, skipInterval);
    for (auto docId : docIds) {
        writer.AddDoc(docId);
    }
    TString data = writer.Finish();
    skips = writer.GetSkipPointers();
    return data;
}

TString BuildBasicList(const TVector<ui64>& docIds, ui32 skipInterval = 128) {
    TVector<TSkipPointer> skips;
    return BuildBasicList(docIds, skips, skipInterval);
}

TString BuildWeightedList(const TVector<std::pair<ui64, ui32>>& entries, TVector<TSkipPointer>& skips, ui32 skipInterval = 128) {
    TPostingListWriter writer(EPostingListType::Weighted, skipInterval);
    for (auto& [docId, freq] : entries) {
        writer.AddDoc(docId, freq);
    }
    TString data = writer.Finish();
    skips = writer.GetSkipPointers();
    return data;
}

TString BuildWeightedList(const TVector<std::pair<ui64, ui32>>& entries, ui32 skipInterval = 128) {
    TVector<TSkipPointer> skips;
    return BuildWeightedList(entries, skips, skipInterval);
}

struct TPositionalEntry {
    ui64 DocId;
    ui32 Freq;
    TVector<ui32> Positions;
};

TString BuildPositionalList(const TVector<TPositionalEntry>& entries, TVector<TSkipPointer>& skips, ui32 skipInterval = 128) {
    TPostingListWriter writer(EPostingListType::Positional, skipInterval);
    for (auto& e : entries) {
        writer.AddDoc(e.DocId, e.Freq, e.Positions);
    }
    TString data = writer.Finish();
    skips = writer.GetSkipPointers();
    return data;
}

TString BuildPositionalList(const TVector<TPositionalEntry>& entries, ui32 skipInterval = 128) {
    TVector<TSkipPointer> skips;
    return BuildPositionalList(entries, skips, skipInterval);
}

TVector<ui64> CollectDocIds(IPostingIterator& iter) {
    TVector<ui64> result;
    while (iter.Next()) {
        result.push_back(iter.DocId());
    }
    return result;
}

// Helper to create a TBasicPostingIterator wrapped in THolder<IPostingIterator>
THolder<IPostingIterator> MakeBasicIter(TVector<TStringBuf> addLists, TVector<TStringBuf> delLists = {},
                                         EPostingListType type = EPostingListType::Basic) {
    return MakeHolder<TBasicPostingIterator>(std::move(addLists), std::move(delLists), type);
}

THolder<IPostingIterator> MakePositionalIter(TVector<TStringBuf> addLists, TVector<TStringBuf> delLists = {}) {
    return MakeHolder<TBasicPostingIterator>(std::move(addLists), std::move(delLists), EPostingListType::Positional);
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// TBasicPostingIterator — extended tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TBasicPostingIteratorExtTest) {

    Y_UNIT_TEST(DuplicateDocIdsAcrossAddLists) {
        // Same doc_id in multiple ADD lists should appear only once
        TString add1 = BuildBasicList({10, 20, 30});
        TString add2 = BuildBasicList({20, 30, 40});
        TBasicPostingIterator iter({add1, add2}, {}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MultipleDelLists) {
        TString add = BuildBasicList({10, 20, 30, 40, 50});
        TString del1 = BuildBasicList({20});
        TString del2 = BuildBasicList({40});
        TBasicPostingIterator iter({add}, {del1, del2}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 30, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MultipleAddAndMultipleDel) {
        TString add1 = BuildBasicList({10, 30, 50});
        TString add2 = BuildBasicList({20, 40, 60});
        TString del1 = BuildBasicList({10, 40});
        TString del2 = BuildBasicList({60});
        TBasicPostingIterator iter({add1, add2}, {del1, del2}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {20, 30, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(PositionalIterator) {
        TString add = BuildPositionalList({
            {10, 2, {0, 5}},
            {20, 3, {1, 3, 7}},
            {30, 1, {4}},
        });
        TBasicPostingIterator iter({add}, {}, EPostingListType::Positional);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 2u);
        TVector<ui32> pos1 = {0, 5};
        UNIT_ASSERT_VALUES_EQUAL(iter.Positions(), pos1);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 3u);
        TVector<ui32> pos2 = {1, 3, 7};
        UNIT_ASSERT_VALUES_EQUAL(iter.Positions(), pos2);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 1u);
        TVector<ui32> pos3 = {4};
        UNIT_ASSERT_VALUES_EQUAL(iter.Positions(), pos3);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(AdvanceAlreadyAtTarget) {
        TString add = BuildBasicList({10, 20, 30, 40, 50});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        // Advance to value <= current should return true immediately
        UNIT_ASSERT(iter.Advance(5));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(10));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
    }

    Y_UNIT_TEST(AdvancePastEnd) {
        TString add = BuildBasicList({10, 20});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(!iter.Advance(100));
        UNIT_ASSERT(!iter.Valid());
    }

    Y_UNIT_TEST(AdvanceWithSkipPointers) {
        // Build a large list with skip pointers
        TVector<ui64> docs;
        for (ui64 i = 1; i <= 500; ++i) {
            docs.push_back(i * 10);
        }
        TVector<TSkipPointer> skips;
        TString add = BuildBasicList(docs, skips, 4);
        UNIT_ASSERT(!skips.empty());

        TVector<TVector<TSkipPointer>> addSkips = {skips};
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic, std::move(addSkips));

        UNIT_ASSERT(iter.Advance(2500));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 2500ull);

        UNIT_ASSERT(iter.Advance(4990));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 4990ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 5000ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(AdvanceWithDelSkipPointers) {
        TVector<ui64> addDocs;
        for (ui64 i = 1; i <= 100; ++i) {
            addDocs.push_back(i);
        }
        TVector<ui64> delDocs;
        for (ui64 i = 1; i <= 100; i += 2) {
            delDocs.push_back(i); // delete all odd
        }
        TVector<TSkipPointer> addSkips, delSkips;
        TString add = BuildBasicList(addDocs, addSkips, 4);
        TString del = BuildBasicList(delDocs, delSkips, 4);

        TBasicPostingIterator iter(
            {add}, {del}, EPostingListType::Basic,
            {addSkips}, {delSkips});

        // Should get only even numbers
        UNIT_ASSERT(iter.Advance(50));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 52ull);
    }

    Y_UNIT_TEST(NextAfterAdvance) {
        TString add = BuildBasicList({10, 20, 30, 40, 50});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Advance(30));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 40ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(AdvanceThenAdvance) {
        TString add = BuildBasicList({10, 20, 30, 40, 50, 60, 70});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Advance(20));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);

        UNIT_ASSERT(iter.Advance(50));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(iter.Advance(65));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 70ull);

        UNIT_ASSERT(!iter.Advance(80));
    }

    Y_UNIT_TEST(DelReaderAdvancesCorrectly) {
        // DEL list has entries beyond ADD list range
        TString add = BuildBasicList({10, 20, 30});
        TString del = BuildBasicList({5, 20, 100});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(ThreeAddListsWithOverlap) {
        TString add1 = BuildBasicList({10, 20, 30});
        TString add2 = BuildBasicList({10, 25, 30});
        TString add3 = BuildBasicList({15, 20, 30});
        TBasicPostingIterator iter({add1, add2, add3}, {}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 15, 20, 25, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(WeightedWithDel) {
        TString add = BuildWeightedList({{10, 3}, {20, 5}, {30, 1}, {40, 7}});
        TString del = BuildBasicList({20, 30});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Weighted);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 3u);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 40ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 7u);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(PositionalWithDel) {
        TString add = BuildPositionalList({
            {10, 2, {0, 5}},
            {20, 1, {3}},
            {30, 3, {1, 4, 9}},
        });
        TString del = BuildBasicList({20});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Positional);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 2u);
        TVector<ui32> pos1 = {0, 5};
        UNIT_ASSERT_VALUES_EQUAL(iter.Positions(), pos1);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 3u);
        TVector<ui32> pos3 = {1, 4, 9};
        UNIT_ASSERT_VALUES_EQUAL(iter.Positions(), pos3);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(EmptyAddEmptyDel) {
        TBasicPostingIterator iter({}, {}, EPostingListType::Basic);
        UNIT_ASSERT(!iter.Next());
        UNIT_ASSERT(!iter.Valid());
        UNIT_ASSERT(!iter.Advance(10));
    }

    Y_UNIT_TEST(SingleDocAdvanceExact) {
        TString add = BuildBasicList({42});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Advance(42));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 42ull);
        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(LargeDocIds) {
        ui64 big1 = 1ull << 40;
        ui64 big2 = (1ull << 40) + 100;
        ui64 big3 = (1ull << 48);
        TString add = BuildBasicList({big1, big2, big3});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Advance(big2));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), big2);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), big3);
    }
}

////////////////////////////////////////////////////////////////////////////////
// TIntersectIterator — extended tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TIntersectIteratorExtTest) {

    Y_UNIT_TEST(SingleChild) {
        TString list1 = BuildBasicList({10, 20, 30});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceAlreadyAtTarget) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({10, 20, 30, 40, 50});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        // Advance to <= current doc
        UNIT_ASSERT(iter.Advance(5));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(10));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
    }

    Y_UNIT_TEST(AdvancePastEnd) {
        TString list1 = BuildBasicList({10, 20});
        TString list2 = BuildBasicList({10, 20});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Advance(100));
        UNIT_ASSERT(!iter.Valid());
    }

    Y_UNIT_TEST(AdvanceWithoutPriorNext) {
        // Advance on a fresh (uninitialized) iterator
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({20, 30, 50, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
    }

    Y_UNIT_TEST(NextAfterAdvance) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({10, 30, 50});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(OneChildExhaustsMidIteration) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({10, 20}); // exhausts after 20

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceThenAdvance) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50, 60, 70, 80});
        TString list2 = BuildBasicList({10, 20, 40, 60, 80});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(15));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);

        UNIT_ASSERT(iter.Advance(55));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 60ull);

        UNIT_ASSERT(iter.Advance(80));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 80ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(FourListsIntersect) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50, 60, 70, 80, 90, 100});
        TString list2 = BuildBasicList({20, 40, 60, 80, 100});
        TString list3 = BuildBasicList({10, 40, 60, 100});
        TString list4 = BuildBasicList({40, 60, 100});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));
        children.push_back(MakeBasicIter({list3}));
        children.push_back(MakeBasicIter({list4}));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {40, 60, 100};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(IdenticalLists) {
        TString list = BuildBasicList({10, 20, 30});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list}));
        children.push_back(MakeBasicIter({list}));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(IntersectWithDelInChild) {
        // Children themselves have DEL lists; intersection should skip deleted docs
        TString add1 = BuildBasicList({10, 20, 30, 40, 50});
        TString del1 = BuildBasicList({30}); // 30 deleted from first child
        TString add2 = BuildBasicList({10, 20, 30, 40, 50});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({add1}, {del1}));
        children.push_back(MakeBasicIter({add2}));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 40, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceToExactIntersection) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({20, 30, 50});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(30));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
    }
}

////////////////////////////////////////////////////////////////////////////////
// TUnionIterator — extended tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TUnionIteratorExtTest) {

    Y_UNIT_TEST(AdvanceAlreadyAtTarget) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(5));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(10));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
    }

    Y_UNIT_TEST(AdvancePastEnd) {
        TString list1 = BuildBasicList({10, 20});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Advance(100));
        UNIT_ASSERT(!iter.Valid());
    }

    Y_UNIT_TEST(AdvanceToExact) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(30));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
    }

    Y_UNIT_TEST(NextAfterAdvance) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(35));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 40ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 60ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(AdvanceThenAdvance) {
        TString list1 = BuildBasicList({10, 30, 50, 70, 90});
        TString list2 = BuildBasicList({20, 40, 60, 80, 100});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);

        UNIT_ASSERT(iter.Advance(75));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 80ull);

        UNIT_ASSERT(iter.Advance(100));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 100ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(ManyOverlappingChildren) {
        TString list1 = BuildBasicList({10, 20, 30});
        TString list2 = BuildBasicList({10, 20, 30});
        TString list3 = BuildBasicList({10, 20, 30});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));
        children.push_back(MakeBasicIter({list3}));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceWithoutPriorNext) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));

        TUnionIterator iter(std::move(children));
        // Advance without calling Next first
        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
    }

    Y_UNIT_TEST(UnionWithDelInChildren) {
        TString add1 = BuildBasicList({10, 20, 30});
        TString del1 = BuildBasicList({20});
        TString add2 = BuildBasicList({25, 35});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({add1}, {del1}));
        children.push_back(MakeBasicIter({add2}));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 25, 30, 35};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(FourChildrenMixed) {
        TString list1 = BuildBasicList({1, 5, 9});
        TString list2 = BuildBasicList({2, 6, 10});
        TString list3 = BuildBasicList({3, 5, 7}); // note: 5 overlaps with list1
        TString list4 = BuildBasicList({4, 8});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));
        children.push_back(MakeBasicIter({list2}));
        children.push_back(MakeBasicIter({list3}));
        children.push_back(MakeBasicIter({list4}));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceToFirstDoc) {
        TString list1 = BuildBasicList({10, 20, 30});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeBasicIter({list1}));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(1));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
    }
}

////////////////////////////////////////////////////////////////////////////////
// TPhraseIterator — extended tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPhraseIteratorExtTest) {

    Y_UNIT_TEST(AdvanceToPhrase) {
        // Phrase "A B" in docs 10 and 30
        TString list1 = BuildPositionalList({{10, 1, {0}}, {20, 1, {0}}, {30, 1, {2}}});
        TString list2 = BuildPositionalList({{10, 1, {1}}, {20, 1, {5}}, {30, 1, {3}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
    }

    Y_UNIT_TEST(AdvancePastEnd) {
        TString list1 = BuildPositionalList({{10, 1, {0}}});
        TString list2 = BuildPositionalList({{10, 1, {1}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Advance(100));
        UNIT_ASSERT(!iter.Valid());
    }

    Y_UNIT_TEST(AdvanceAlreadyAtTarget) {
        TString list1 = BuildPositionalList({{10, 1, {0}}, {20, 1, {0}}});
        TString list2 = BuildPositionalList({{10, 1, {1}}, {20, 1, {1}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(5));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);

        UNIT_ASSERT(iter.Advance(10));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
    }

    Y_UNIT_TEST(NextAfterAdvance) {
        TString list1 = BuildPositionalList({{10, 1, {0}}, {20, 1, {0}}, {30, 1, {0}}});
        TString list2 = BuildPositionalList({{10, 1, {1}}, {20, 1, {1}}, {30, 1, {1}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(15));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(SecondPositionOccurrenceMatches) {
        // Word1 at {0, 5}, word2 at {6} -> position 5+1=6 matches (not 0+1=1)
        TString list1 = BuildPositionalList({{10, 2, {0, 5}}});
        TString list2 = BuildPositionalList({{10, 1, {6}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MultipleStartingPositionsForPhrase) {
        // "A B" appears twice in the same doc: at positions (0,1) and (5,6)
        TString list1 = BuildPositionalList({{10, 2, {0, 5}}});
        TString list2 = BuildPositionalList({{10, 2, {1, 6}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(OneChildHasNoDocsInCommon) {
        // word1 in doc 10, word2 only in doc 20 -> no phrase match
        TString list1 = BuildPositionalList({{10, 1, {0}}});
        TString list2 = BuildPositionalList({{20, 1, {1}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(FourWordPhrase) {
        // "A B C D" at positions 2,3,4,5 in doc 10
        TString list1 = BuildPositionalList({{10, 1, {2}}, {20, 1, {0}}});
        TString list2 = BuildPositionalList({{10, 1, {3}}, {20, 1, {0}}});
        TString list3 = BuildPositionalList({{10, 1, {4}}, {20, 1, {0}}});
        TString list4 = BuildPositionalList({{10, 1, {5}}, {20, 1, {0}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));
        children.push_back(MakePositionalIter({list3}));
        children.push_back(MakePositionalIter({list4}));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        // doc 10: 2,3,4,5 is consecutive; doc 20: all at 0, not consecutive
        TVector<ui64> expected = {10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(PhraseSkipsNonMatchingThenFinds) {
        // Several docs: phrase only matches in the last one
        TString list1 = BuildPositionalList({
            {10, 1, {0}},
            {20, 1, {0}},
            {30, 1, {0}},
            {40, 1, {7}},
        });
        TString list2 = BuildPositionalList({
            {10, 1, {5}}, // 0+1=1 != 5
            {20, 1, {3}}, // 0+1=1 != 3
            {30, 1, {9}}, // 0+1=1 != 9
            {40, 1, {8}}, // 7+1=8 match!
        });

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(PhraseAdvanceThenNext) {
        TString list1 = BuildPositionalList({
            {10, 1, {0}},
            {20, 1, {0}},
            {30, 1, {0}},
        });
        TString list2 = BuildPositionalList({
            {10, 1, {1}},
            {20, 1, {1}},
            {30, 1, {1}},
        });

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakePositionalIter({list1}));
        children.push_back(MakePositionalIter({list2}));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(15));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(PhraseWithDelInChild) {
        // Phrase matches in docs 10,20,30 but doc 20 is deleted from one child
        TString add1 = BuildPositionalList({{10, 1, {0}}, {20, 1, {0}}, {30, 1, {0}}});
        TString del1 = BuildBasicList({20});
        TString add2 = BuildPositionalList({{10, 1, {1}}, {20, 1, {1}}, {30, 1, {1}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(
            TVector<TStringBuf>{add1}, TVector<TStringBuf>{del1}, EPostingListType::Positional));
        children.push_back(MakePositionalIter({add2}));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Nested iterator combinations
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TNestedIteratorTest) {

    Y_UNIT_TEST(IntersectOfUnions) {
        // (A OR B) AND (C OR D)
        TString listA = BuildBasicList({10, 20, 30});
        TString listB = BuildBasicList({25, 35, 45});
        TString listC = BuildBasicList({15, 25, 35});
        TString listD = BuildBasicList({20, 40, 50});

        TVector<THolder<IPostingIterator>> unionChildren1;
        unionChildren1.push_back(MakeBasicIter({listA}));
        unionChildren1.push_back(MakeBasicIter({listB}));

        TVector<THolder<IPostingIterator>> unionChildren2;
        unionChildren2.push_back(MakeBasicIter({listC}));
        unionChildren2.push_back(MakeBasicIter({listD}));

        TVector<THolder<IPostingIterator>> intersectChildren;
        intersectChildren.push_back(MakeHolder<TUnionIterator>(std::move(unionChildren1)));
        intersectChildren.push_back(MakeHolder<TUnionIterator>(std::move(unionChildren2)));

        TIntersectIterator iter(std::move(intersectChildren));
        auto docIds = CollectDocIds(iter);
        // Union1 = {10,20,25,30,35,45}, Union2 = {15,20,25,35,40,50}
        // Intersection = {20, 25, 35}
        TVector<ui64> expected = {20, 25, 35};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(UnionOfIntersects) {
        // (A AND B) OR (C AND D)
        TString listA = BuildBasicList({10, 20, 30, 40});
        TString listB = BuildBasicList({20, 30, 50});
        TString listC = BuildBasicList({30, 40, 60});
        TString listD = BuildBasicList({40, 50, 60});

        TVector<THolder<IPostingIterator>> intersectChildren1;
        intersectChildren1.push_back(MakeBasicIter({listA}));
        intersectChildren1.push_back(MakeBasicIter({listB}));

        TVector<THolder<IPostingIterator>> intersectChildren2;
        intersectChildren2.push_back(MakeBasicIter({listC}));
        intersectChildren2.push_back(MakeBasicIter({listD}));

        TVector<THolder<IPostingIterator>> unionChildren;
        unionChildren.push_back(MakeHolder<TIntersectIterator>(std::move(intersectChildren1)));
        unionChildren.push_back(MakeHolder<TIntersectIterator>(std::move(intersectChildren2)));

        TUnionIterator iter(std::move(unionChildren));
        auto docIds = CollectDocIds(iter);
        // Intersect1 = {20, 30}, Intersect2 = {40, 60}
        // Union = {20, 30, 40, 60}
        TVector<ui64> expected = {20, 30, 40, 60};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceOnNestedIterators) {
        // (A OR B) AND C
        TString listA = BuildBasicList({10, 30, 50, 70, 90});
        TString listB = BuildBasicList({20, 40, 60, 80, 100});
        TString listC = BuildBasicList({10, 40, 50, 80, 100});

        TVector<THolder<IPostingIterator>> unionChildren;
        unionChildren.push_back(MakeBasicIter({listA}));
        unionChildren.push_back(MakeBasicIter({listB}));

        TVector<THolder<IPostingIterator>> intersectChildren;
        intersectChildren.push_back(MakeHolder<TUnionIterator>(std::move(unionChildren)));
        intersectChildren.push_back(MakeBasicIter({listC}));

        TIntersectIterator iter(std::move(intersectChildren));
        // Union = {10,20,30,40,50,60,70,80,90,100} intersect C = {10,40,50,80,100}
        UNIT_ASSERT(iter.Advance(45));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 80ull);
    }
}

} // namespace NKikimr::NPostingList
