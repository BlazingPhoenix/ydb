#pragma once

#include "posting_list.h"

#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// Merge ADD posting lists using k-way merge.
// For duplicate doc_ids, the entry from the highest-index list wins.
////////////////////////////////////////////////////////////////////////////////

TString MergeAddPostingLists(
    const TVector<TStringBuf>& lists,
    EPostingListType type,
    TVector<TSkipPointer>& outSkipPointers,
    ui32 skipInterval = 128);

////////////////////////////////////////////////////////////////////////////////
// Merge DEL posting lists (Basic type) into one sorted deduplicated list.
////////////////////////////////////////////////////////////////////////////////

TString MergeDelPostingLists(
    const TVector<TStringBuf>& lists,
    TVector<TSkipPointer>& outSkipPointers,
    ui32 skipInterval = 128);

////////////////////////////////////////////////////////////////////////////////
// Subtract DEL posting list from ADD posting list.
// - Entries in ADD but not in DEL -> kept in output ADD
// - Entries in both -> removed from output ADD
// - If keepDel: DEL entries written to outRemainingDel
// - If !keepDel: DEL entries discarded
////////////////////////////////////////////////////////////////////////////////

TString SubtractPostingLists(
    TStringBuf addList,
    TStringBuf delList,
    EPostingListType type,
    TVector<TSkipPointer>& outSkipPointers,
    bool keepDel,
    TString* outRemainingDel = nullptr,
    TVector<TSkipPointer>* outDelSkipPointers = nullptr,
    ui32 skipInterval = 128);

////////////////////////////////////////////////////////////////////////////////
// Full compaction flow combining merge and subtract.
////////////////////////////////////////////////////////////////////////////////

struct TCompactionResult {
    TString AddData;
    TVector<TSkipPointer> AddSkipPointers;
    TString DelData;
    TVector<TSkipPointer> DelSkipPointers;
};

TCompactionResult CompactPostingLists(
    const TVector<TStringBuf>& addLists,
    const TVector<TStringBuf>& delLists,
    EPostingListType type,
    bool isFinalLevel,
    ui32 skipInterval = 128);

} // namespace NKikimr::NPostingList
