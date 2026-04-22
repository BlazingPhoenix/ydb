#include "posting_list_merge.h"

#include <queue>
#include <util/generic/yexception.h>

namespace NKikimr::NPostingList {

namespace {

////////////////////////////////////////////////////////////////////////////////
// Helper: entry from a reader, tagged with its source list index.
////////////////////////////////////////////////////////////////////////////////

struct TReaderEntry {
    ui64 DocId;
    ui32 Freq;
    TVector<ui32> Positions;
    ui32 ListIndex; // index into the original lists vector
};

// For the priority queue: smallest doc_id first, ties broken by highest list index first.
struct TReaderEntryGreater {
    bool operator()(const TReaderEntry& a, const TReaderEntry& b) const {
        if (a.DocId != b.DocId) {
            return a.DocId > b.DocId;
        }
        // Higher list index = more recent, should come out first
        return a.ListIndex < b.ListIndex;
    }
};

////////////////////////////////////////////////////////////////////////////////
// Helper: write a single entry to a writer based on type.
////////////////////////////////////////////////////////////////////////////////

void WriteEntry(TPostingListWriter& writer, EPostingListType type,
                ui64 docId, ui32 freq, const TVector<ui32>& positions) {
    switch (type) {
        case EPostingListType::Basic:
            writer.AddDoc(docId);
            break;
        case EPostingListType::Weighted:
            writer.AddDoc(docId, freq);
            break;
        case EPostingListType::Positional:
            writer.AddDoc(docId, freq, positions);
            break;
    }
}

// Write an entry from a positioned reader, reading freq/positions only when appropriate.
void WriteEntryFromReader(TPostingListWriter& writer, EPostingListType type,
                          TPostingListReader& reader) {
    switch (type) {
        case EPostingListType::Basic:
            writer.AddDoc(reader.DocId());
            break;
        case EPostingListType::Weighted:
            writer.AddDoc(reader.DocId(), reader.Freq());
            break;
        case EPostingListType::Positional:
            writer.AddDoc(reader.DocId(), reader.Freq(), reader.Positions());
            break;
    }
}

////////////////////////////////////////////////////////////////////////////////
// Helper: populate a TReaderEntry from a positioned reader.
////////////////////////////////////////////////////////////////////////////////

TReaderEntry MakeEntry(TPostingListReader& reader, EPostingListType type, ui32 listIndex) {
    TReaderEntry entry;
    entry.DocId = reader.DocId();
    entry.ListIndex = listIndex;
    entry.Freq = 0;

    if (type == EPostingListType::Weighted || type == EPostingListType::Positional) {
        entry.Freq = reader.Freq();
    }
    if (type == EPostingListType::Positional) {
        entry.Positions = reader.Positions();
    }

    return entry;
}

} // anonymous namespace

////////////////////////////////////////////////////////////////////////////////
// MergeAddPostingLists
////////////////////////////////////////////////////////////////////////////////

TString MergeAddPostingLists(
    const TVector<TStringBuf>& lists,
    EPostingListType type,
    TVector<TSkipPointer>& outSkipPointers,
    ui32 skipInterval)
{
    outSkipPointers.clear();

    if (lists.empty()) {
        return {};
    }

    // Single list optimization: just re-encode (or copy) as-is
    if (lists.size() == 1) {
        TPostingListWriter writer(type, skipInterval);
        TPostingListReader reader(lists[0], type);
        while (reader.Next()) {
            WriteEntryFromReader(writer, type, reader);
        }
        TString result = writer.Finish();
        outSkipPointers = writer.GetSkipPointers();
        return result;
    }

    // Initialize readers and seed the priority queue
    TVector<TPostingListReader> readers;
    readers.reserve(lists.size());

    using TMinHeap = std::priority_queue<TReaderEntry, TVector<TReaderEntry>, TReaderEntryGreater>;
    TMinHeap heap;

    for (ui32 i = 0; i < lists.size(); ++i) {
        readers.emplace_back(lists[i], type);
        if (readers.back().Next()) {
            heap.push(MakeEntry(readers.back(), type, i));
        }
    }

    TPostingListWriter writer(type, skipInterval);

    while (!heap.empty()) {
        // Pop the entry with smallest doc_id (and highest list index for ties)
        TReaderEntry best = heap.top();
        heap.pop();

        // Advance the reader that produced this entry
        if (readers[best.ListIndex].Next()) {
            heap.push(MakeEntry(readers[best.ListIndex], type, best.ListIndex));
        }

        // Skip duplicate doc_ids from older lists
        while (!heap.empty() && heap.top().DocId == best.DocId) {
            TReaderEntry dup = heap.top();
            heap.pop();
            // Advance the reader that produced the duplicate
            if (readers[dup.ListIndex].Next()) {
                heap.push(MakeEntry(readers[dup.ListIndex], type, dup.ListIndex));
            }
        }

        WriteEntry(writer, type, best.DocId, best.Freq, best.Positions);
    }

    TString result = writer.Finish();
    outSkipPointers = writer.GetSkipPointers();
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// MergeDelPostingLists
////////////////////////////////////////////////////////////////////////////////

TString MergeDelPostingLists(
    const TVector<TStringBuf>& lists,
    TVector<TSkipPointer>& outSkipPointers,
    ui32 skipInterval)
{
    outSkipPointers.clear();

    if (lists.empty()) {
        return {};
    }

    const EPostingListType type = EPostingListType::Basic;

    if (lists.size() == 1) {
        TPostingListWriter writer(type, skipInterval);
        TPostingListReader reader(lists[0], type);
        while (reader.Next()) {
            writer.AddDoc(reader.DocId());
        }
        TString result = writer.Finish();
        outSkipPointers = writer.GetSkipPointers();
        return result;
    }

    // Initialize readers and seed the priority queue
    TVector<TPostingListReader> readers;
    readers.reserve(lists.size());

    // For DEL lists we only need min-heap by doc_id; list index does not matter
    // since we deduplicate anyway.
    using TMinHeap = std::priority_queue<TReaderEntry, TVector<TReaderEntry>, TReaderEntryGreater>;
    TMinHeap heap;

    for (ui32 i = 0; i < lists.size(); ++i) {
        readers.emplace_back(lists[i], type);
        if (readers.back().Next()) {
            heap.push(MakeEntry(readers.back(), type, i));
        }
    }

    TPostingListWriter writer(type, skipInterval);

    while (!heap.empty()) {
        TReaderEntry best = heap.top();
        heap.pop();

        // Advance the source reader
        if (readers[best.ListIndex].Next()) {
            heap.push(MakeEntry(readers[best.ListIndex], type, best.ListIndex));
        }

        // Deduplicate: skip all entries with the same doc_id
        while (!heap.empty() && heap.top().DocId == best.DocId) {
            TReaderEntry dup = heap.top();
            heap.pop();
            if (readers[dup.ListIndex].Next()) {
                heap.push(MakeEntry(readers[dup.ListIndex], type, dup.ListIndex));
            }
        }

        writer.AddDoc(best.DocId);
    }

    TString result = writer.Finish();
    outSkipPointers = writer.GetSkipPointers();
    return result;
}

////////////////////////////////////////////////////////////////////////////////
// SubtractPostingLists
////////////////////////////////////////////////////////////////////////////////

TString SubtractPostingLists(
    TStringBuf addList,
    TStringBuf delList,
    EPostingListType type,
    TVector<TSkipPointer>& outSkipPointers,
    bool keepDel,
    TString* outRemainingDel,
    TVector<TSkipPointer>* outDelSkipPointers,
    ui32 skipInterval)
{
    outSkipPointers.clear();

    if (outRemainingDel) {
        outRemainingDel->clear();
    }
    if (outDelSkipPointers) {
        outDelSkipPointers->clear();
    }

    TPostingListWriter addWriter(type, skipInterval);

    // Optional writer for remaining DEL entries
    TPostingListWriter delWriter(EPostingListType::Basic, skipInterval);
    bool hasDelWriter = keepDel && outRemainingDel;

    TPostingListReader addReader(addList, type);
    TPostingListReader delReader(delList, EPostingListType::Basic);

    bool addValid = addReader.Next();
    bool delValid = delReader.Next();

    while (addValid && delValid) {
        ui64 addDocId = addReader.DocId();
        ui64 delDocId = delReader.DocId();

        if (addDocId < delDocId) {
            // ADD entry not in DEL -> keep
            WriteEntryFromReader(addWriter, type, addReader);
            addValid = addReader.Next();
        } else if (addDocId > delDocId) {
            // DEL entry with no matching ADD
            if (hasDelWriter) {
                delWriter.AddDoc(delDocId);
            }
            delValid = delReader.Next();
        } else {
            // Match: entry in both ADD and DEL -> remove from output ADD
            if (hasDelWriter) {
                delWriter.AddDoc(delDocId);
            }
            addValid = addReader.Next();
            delValid = delReader.Next();
        }
    }

    // Remaining ADD entries
    while (addValid) {
        WriteEntryFromReader(addWriter, type, addReader);
        addValid = addReader.Next();
    }

    // Remaining DEL entries
    while (delValid) {
        if (hasDelWriter) {
            delWriter.AddDoc(delReader.DocId());
        }
        delValid = delReader.Next();
    }

    TString result = addWriter.Finish();
    outSkipPointers = addWriter.GetSkipPointers();

    if (hasDelWriter) {
        *outRemainingDel = delWriter.Finish();
        if (outDelSkipPointers) {
            *outDelSkipPointers = delWriter.GetSkipPointers();
        }
    }

    return result;
}

////////////////////////////////////////////////////////////////////////////////
// CompactPostingLists
////////////////////////////////////////////////////////////////////////////////

TCompactionResult CompactPostingLists(
    const TVector<TStringBuf>& addLists,
    const TVector<TStringBuf>& delLists,
    EPostingListType type,
    bool isFinalLevel,
    ui32 skipInterval)
{
    TCompactionResult result;

    // Step 1: Merge all ADD lists
    TVector<TSkipPointer> mergedAddSkip;
    TString mergedAdd = MergeAddPostingLists(addLists, type, mergedAddSkip, skipInterval);

    // Step 2: Merge all DEL lists
    TVector<TSkipPointer> mergedDelSkip;
    TString mergedDel = MergeDelPostingLists(delLists, mergedDelSkip, skipInterval);

    // Step 3: Subtract DEL from ADD
    if (mergedDel.empty()) {
        // No deletions: merged ADD is the final result
        result.AddData = std::move(mergedAdd);
        result.AddSkipPointers = std::move(mergedAddSkip);
        // No DEL data to output
        return result;
    }

    if (mergedAdd.empty()) {
        // No additions: if keepDel, pass through the DEL list
        if (!isFinalLevel) {
            result.DelData = std::move(mergedDel);
            result.DelSkipPointers = std::move(mergedDelSkip);
        }
        return result;
    }

    bool keepDel = !isFinalLevel;
    TString remainingDel;
    TVector<TSkipPointer> remainingDelSkip;

    result.AddData = SubtractPostingLists(
        mergedAdd,
        mergedDel,
        type,
        result.AddSkipPointers,
        keepDel,
        keepDel ? &remainingDel : nullptr,
        keepDel ? &remainingDelSkip : nullptr,
        skipInterval);

    if (keepDel) {
        result.DelData = std::move(remainingDel);
        result.DelSkipPointers = std::move(remainingDelSkip);
    }

    return result;
}

} // namespace NKikimr::NPostingList
