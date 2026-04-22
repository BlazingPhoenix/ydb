#pragma once

#include <util/system/types.h>
#include <util/generic/string.h>
#include <util/generic/strbuf.h>
#include <util/generic/vector.h>

namespace NKikimr::NPostingList {

enum class EPostingListType : ui8 {
    Basic = 1,       // sorted doc_ids only
    Weighted = 2,    // doc_ids + freq
    Positional = 3,  // doc_ids + freq + positions
};

// Skip pointer: (doc_id, byte_offset) into posting list data
struct TSkipPointer {
    ui64 DocId = 0;
    ui32 Offset = 0;
};

////////////////////////////////////////////////////////////////////////////////
// Varint encoding/decoding
////////////////////////////////////////////////////////////////////////////////

// Encode a ui32 value as varint, appending bytes to output.
void EncodeVarint32(ui32 value, TString& output);

// Encode a ui64 value as varint, appending bytes to output.
void EncodeVarint64(ui64 value, TString& output);

// Decode a varint-encoded ui32. Returns number of bytes consumed, 0 on error.
size_t DecodeVarint32(const char* data, size_t size, ui32& result);

// Decode a varint-encoded ui64. Returns number of bytes consumed, 0 on error.
size_t DecodeVarint64(const char* data, size_t size, ui64& result);

////////////////////////////////////////////////////////////////////////////////
// PostingListWriter
////////////////////////////////////////////////////////////////////////////////

class TPostingListWriter {
public:
    explicit TPostingListWriter(EPostingListType type, ui32 skipInterval = 128);

    // Basic posting list: add doc_id only
    void AddDoc(ui64 docId);

    // Weighted posting list: add doc_id with frequency
    void AddDoc(ui64 docId, ui32 freq);

    // Positional posting list: add doc_id with frequency and positions
    void AddDoc(ui64 docId, ui32 freq, const TVector<ui32>& positions);

    // Finalize and return serialized posting list data
    TString Finish();

    // Return skip pointers built so far
    const TVector<TSkipPointer>& GetSkipPointers() const;

private:
    void MaybeAddSkipPointer(ui64 docId);

private:
    EPostingListType Type;
    ui32 SkipInterval;
    ui32 DocCount = 0;
    ui64 LastDocId = 0;
    TString Data;
    TVector<TSkipPointer> SkipPointers;
};

////////////////////////////////////////////////////////////////////////////////
// PostingListReader
////////////////////////////////////////////////////////////////////////////////

class TPostingListReader {
public:
    TPostingListReader(
        TStringBuf data,
        EPostingListType type,
        const TVector<TSkipPointer>& skipPointers = {});

    // Advance to the next entry. Must be called before first access.
    bool Next();

    // Seek to the first entry with doc_id >= target.
    // Returns true if positioned at a valid entry.
    bool Advance(ui64 targetDocId);

    // Check if iterator is positioned at a valid entry
    bool Valid() const;

    // Current doc_id
    ui64 DocId() const;

    // Current frequency (Weighted and Positional only)
    ui32 Freq() const;

    // Current positions (Positional only)
    const TVector<ui32>& Positions() const;

private:
    bool DecodeNext();

private:
    TStringBuf Data;
    EPostingListType Type;
    TVector<TSkipPointer> SkipPointers;

    size_t Pos = 0;
    bool IsValid = false;
    ui64 CurrentDocId = 0;
    ui32 CurrentFreq = 0;
    TVector<ui32> CurrentPositions;
    ui32 EntryIndex = 0; // number of entries decoded so far
};

} // namespace NKikimr::NPostingList
