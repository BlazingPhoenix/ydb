#include "posting_list.h"
#include "posting_list_merge.h"
#include "posting_list_iterator.h"

#include <library/cpp/testing/unittest/registar.h>

#include <util/generic/vector.h>
#include <util/generic/string.h>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// Helpers
////////////////////////////////////////////////////////////////////////////////

namespace {

// Build a Basic posting list from a vector of doc_ids
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

// Build a Weighted posting list from (doc_id, freq) pairs
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

// Build a Positional posting list
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

// Read all doc_ids from a Basic posting list
TVector<ui64> ReadAllDocIds(TStringBuf data, EPostingListType type = EPostingListType::Basic, const TVector<TSkipPointer>& skips = {}) {
    TPostingListReader reader(data, type, skips);
    TVector<ui64> result;
    while (reader.Next()) {
        result.push_back(reader.DocId());
    }
    return result;
}

// Read all doc_ids from an IPostingIterator
TVector<ui64> CollectDocIds(IPostingIterator& iter) {
    TVector<ui64> result;
    while (iter.Next()) {
        result.push_back(iter.DocId());
    }
    return result;
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// Varint encode/decode tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TVarintTest) {

    Y_UNIT_TEST(EncodeDecodeSmallValues32) {
        for (ui32 val : {0u, 1u, 42u, 127u}) {
            TString buf;
            EncodeVarint32(val, buf);
            UNIT_ASSERT_VALUES_EQUAL(buf.size(), 1u);
            ui32 decoded = 0;
            size_t consumed = DecodeVarint32(buf.data(), buf.size(), decoded);
            UNIT_ASSERT_VALUES_EQUAL(consumed, 1u);
            UNIT_ASSERT_VALUES_EQUAL(decoded, val);
        }
    }

    Y_UNIT_TEST(EncodeDecodeMediumValues32) {
        for (ui32 val : {128u, 255u, 16383u}) {
            TString buf;
            EncodeVarint32(val, buf);
            UNIT_ASSERT(buf.size() == 2);
            ui32 decoded = 0;
            size_t consumed = DecodeVarint32(buf.data(), buf.size(), decoded);
            UNIT_ASSERT(consumed == 2);
            UNIT_ASSERT_VALUES_EQUAL(decoded, val);
        }
    }

    Y_UNIT_TEST(EncodeDecodeLargeValues32) {
        for (ui32 val : {16384u, 1000000u, Max<ui32>()}) {
            TString buf;
            EncodeVarint32(val, buf);
            UNIT_ASSERT(buf.size() >= 3);
            ui32 decoded = 0;
            size_t consumed = DecodeVarint32(buf.data(), buf.size(), decoded);
            UNIT_ASSERT(consumed > 0);
            UNIT_ASSERT_VALUES_EQUAL(decoded, val);
        }
    }

    Y_UNIT_TEST(EncodeDecodeSmallValues64) {
        for (ui64 val : {0ull, 1ull, 127ull}) {
            TString buf;
            EncodeVarint64(val, buf);
            UNIT_ASSERT_VALUES_EQUAL(buf.size(), 1u);
            ui64 decoded = 0;
            size_t consumed = DecodeVarint64(buf.data(), buf.size(), decoded);
            UNIT_ASSERT_VALUES_EQUAL(consumed, 1u);
            UNIT_ASSERT_VALUES_EQUAL(decoded, val);
        }
    }

    Y_UNIT_TEST(EncodeDecodeLargeValues64) {
        for (ui64 val : {ui64(128), ui64(1000000), ui64(1) << 32, Max<ui64>()}) {
            TString buf;
            EncodeVarint64(val, buf);
            ui64 decoded = 0;
            size_t consumed = DecodeVarint64(buf.data(), buf.size(), decoded);
            UNIT_ASSERT(consumed > 0);
            UNIT_ASSERT_VALUES_EQUAL(decoded, val);
        }
    }

    Y_UNIT_TEST(MultipleVarintsInBuffer) {
        TString buf;
        EncodeVarint32(100, buf);
        EncodeVarint32(200, buf);
        EncodeVarint64(300, buf);

        ui32 v1 = 0, v2 = 0;
        ui64 v3 = 0;
        size_t pos = 0;
        size_t c = DecodeVarint32(buf.data() + pos, buf.size() - pos, v1);
        UNIT_ASSERT(c > 0);
        pos += c;
        c = DecodeVarint32(buf.data() + pos, buf.size() - pos, v2);
        UNIT_ASSERT(c > 0);
        pos += c;
        c = DecodeVarint64(buf.data() + pos, buf.size() - pos, v3);
        UNIT_ASSERT(c > 0);
        pos += c;

        UNIT_ASSERT_VALUES_EQUAL(v1, 100u);
        UNIT_ASSERT_VALUES_EQUAL(v2, 200u);
        UNIT_ASSERT_VALUES_EQUAL(v3, 300ull);
        UNIT_ASSERT_VALUES_EQUAL(pos, buf.size());
    }

    Y_UNIT_TEST(DecodeEmptyBuffer) {
        ui32 v32 = 42;
        UNIT_ASSERT_VALUES_EQUAL(DecodeVarint32("", 0, v32), 0u);
        ui64 v64 = 42;
        UNIT_ASSERT_VALUES_EQUAL(DecodeVarint64("", 0, v64), 0u);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Basic posting list write/read tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPostingListBasicTest) {

    Y_UNIT_TEST(EmptyList) {
        TPostingListWriter writer(EPostingListType::Basic);
        TString data = writer.Finish();
        UNIT_ASSERT(data.empty());

        TPostingListReader reader(data, EPostingListType::Basic);
        UNIT_ASSERT(!reader.Next());
        UNIT_ASSERT(!reader.Valid());
    }

    Y_UNIT_TEST(SingleDoc) {
        TString data = BuildBasicList({42});
        auto docIds = ReadAllDocIds(data);
        UNIT_ASSERT_VALUES_EQUAL(docIds.size(), 1u);
        UNIT_ASSERT_VALUES_EQUAL(docIds[0], 42ull);
    }

    Y_UNIT_TEST(MultipleDocs) {
        TVector<ui64> expected = {1, 5, 10, 100, 1000, 10000};
        TString data = BuildBasicList(expected);
        auto docIds = ReadAllDocIds(data);
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(LargeDocIds) {
        TVector<ui64> expected = {1ull << 32, (1ull << 32) + 1, (1ull << 48), Max<ui64>() - 1};
        TString data = BuildBasicList(expected);
        auto docIds = ReadAllDocIds(data);
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(ConsecutiveDocIds) {
        TVector<ui64> expected;
        for (ui64 i = 1; i <= 200; ++i) {
            expected.push_back(i);
        }
        TString data = BuildBasicList(expected);
        auto docIds = ReadAllDocIds(data);
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceForward) {
        TVector<ui64> docs = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100};
        TString data = BuildBasicList(docs);
        TPostingListReader reader(data, EPostingListType::Basic);

        UNIT_ASSERT(reader.Advance(35));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 40ull);

        UNIT_ASSERT(reader.Advance(70));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 70ull);

        UNIT_ASSERT(reader.Advance(100));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 100ull);

        UNIT_ASSERT(!reader.Advance(101));
    }

    Y_UNIT_TEST(AdvanceToExact) {
        TVector<ui64> docs = {10, 20, 30};
        TString data = BuildBasicList(docs);
        TPostingListReader reader(data, EPostingListType::Basic);

        UNIT_ASSERT(reader.Advance(10));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 10ull);
    }

    Y_UNIT_TEST(AdvancePastEnd) {
        TVector<ui64> docs = {10, 20};
        TString data = BuildBasicList(docs);
        TPostingListReader reader(data, EPostingListType::Basic);

        UNIT_ASSERT(!reader.Advance(100));
        UNIT_ASSERT(!reader.Valid());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Skip pointer tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TSkipPointerTest) {

    Y_UNIT_TEST(SkipPointersGenerated) {
        // With skipInterval=4, inserting 10 docs should produce skip pointers
        TVector<ui64> docs;
        for (ui64 i = 1; i <= 10; ++i) {
            docs.push_back(i * 100);
        }
        TVector<TSkipPointer> skips;
        TString data = BuildBasicList(docs, skips, 4);

        // Skip pointers at entries 4, 8 (0-indexed: after 4th and 8th entries)
        UNIT_ASSERT_VALUES_EQUAL(skips.size(), 2u);
        UNIT_ASSERT_VALUES_EQUAL(skips[0].DocId, 500ull); // 5th doc (index 4)
        UNIT_ASSERT_VALUES_EQUAL(skips[1].DocId, 900ull); // 9th doc (index 8)
    }

    Y_UNIT_TEST(AdvanceWithSkipPointers) {
        TVector<ui64> docs;
        for (ui64 i = 1; i <= 1000; ++i) {
            docs.push_back(i * 10);
        }
        TVector<TSkipPointer> skips;
        TString data = BuildBasicList(docs, skips, 4);
        UNIT_ASSERT(!skips.empty());

        TPostingListReader reader(data, EPostingListType::Basic, skips);

        // Advance to a doc deep in the list using skip pointers
        UNIT_ASSERT(reader.Advance(5000));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 5000ull);

        UNIT_ASSERT(reader.Advance(9990));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 9990ull);

        UNIT_ASSERT(reader.Advance(10000));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 10000ull);

        UNIT_ASSERT(!reader.Advance(10001));
    }

    Y_UNIT_TEST(AdvanceWithSkipPointersWeighted) {
        TVector<std::pair<ui64, ui32>> entries;
        for (ui64 i = 1; i <= 100; ++i) {
            entries.push_back({i * 10, static_cast<ui32>(i % 5 + 1)});
        }
        TVector<TSkipPointer> skips;
        TString data = BuildWeightedList(entries, skips, 4);
        UNIT_ASSERT(!skips.empty());

        TPostingListReader reader(data, EPostingListType::Weighted, skips);

        UNIT_ASSERT(reader.Advance(500));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 500ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 1u); // 50 % 5 + 1 = 1

        UNIT_ASSERT(reader.Advance(730));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 730ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 4u); // 73 % 5 + 1 = 4
    }
}

////////////////////////////////////////////////////////////////////////////////
// Weighted posting list tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPostingListWeightedTest) {

    Y_UNIT_TEST(SingleDocWithFreq) {
        TString data = BuildWeightedList({{42, 5}});
        TPostingListReader reader(data, EPostingListType::Weighted);
        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 42ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 5u);
        UNIT_ASSERT(!reader.Next());
    }

    Y_UNIT_TEST(MultipleDocsWithFreq) {
        TVector<std::pair<ui64, ui32>> entries = {{10, 3}, {20, 7}, {30, 1}};
        TString data = BuildWeightedList(entries);
        TPostingListReader reader(data, EPostingListType::Weighted);

        for (auto& [expectedDocId, expectedFreq] : entries) {
            UNIT_ASSERT(reader.Next());
            UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), expectedDocId);
            UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), expectedFreq);
        }
        UNIT_ASSERT(!reader.Next());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Positional posting list tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPostingListPositionalTest) {

    Y_UNIT_TEST(SingleDocWithPositions) {
        TVector<TPositionalEntry> entries = {{42, 3, {0, 5, 10}}};
        TString data = BuildPositionalList(entries);
        TPostingListReader reader(data, EPostingListType::Positional);

        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 42ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 3u);
        TVector<ui32> expectedPos = {0, 5, 10};
        UNIT_ASSERT_VALUES_EQUAL(reader.Positions(), expectedPos);
        UNIT_ASSERT(!reader.Next());
    }

    Y_UNIT_TEST(MultipleDocsWithPositions) {
        TVector<TPositionalEntry> entries = {
            {10, 2, {0, 3}},
            {20, 1, {7}},
            {30, 3, {1, 4, 9}},
        };
        TString data = BuildPositionalList(entries);
        TPostingListReader reader(data, EPostingListType::Positional);

        for (auto& e : entries) {
            UNIT_ASSERT(reader.Next());
            UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), e.DocId);
            UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), e.Freq);
            UNIT_ASSERT_VALUES_EQUAL(reader.Positions(), e.Positions);
        }
        UNIT_ASSERT(!reader.Next());
    }

    Y_UNIT_TEST(AdvanceWithPositions) {
        TVector<TPositionalEntry> entries = {
            {10, 1, {0}},
            {20, 2, {1, 3}},
            {30, 1, {5}},
        };
        TString data = BuildPositionalList(entries);
        TPostingListReader reader(data, EPostingListType::Positional);

        UNIT_ASSERT(reader.Advance(20));
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 20ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 2u);
        TVector<ui32> expectedPos = {1, 3};
        UNIT_ASSERT_VALUES_EQUAL(reader.Positions(), expectedPos);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Merge tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPostingListMergeTest) {

    Y_UNIT_TEST(MergeAddEmptyLists) {
        TVector<TStringBuf> lists;
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Basic, skips);
        UNIT_ASSERT(result.empty());
    }

    Y_UNIT_TEST(MergeAddSingleList) {
        TString data = BuildBasicList({10, 20, 30});
        TVector<TStringBuf> lists = {data};
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Basic, skips);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MergeAddTwoDisjointLists) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});
        TVector<TStringBuf> lists = {list1, list2};
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Basic, skips);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 20, 30, 40, 50, 60};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MergeAddOverlappingLists) {
        TString list1 = BuildBasicList({10, 20, 30});
        TString list2 = BuildBasicList({20, 30, 40});
        TVector<TStringBuf> lists = {list1, list2};
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Basic, skips);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 20, 30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MergeAddWeightedDuplicateTakesLatest) {
        // list2 (index 1) should win for duplicate doc_ids
        TString list1 = BuildWeightedList({{10, 1}, {20, 2}});
        TString list2 = BuildWeightedList({{20, 99}, {30, 3}});
        TVector<TStringBuf> lists = {list1, list2};
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Weighted, skips);

        TPostingListReader reader(result, EPostingListType::Weighted);
        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 1u);

        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 20ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 99u); // from list2

        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 30ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 3u);

        UNIT_ASSERT(!reader.Next());
    }

    Y_UNIT_TEST(MergeAddThreeLists) {
        TString list1 = BuildBasicList({1, 4, 7});
        TString list2 = BuildBasicList({2, 5, 8});
        TString list3 = BuildBasicList({3, 6, 9});
        TVector<TStringBuf> lists = {list1, list2, list3};
        TVector<TSkipPointer> skips;
        TString result = MergeAddPostingLists(lists, EPostingListType::Basic, skips);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {1, 2, 3, 4, 5, 6, 7, 8, 9};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(MergeDelEmptyLists) {
        TVector<TStringBuf> lists;
        TVector<TSkipPointer> skips;
        TString result = MergeDelPostingLists(lists, skips);
        UNIT_ASSERT(result.empty());
    }

    Y_UNIT_TEST(MergeDelTwoLists) {
        TString del1 = BuildBasicList({10, 30});
        TString del2 = BuildBasicList({20, 30, 40});
        TVector<TStringBuf> lists = {del1, del2};
        TVector<TSkipPointer> skips;
        TString result = MergeDelPostingLists(lists, skips);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 20, 30, 40}; // deduplicated
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(SubtractBasic) {
        TString addList = BuildBasicList({10, 20, 30, 40, 50});
        TString delList = BuildBasicList({20, 40});
        TVector<TSkipPointer> skips;
        TString result = SubtractPostingLists(addList, delList, EPostingListType::Basic, skips, false);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 30, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(SubtractKeepsDel) {
        TString addList = BuildBasicList({10, 20, 30});
        TString delList = BuildBasicList({20, 40}); // 40 has no matching ADD

        TVector<TSkipPointer> skips;
        TString remainingDel;
        TVector<TSkipPointer> delSkips;
        TString result = SubtractPostingLists(addList, delList, EPostingListType::Basic, skips,
            true, &remainingDel, &delSkips);

        auto addDocIds = ReadAllDocIds(result);
        TVector<ui64> expectedAdd = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(addDocIds, expectedAdd);

        auto delDocIds = ReadAllDocIds(remainingDel);
        TVector<ui64> expectedDel = {20, 40};
        UNIT_ASSERT_VALUES_EQUAL(delDocIds, expectedDel);
    }

    Y_UNIT_TEST(SubtractAllDeleted) {
        TString addList = BuildBasicList({10, 20, 30});
        TString delList = BuildBasicList({10, 20, 30});
        TVector<TSkipPointer> skips;
        TString result = SubtractPostingLists(addList, delList, EPostingListType::Basic, skips, false);

        auto docIds = ReadAllDocIds(result);
        UNIT_ASSERT(docIds.empty());
    }

    Y_UNIT_TEST(SubtractNothingDeleted) {
        TString addList = BuildBasicList({10, 20, 30});
        TString delList = BuildBasicList({5, 15, 25});
        TVector<TSkipPointer> skips;
        TString result = SubtractPostingLists(addList, delList, EPostingListType::Basic, skips, false);

        auto docIds = ReadAllDocIds(result);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(SubtractWeighted) {
        TString addList = BuildWeightedList({{10, 3}, {20, 5}, {30, 1}});
        TString delList = BuildBasicList({20});
        TVector<TSkipPointer> skips;
        TString result = SubtractPostingLists(addList, delList, EPostingListType::Weighted, skips, false);

        TPostingListReader reader(result, EPostingListType::Weighted);
        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 3u);

        UNIT_ASSERT(reader.Next());
        UNIT_ASSERT_VALUES_EQUAL(reader.DocId(), 30ull);
        UNIT_ASSERT_VALUES_EQUAL(reader.Freq(), 1u);

        UNIT_ASSERT(!reader.Next());
    }
}

////////////////////////////////////////////////////////////////////////////////
// CompactPostingLists tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TCompactionTest) {

    Y_UNIT_TEST(CompactNoDelFinalLevel) {
        TString add1 = BuildBasicList({10, 30});
        TString add2 = BuildBasicList({20, 40});
        TVector<TStringBuf> addLists = {add1, add2};
        TVector<TStringBuf> delLists;

        auto result = CompactPostingLists(addLists, delLists, EPostingListType::Basic, true);
        auto docIds = ReadAllDocIds(result.AddData);
        TVector<ui64> expected = {10, 20, 30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
        UNIT_ASSERT(result.DelData.empty());
    }

    Y_UNIT_TEST(CompactWithDelFinalLevel) {
        TString add1 = BuildBasicList({10, 20, 30, 40});
        TString del1 = BuildBasicList({20, 40});
        TVector<TStringBuf> addLists = {add1};
        TVector<TStringBuf> delLists = {del1};

        auto result = CompactPostingLists(addLists, delLists, EPostingListType::Basic, true);
        auto docIds = ReadAllDocIds(result.AddData);
        TVector<ui64> expected = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
        UNIT_ASSERT(result.DelData.empty()); // final level: DEL discarded
    }

    Y_UNIT_TEST(CompactWithDelIntermediateLevel) {
        TString add1 = BuildBasicList({10, 20, 30, 40});
        TString del1 = BuildBasicList({20, 40});
        TVector<TStringBuf> addLists = {add1};
        TVector<TStringBuf> delLists = {del1};

        auto result = CompactPostingLists(addLists, delLists, EPostingListType::Basic, false);
        auto addDocIds = ReadAllDocIds(result.AddData);
        TVector<ui64> expectedAdd = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(addDocIds, expectedAdd);

        // Intermediate level: DEL entries preserved
        auto delDocIds = ReadAllDocIds(result.DelData);
        TVector<ui64> expectedDel = {20, 40};
        UNIT_ASSERT_VALUES_EQUAL(delDocIds, expectedDel);
    }

    Y_UNIT_TEST(CompactMultipleAddAndDel) {
        TString add1 = BuildBasicList({10, 20, 30});
        TString add2 = BuildBasicList({15, 25, 35});
        TString del1 = BuildBasicList({10, 25});
        TString del2 = BuildBasicList({35});
        TVector<TStringBuf> addLists = {add1, add2};
        TVector<TStringBuf> delLists = {del1, del2};

        auto result = CompactPostingLists(addLists, delLists, EPostingListType::Basic, true);
        auto docIds = ReadAllDocIds(result.AddData);
        TVector<ui64> expected = {15, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(CompactEmpty) {
        TVector<TStringBuf> addLists;
        TVector<TStringBuf> delLists;
        auto result = CompactPostingLists(addLists, delLists, EPostingListType::Basic, true);
        UNIT_ASSERT(result.AddData.empty());
        UNIT_ASSERT(result.DelData.empty());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Basic posting iterator tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TBasicPostingIteratorTest) {

    Y_UNIT_TEST(SingleAddList) {
        TString add = BuildBasicList({10, 20, 30});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(TwoAddLists) {
        TString add1 = BuildBasicList({10, 30});
        TString add2 = BuildBasicList({20, 40});
        TBasicPostingIterator iter({add1, add2}, {}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AddWithDel) {
        TString add = BuildBasicList({10, 20, 30, 40, 50});
        TString del = BuildBasicList({20, 40});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Basic);
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 30, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceSkipsDeleted) {
        TString add = BuildBasicList({10, 20, 30, 40, 50});
        TString del = BuildBasicList({30});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Basic);

        UNIT_ASSERT(iter.Advance(25));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 40ull); // 30 is deleted, so skip to 40
    }

    Y_UNIT_TEST(WeightedIterator) {
        TString add = BuildWeightedList({{10, 3}, {20, 5}, {30, 1}});
        TBasicPostingIterator iter({add}, {}, EPostingListType::Weighted);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 10ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 3u);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 20ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 5u);

        UNIT_ASSERT(iter.Next());
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 30ull);
        UNIT_ASSERT_VALUES_EQUAL(iter.Freq(), 1u);

        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(EmptyAddLists) {
        TBasicPostingIterator iter({}, {}, EPostingListType::Basic);
        UNIT_ASSERT(!iter.Next());
        UNIT_ASSERT(!iter.Valid());
    }

    Y_UNIT_TEST(AllDeleted) {
        TString add = BuildBasicList({10, 20, 30});
        TString del = BuildBasicList({10, 20, 30});
        TBasicPostingIterator iter({add}, {del}, EPostingListType::Basic);
        UNIT_ASSERT(!iter.Next());
    }
}

////////////////////////////////////////////////////////////////////////////////
// Intersect iterator tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TIntersectIteratorTest) {

    Y_UNIT_TEST(TwoListsWithOverlap) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({20, 30, 50, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {20, 30, 50};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(ThreeListsIntersect) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50});
        TString list2 = BuildBasicList({20, 30, 40, 60});
        TString list3 = BuildBasicList({30, 40, 70});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list3}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TIntersectIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(NoIntersection) {
        TString list1 = BuildBasicList({10, 20});
        TString list2 = BuildBasicList({30, 40});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(AdvanceIntersect) {
        TString list1 = BuildBasicList({10, 20, 30, 40, 50, 60});
        TString list2 = BuildBasicList({10, 30, 50, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TIntersectIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(40));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 50ull); // 40 not in list2
    }
}

////////////////////////////////////////////////////////////////////////////////
// Union iterator tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TUnionIteratorTest) {

    Y_UNIT_TEST(TwoDisjointLists) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30, 40, 50, 60};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(TwoOverlappingLists) {
        TString list1 = BuildBasicList({10, 20, 30});
        TString list2 = BuildBasicList({20, 30, 40});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30, 40};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(ThreeListsUnion) {
        TString list1 = BuildBasicList({1, 4});
        TString list2 = BuildBasicList({2, 5});
        TString list3 = BuildBasicList({3, 6});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list3}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {1, 2, 3, 4, 5, 6};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(AdvanceUnion) {
        TString list1 = BuildBasicList({10, 30, 50});
        TString list2 = BuildBasicList({20, 40, 60});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TUnionIterator iter(std::move(children));
        UNIT_ASSERT(iter.Advance(35));
        UNIT_ASSERT_VALUES_EQUAL(iter.DocId(), 40ull);
    }

    Y_UNIT_TEST(SingleListUnion) {
        TString list1 = BuildBasicList({10, 20, 30});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Basic));

        TUnionIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 20, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }
}

////////////////////////////////////////////////////////////////////////////////
// Phrase iterator tests
////////////////////////////////////////////////////////////////////////////////

Y_UNIT_TEST_SUITE(TPhraseIteratorTest) {

    Y_UNIT_TEST(SimplePhraseMatch) {
        // "hello world" in doc 10: "hello" at pos 0, "world" at pos 1
        TString list1 = BuildPositionalList({{10, 2, {0, 5}}, {20, 1, {3}}});
        TString list2 = BuildPositionalList({{10, 2, {1, 7}}, {20, 1, {5}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Positional));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Positional));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        // Doc 10: word1 at {0,5}, word2 at {1,7} -> 0+1=1 matches, so doc 10 matches
        // Doc 20: word1 at {3}, word2 at {5} -> 3+1=4 != 5, no match
        TVector<ui64> expected = {10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(NoPhrase) {
        // Words appear in same doc but not adjacent
        TString list1 = BuildPositionalList({{10, 1, {0}}});
        TString list2 = BuildPositionalList({{10, 1, {5}}}); // not at position 1

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Positional));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Positional));

        TPhraseIterator iter(std::move(children));
        UNIT_ASSERT(!iter.Next());
    }

    Y_UNIT_TEST(ThreeWordPhrase) {
        // "the quick brown" -> positions must be consecutive
        TString list1 = BuildPositionalList({{10, 1, {2}}});
        TString list2 = BuildPositionalList({{10, 1, {3}}});
        TString list3 = BuildPositionalList({{10, 1, {4}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Positional));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Positional));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list3}, TVector<TStringBuf>{}, EPostingListType::Positional));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }

    Y_UNIT_TEST(PhraseInMultipleDocs) {
        // Phrase "A B" exists in docs 10 and 30 but not 20
        TString list1 = BuildPositionalList({{10, 1, {0}}, {20, 1, {0}}, {30, 1, {3}}});
        TString list2 = BuildPositionalList({{10, 1, {1}}, {20, 1, {5}}, {30, 1, {4}}});

        TVector<THolder<IPostingIterator>> children;
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list1}, TVector<TStringBuf>{}, EPostingListType::Positional));
        children.push_back(MakeHolder<TBasicPostingIterator>(TVector<TStringBuf>{list2}, TVector<TStringBuf>{}, EPostingListType::Positional));

        TPhraseIterator iter(std::move(children));
        auto docIds = CollectDocIds(iter);
        TVector<ui64> expected = {10, 30};
        UNIT_ASSERT_VALUES_EQUAL(docIds, expected);
    }
}

} // namespace NKikimr::NPostingList
