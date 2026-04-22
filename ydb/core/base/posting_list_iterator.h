#pragma once

#include "posting_list.h"

#include <util/generic/ptr.h>
#include <util/generic/vector.h>
#include <util/system/types.h>

#include <queue>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// IPostingIterator - base interface for query iterators
////////////////////////////////////////////////////////////////////////////////

class IPostingIterator {
public:
    virtual ~IPostingIterator() = default;

    // Advance to the next matching document.
    virtual bool Next() = 0;

    // Seek to the first document with doc_id >= targetDocId.
    virtual bool Advance(ui64 targetDocId) = 0;

    // Check if iterator is positioned at a valid document.
    virtual bool Valid() const = 0;

    // Current document id.
    virtual ui64 DocId() const = 0;

    // Frequency (for weighted/positional lists, 0 otherwise).
    virtual ui32 Freq() const { return 0; }

    // Positions (for positional lists, empty otherwise).
    virtual const TVector<ui32>& Positions() const {
        static TVector<ui32> empty;
        return empty;
    }
};

////////////////////////////////////////////////////////////////////////////////
// TBasicPostingIterator - wraps TPostingListReader with ADD/DEL merging
////////////////////////////////////////////////////////////////////////////////

class TBasicPostingIterator : public IPostingIterator {
public:
    // Takes ADD and DEL posting lists for a single word_id.
    // Merges ADD lists via k-way merge and subtracts DEL on-the-fly.
    TBasicPostingIterator(
        TVector<TStringBuf> addLists,
        TVector<TStringBuf> delLists,
        EPostingListType type,
        TVector<TVector<TSkipPointer>> addSkipPointers = {},
        TVector<TVector<TSkipPointer>> delSkipPointers = {});

    bool Next() override;
    bool Advance(ui64 targetDocId) override;
    bool Valid() const override;
    ui64 DocId() const override;
    ui32 Freq() const override;
    const TVector<ui32>& Positions() const override;

private:
    // Advance internal state to the next non-deleted document.
    bool AdvanceInternal();

    // Check if the given doc_id is present in any DEL list.
    bool IsDeleted(ui64 docId);

    // Advance all DEL readers to at least targetDocId.
    void AdvanceDelReaders(ui64 targetDocId);

private:
    EPostingListType Type;
    TVector<TPostingListReader> AddReaders;
    TVector<TPostingListReader> DelReaders;

    // Min-heap entry for k-way merge of ADD lists
    struct THeapEntry {
        ui64 DocId;
        size_t ReaderIndex;

        bool operator>(const THeapEntry& other) const {
            return DocId > other.DocId;
        }
    };

    std::priority_queue<THeapEntry, TVector<THeapEntry>, std::greater<THeapEntry>> AddHeap;

    bool IsValid = false;
    ui64 CurrentDocId = 0;
    ui32 CurrentFreq = 0;
    TVector<ui32> CurrentPositions;
};

////////////////////////////////////////////////////////////////////////////////
// TIntersectIterator - AND query (returns doc only if in ALL children)
////////////////////////////////////////////////////////////////////////////////

class TIntersectIterator : public IPostingIterator {
public:
    explicit TIntersectIterator(TVector<THolder<IPostingIterator>> children);

    bool Next() override;
    bool Advance(ui64 targetDocId) override;
    bool Valid() const override;
    ui64 DocId() const override;

private:
    // Align all children on the same doc_id. Returns true if found.
    bool AlignChildren();

private:
    TVector<THolder<IPostingIterator>> Children;
    bool IsValid = false;
    ui64 CurrentDocId = 0;
};

////////////////////////////////////////////////////////////////////////////////
// TUnionIterator - OR query (returns doc if in ANY child)
////////////////////////////////////////////////////////////////////////////////

class TUnionIterator : public IPostingIterator {
public:
    explicit TUnionIterator(TVector<THolder<IPostingIterator>> children);

    bool Next() override;
    bool Advance(ui64 targetDocId) override;
    bool Valid() const override;
    ui64 DocId() const override;

private:
    void InitHeap();

private:
    TVector<THolder<IPostingIterator>> Children;

    struct THeapEntry {
        ui64 DocId;
        size_t ChildIndex;

        bool operator>(const THeapEntry& other) const {
            return DocId > other.DocId;
        }
    };

    std::priority_queue<THeapEntry, TVector<THeapEntry>, std::greater<THeapEntry>> Heap;
    bool IsValid = false;
    ui64 CurrentDocId = 0;
    bool Initialized = false;
};

////////////////////////////////////////////////////////////////////////////////
// TPhraseIterator - positional phrase search
////////////////////////////////////////////////////////////////////////////////

class TPhraseIterator : public IPostingIterator {
public:
    // Children must be positional iterators for consecutive words in the phrase.
    explicit TPhraseIterator(TVector<THolder<IPostingIterator>> children);

    bool Next() override;
    bool Advance(ui64 targetDocId) override;
    bool Valid() const override;
    ui64 DocId() const override;

private:
    // Check if positions of all children form a consecutive phrase.
    bool CheckPositions() const;

    // Align children on the same doc_id and verify phrase positions.
    bool FindNextPhrase();

private:
    TVector<THolder<IPostingIterator>> Children;
    bool IsValid = false;
    ui64 CurrentDocId = 0;
};

} // namespace NKikimr::NPostingList
