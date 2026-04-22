#include "posting_list_iterator.h"

#include <util/generic/yexception.h>

#include <algorithm>
#include <limits>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// TBasicPostingIterator
////////////////////////////////////////////////////////////////////////////////

TBasicPostingIterator::TBasicPostingIterator(
    TVector<TStringBuf> addLists,
    TVector<TStringBuf> delLists,
    EPostingListType type,
    TVector<TVector<TSkipPointer>> addSkipPointers,
    TVector<TVector<TSkipPointer>> delSkipPointers)
    : Type(type)
{
    // Create ADD readers
    AddReaders.reserve(addLists.size());
    for (size_t i = 0; i < addLists.size(); ++i) {
        TVector<TSkipPointer> skips;
        if (i < addSkipPointers.size()) {
            skips = std::move(addSkipPointers[i]);
        }
        AddReaders.emplace_back(addLists[i], type, skips);
    }

    // Create DEL readers
    DelReaders.reserve(delLists.size());
    for (size_t i = 0; i < delLists.size(); ++i) {
        TVector<TSkipPointer> skips;
        if (i < delSkipPointers.size()) {
            skips = std::move(delSkipPointers[i]);
        }
        DelReaders.emplace_back(delLists[i], EPostingListType::Basic, skips);
    }

    // Initialize the min-heap: advance each ADD reader once and push to heap
    for (size_t i = 0; i < AddReaders.size(); ++i) {
        if (AddReaders[i].Next()) {
            AddHeap.push(THeapEntry{AddReaders[i].DocId(), i});
        }
    }

    // Initialize DEL readers (advance each once)
    for (auto& reader : DelReaders) {
        reader.Next();
    }
}

bool TBasicPostingIterator::IsDeleted(ui64 docId) {
    for (auto& reader : DelReaders) {
        if (reader.Valid() && reader.DocId() == docId) {
            return true;
        }
    }
    return false;
}

void TBasicPostingIterator::AdvanceDelReaders(ui64 targetDocId) {
    for (auto& reader : DelReaders) {
        if (reader.Valid() && reader.DocId() < targetDocId) {
            reader.Advance(targetDocId);
        }
    }
}

bool TBasicPostingIterator::AdvanceInternal() {
    while (!AddHeap.empty()) {
        auto top = AddHeap.top();
        AddHeap.pop();

        ui64 docId = top.DocId;
        size_t readerIdx = top.ReaderIndex;

        // Skip duplicates from other ADD lists with the same doc_id
        while (!AddHeap.empty() && AddHeap.top().DocId == docId) {
            auto dup = AddHeap.top();
            AddHeap.pop();
            // Advance the duplicate reader
            if (AddReaders[dup.ReaderIndex].Next()) {
                AddHeap.push(THeapEntry{AddReaders[dup.ReaderIndex].DocId(), dup.ReaderIndex});
            }
        }

        // Advance the primary reader for this doc
        auto& primaryReader = AddReaders[readerIdx];

        // Capture freq/positions before advancing the reader
        CurrentDocId = docId;
        CurrentFreq = 0;
        CurrentPositions.clear();

        if (Type == EPostingListType::Weighted || Type == EPostingListType::Positional) {
            CurrentFreq = primaryReader.Freq();
        }
        if (Type == EPostingListType::Positional) {
            CurrentPositions = primaryReader.Positions();
        }

        // Advance the primary reader to next entry
        if (primaryReader.Next()) {
            AddHeap.push(THeapEntry{primaryReader.DocId(), readerIdx});
        }

        // Check if this doc_id is deleted
        AdvanceDelReaders(docId);
        if (IsDeleted(docId)) {
            continue;
        }

        IsValid = true;
        return true;
    }

    IsValid = false;
    return false;
}

bool TBasicPostingIterator::Next() {
    return AdvanceInternal();
}

bool TBasicPostingIterator::Advance(ui64 targetDocId) {
    if (IsValid && CurrentDocId >= targetDocId) {
        return true;
    }

    // Drain heap entries below target.
    // Re-seek ADD readers that are below target using skip pointers.
    // Rebuild heap with seeked positions.
    TVector<THeapEntry> pending;
    while (!AddHeap.empty()) {
        pending.push_back(AddHeap.top());
        AddHeap.pop();
    }

    for (auto& entry : pending) {
        if (entry.DocId < targetDocId) {
            if (AddReaders[entry.ReaderIndex].Advance(targetDocId)) {
                AddHeap.push(THeapEntry{AddReaders[entry.ReaderIndex].DocId(), entry.ReaderIndex});
            }
        } else {
            AddHeap.push(entry);
        }
    }

    // Advance DEL readers too
    AdvanceDelReaders(targetDocId);

    // Now find the next non-deleted doc >= target
    return AdvanceInternal();
}

bool TBasicPostingIterator::Valid() const {
    return IsValid;
}

ui64 TBasicPostingIterator::DocId() const {
    Y_ENSURE(IsValid, "Iterator is not positioned at a valid entry");
    return CurrentDocId;
}

ui32 TBasicPostingIterator::Freq() const {
    return CurrentFreq;
}

const TVector<ui32>& TBasicPostingIterator::Positions() const {
    return CurrentPositions;
}

////////////////////////////////////////////////////////////////////////////////
// TIntersectIterator
////////////////////////////////////////////////////////////////////////////////

TIntersectIterator::TIntersectIterator(TVector<THolder<IPostingIterator>> children)
    : Children(std::move(children))
{
    Y_ENSURE(!Children.empty(), "TIntersectIterator requires at least one child");
}

bool TIntersectIterator::AlignChildren() {
    // Try to align all children on the same doc_id.
    // Strategy: pick the max doc_id among all children, then advance all others
    // to at least that. Repeat until all converge or some child exhausts.

    ui64 targetDocId = 0;

    // Find the current max doc_id among all valid children
    for (auto& child : Children) {
        if (!child->Valid()) {
            IsValid = false;
            return false;
        }
        targetDocId = std::max(targetDocId, child->DocId());
    }

    for (;;) {
        bool allAligned = true;
        for (auto& child : Children) {
            if (child->DocId() < targetDocId) {
                if (!child->Advance(targetDocId)) {
                    IsValid = false;
                    return false;
                }
            }
            if (child->DocId() > targetDocId) {
                targetDocId = child->DocId();
                allAligned = false;
                break; // restart alignment loop
            }
        }
        if (allAligned) {
            CurrentDocId = targetDocId;
            IsValid = true;
            return true;
        }
    }
}

bool TIntersectIterator::Next() {
    // On first call, advance all children once
    if (!IsValid && CurrentDocId == 0) {
        // Initial call: advance all children
        for (auto& child : Children) {
            if (!child->Next()) {
                IsValid = false;
                return false;
            }
        }
        return AlignChildren();
    }

    // Advance the first child past the current doc_id, then realign
    if (!Children[0]->Next()) {
        IsValid = false;
        return false;
    }
    if (!Children[0]->Valid()) {
        IsValid = false;
        return false;
    }
    return AlignChildren();
}

bool TIntersectIterator::Advance(ui64 targetDocId) {
    if (IsValid && CurrentDocId >= targetDocId) {
        return true;
    }

    // Advance all children to at least targetDocId
    for (auto& child : Children) {
        if (!child->Valid()) {
            // Try to initialize if not yet started
            if (!child->Next()) {
                IsValid = false;
                return false;
            }
        }
        if (child->DocId() < targetDocId) {
            if (!child->Advance(targetDocId)) {
                IsValid = false;
                return false;
            }
        }
    }

    return AlignChildren();
}

bool TIntersectIterator::Valid() const {
    return IsValid;
}

ui64 TIntersectIterator::DocId() const {
    Y_ENSURE(IsValid, "Iterator is not positioned at a valid entry");
    return CurrentDocId;
}

////////////////////////////////////////////////////////////////////////////////
// TUnionIterator
////////////////////////////////////////////////////////////////////////////////

TUnionIterator::TUnionIterator(TVector<THolder<IPostingIterator>> children)
    : Children(std::move(children))
{
    Y_ENSURE(!Children.empty(), "TUnionIterator requires at least one child");
}

void TUnionIterator::InitHeap() {
    if (Initialized) {
        return;
    }
    Initialized = true;
    for (size_t i = 0; i < Children.size(); ++i) {
        if (Children[i]->Next()) {
            Heap.push(THeapEntry{Children[i]->DocId(), i});
        }
    }
}

bool TUnionIterator::Next() {
    InitHeap();

    if (Heap.empty()) {
        IsValid = false;
        return false;
    }

    auto top = Heap.top();
    Heap.pop();
    CurrentDocId = top.DocId;

    // Advance the child that produced this entry
    if (Children[top.ChildIndex]->Next()) {
        Heap.push(THeapEntry{Children[top.ChildIndex]->DocId(), top.ChildIndex});
    }

    // Skip duplicate doc_ids from other children
    while (!Heap.empty() && Heap.top().DocId == CurrentDocId) {
        auto dup = Heap.top();
        Heap.pop();
        if (Children[dup.ChildIndex]->Next()) {
            Heap.push(THeapEntry{Children[dup.ChildIndex]->DocId(), dup.ChildIndex});
        }
    }

    IsValid = true;
    return true;
}

bool TUnionIterator::Advance(ui64 targetDocId) {
    InitHeap();

    if (IsValid && CurrentDocId >= targetDocId) {
        return true;
    }

    // Drain entries below target: advance each child to targetDocId
    TVector<THeapEntry> pending;
    while (!Heap.empty()) {
        pending.push_back(Heap.top());
        Heap.pop();
    }

    for (auto& entry : pending) {
        if (entry.DocId < targetDocId) {
            if (Children[entry.ChildIndex]->Advance(targetDocId)) {
                Heap.push(THeapEntry{Children[entry.ChildIndex]->DocId(), entry.ChildIndex});
            }
        } else {
            Heap.push(entry);
        }
    }

    if (Heap.empty()) {
        IsValid = false;
        return false;
    }

    // Take the minimum
    auto top = Heap.top();
    Heap.pop();
    CurrentDocId = top.DocId;

    // Advance this child
    if (Children[top.ChildIndex]->Next()) {
        Heap.push(THeapEntry{Children[top.ChildIndex]->DocId(), top.ChildIndex});
    }

    // Dedup
    while (!Heap.empty() && Heap.top().DocId == CurrentDocId) {
        auto dup = Heap.top();
        Heap.pop();
        if (Children[dup.ChildIndex]->Next()) {
            Heap.push(THeapEntry{Children[dup.ChildIndex]->DocId(), dup.ChildIndex});
        }
    }

    IsValid = true;
    return true;
}

bool TUnionIterator::Valid() const {
    return IsValid;
}

ui64 TUnionIterator::DocId() const {
    Y_ENSURE(IsValid, "Iterator is not positioned at a valid entry");
    return CurrentDocId;
}

////////////////////////////////////////////////////////////////////////////////
// TPhraseIterator
////////////////////////////////////////////////////////////////////////////////

TPhraseIterator::TPhraseIterator(TVector<THolder<IPostingIterator>> children)
    : Children(std::move(children))
{
    Y_ENSURE(Children.size() >= 2, "TPhraseIterator requires at least two children");
}

bool TPhraseIterator::CheckPositions() const {
    // For a phrase of N words, we need positions p0, p1, ..., p(N-1)
    // such that p(i+1) = p(i) + 1.
    // Use a simple scan: for each starting position in child 0, check if
    // consecutive positions exist in children 1..N-1.

    const auto& firstPositions = Children[0]->Positions();

    for (ui32 startPos : firstPositions) {
        bool found = true;
        ui32 expectedPos = startPos + 1;

        for (size_t c = 1; c < Children.size(); ++c) {
            const auto& childPositions = Children[c]->Positions();
            // Binary search for expectedPos
            auto it = std::lower_bound(childPositions.begin(), childPositions.end(), expectedPos);
            if (it == childPositions.end() || *it != expectedPos) {
                found = false;
                break;
            }
            ++expectedPos;
        }

        if (found) {
            return true;
        }
    }

    return false;
}

bool TPhraseIterator::FindNextPhrase() {
    // Align all children on the same doc_id (intersection logic),
    // then check phrase positions. If positions don't match, advance and retry.

    for (;;) {
        // Find the max doc_id among children
        ui64 targetDocId = 0;
        for (auto& child : Children) {
            if (!child->Valid()) {
                IsValid = false;
                return false;
            }
            targetDocId = std::max(targetDocId, child->DocId());
        }

        // Align all children to targetDocId
        bool allAligned = true;
        for (auto& child : Children) {
            if (child->DocId() < targetDocId) {
                if (!child->Advance(targetDocId)) {
                    IsValid = false;
                    return false;
                }
            }
            if (child->DocId() > targetDocId) {
                targetDocId = child->DocId();
                allAligned = false;
                break;
            }
        }

        if (!allAligned) {
            continue;
        }

        // All aligned on targetDocId, check positions
        if (CheckPositions()) {
            CurrentDocId = targetDocId;
            IsValid = true;
            return true;
        }

        // Positions don't match, advance first child and retry
        if (!Children[0]->Next()) {
            IsValid = false;
            return false;
        }
    }
}

bool TPhraseIterator::Next() {
    if (!IsValid && CurrentDocId == 0) {
        // Initial call: advance all children once
        for (auto& child : Children) {
            if (!child->Next()) {
                IsValid = false;
                return false;
            }
        }
        return FindNextPhrase();
    }

    // Advance first child past current doc
    if (!Children[0]->Next()) {
        IsValid = false;
        return false;
    }
    return FindNextPhrase();
}

bool TPhraseIterator::Advance(ui64 targetDocId) {
    if (IsValid && CurrentDocId >= targetDocId) {
        return true;
    }

    // Advance all children to at least targetDocId
    for (auto& child : Children) {
        if (!child->Valid()) {
            if (!child->Next()) {
                IsValid = false;
                return false;
            }
        }
        if (child->DocId() < targetDocId) {
            if (!child->Advance(targetDocId)) {
                IsValid = false;
                return false;
            }
        }
    }

    return FindNextPhrase();
}

bool TPhraseIterator::Valid() const {
    return IsValid;
}

ui64 TPhraseIterator::DocId() const {
    Y_ENSURE(IsValid, "Iterator is not positioned at a valid entry");
    return CurrentDocId;
}

} // namespace NKikimr::NPostingList
