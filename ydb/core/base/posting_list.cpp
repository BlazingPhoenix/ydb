#include "posting_list.h"

#include <util/generic/yexception.h>

namespace NKikimr::NPostingList {

////////////////////////////////////////////////////////////////////////////////
// Varint encoding/decoding
////////////////////////////////////////////////////////////////////////////////

void EncodeVarint32(ui32 value, TString& output) {
    while (value >= 0x80) {
        output += static_cast<char>(static_cast<ui8>(value | 0x80));
        value >>= 7;
    }
    output += static_cast<char>(static_cast<ui8>(value));
}

void EncodeVarint64(ui64 value, TString& output) {
    while (value >= 0x80) {
        output += static_cast<char>(static_cast<ui8>(value | 0x80));
        value >>= 7;
    }
    output += static_cast<char>(static_cast<ui8>(value));
}

size_t DecodeVarint32(const char* data, size_t size, ui32& result) {
    result = 0;
    ui32 shift = 0;
    for (size_t i = 0; i < size && shift < 35; ++i) {
        ui8 byte = static_cast<ui8>(data[i]);
        result |= static_cast<ui32>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return i + 1;
        }
        shift += 7;
    }
    return 0; // error: truncated or too long
}

size_t DecodeVarint64(const char* data, size_t size, ui64& result) {
    result = 0;
    ui32 shift = 0;
    for (size_t i = 0; i < size && shift < 70; ++i) {
        ui8 byte = static_cast<ui8>(data[i]);
        result |= static_cast<ui64>(byte & 0x7F) << shift;
        if ((byte & 0x80) == 0) {
            return i + 1;
        }
        shift += 7;
    }
    return 0; // error: truncated or too long
}

////////////////////////////////////////////////////////////////////////////////
// PostingListWriter
////////////////////////////////////////////////////////////////////////////////

TPostingListWriter::TPostingListWriter(EPostingListType type, ui32 skipInterval)
    : Type(type)
    , SkipInterval(skipInterval)
{}

void TPostingListWriter::MaybeAddSkipPointer(ui64 docId) {
    if (SkipInterval > 0 && DocCount > 0 && (DocCount % SkipInterval) == 0) {
        TSkipPointer sp;
        sp.DocId = docId;
        sp.Offset = static_cast<ui32>(Data.size());
        SkipPointers.push_back(sp);
    }
}

void TPostingListWriter::AddDoc(ui64 docId) {
    Y_ENSURE(Type == EPostingListType::Basic,
        "AddDoc(docId) is only valid for Basic posting lists");
    Y_ENSURE(DocCount == 0 || docId > LastDocId,
        "doc_ids must be added in strictly increasing order");

    MaybeAddSkipPointer(docId);

    ui64 delta = (DocCount == 0) ? docId : (docId - LastDocId);
    EncodeVarint64(delta, Data);

    LastDocId = docId;
    ++DocCount;
}

void TPostingListWriter::AddDoc(ui64 docId, ui32 freq) {
    Y_ENSURE(Type == EPostingListType::Weighted,
        "AddDoc(docId, freq) is only valid for Weighted posting lists");
    Y_ENSURE(DocCount == 0 || docId > LastDocId,
        "doc_ids must be added in strictly increasing order");

    MaybeAddSkipPointer(docId);

    ui64 delta = (DocCount == 0) ? docId : (docId - LastDocId);
    EncodeVarint64(delta, Data);
    EncodeVarint32(freq, Data);

    LastDocId = docId;
    ++DocCount;
}

void TPostingListWriter::AddDoc(ui64 docId, ui32 freq, const TVector<ui32>& positions) {
    Y_ENSURE(Type == EPostingListType::Positional,
        "AddDoc(docId, freq, positions) is only valid for Positional posting lists");
    Y_ENSURE(DocCount == 0 || docId > LastDocId,
        "doc_ids must be added in strictly increasing order");
    Y_ENSURE(freq == positions.size(),
        "freq must equal number of positions");

    MaybeAddSkipPointer(docId);

    ui64 delta = (DocCount == 0) ? docId : (docId - LastDocId);
    EncodeVarint64(delta, Data);
    EncodeVarint32(freq, Data);

    // Positions are sorted, first as full varint, rest as deltas
    ui32 lastPos = 0;
    for (ui32 i = 0; i < positions.size(); ++i) {
        if (i > 0) {
            Y_ENSURE(positions[i] >= lastPos,
                "positions must be in non-decreasing order");
        }
        ui32 posDelta = (i == 0) ? positions[i] : (positions[i] - lastPos);
        EncodeVarint32(posDelta, Data);
        lastPos = positions[i];
    }

    LastDocId = docId;
    ++DocCount;
}

TString TPostingListWriter::Finish() {
    return Data;
}

const TVector<TSkipPointer>& TPostingListWriter::GetSkipPointers() const {
    return SkipPointers;
}

////////////////////////////////////////////////////////////////////////////////
// PostingListReader
////////////////////////////////////////////////////////////////////////////////

TPostingListReader::TPostingListReader(
    TStringBuf data,
    EPostingListType type,
    const TVector<TSkipPointer>& skipPointers)
    : Data(data)
    , Type(type)
    , SkipPointers(skipPointers)
{}

bool TPostingListReader::DecodeNext() {
    if (Pos >= Data.size()) {
        IsValid = false;
        return false;
    }

    // Decode doc_id delta
    ui64 delta = 0;
    size_t consumed = DecodeVarint64(Data.data() + Pos, Data.size() - Pos, delta);
    Y_ENSURE(consumed > 0, "Failed to decode doc_id varint");
    Pos += consumed;

    CurrentDocId = (EntryIndex == 0) ? delta : (CurrentDocId + delta);
    CurrentFreq = 0;
    CurrentPositions.clear();

    if (Type == EPostingListType::Weighted || Type == EPostingListType::Positional) {
        // Decode freq
        ui32 freq = 0;
        consumed = DecodeVarint32(Data.data() + Pos, Data.size() - Pos, freq);
        Y_ENSURE(consumed > 0, "Failed to decode freq varint");
        Pos += consumed;
        CurrentFreq = freq;

        if (Type == EPostingListType::Positional) {
            // Decode positions
            CurrentPositions.reserve(freq);
            ui32 lastPos = 0;
            for (ui32 i = 0; i < freq; ++i) {
                ui32 posDelta = 0;
                consumed = DecodeVarint32(Data.data() + Pos, Data.size() - Pos, posDelta);
                Y_ENSURE(consumed > 0, "Failed to decode position varint");
                Pos += consumed;

                ui32 pos = (i == 0) ? posDelta : (lastPos + posDelta);
                CurrentPositions.push_back(pos);
                lastPos = pos;
            }
        }
    }

    ++EntryIndex;
    IsValid = true;
    return true;
}

bool TPostingListReader::Next() {
    return DecodeNext();
}

bool TPostingListReader::Advance(ui64 targetDocId) {
    // If already positioned at or past target, return current validity
    if (IsValid && CurrentDocId >= targetDocId) {
        return true;
    }

    // Use skip pointers to jump ahead if possible.
    // Skip pointers are sorted by DocId. Find the last one with DocId < targetDocId
    // (we want to land before or at target, then scan forward).
    if (!SkipPointers.empty()) {
        size_t bestSkip = SkipPointers.size();
        for (size_t i = 0; i < SkipPointers.size(); ++i) {
            if (SkipPointers[i].DocId < targetDocId) {
                bestSkip = i;
            } else if (SkipPointers[i].DocId == targetDocId) {
                bestSkip = i;
                break;
            } else {
                break;
            }
        }

        if (bestSkip < SkipPointers.size()) {
            const auto& sp = SkipPointers[bestSkip];
            // Only use skip pointer if it advances us past current position
            if (!IsValid || sp.DocId > CurrentDocId) {
                // Jump to skip pointer offset and decode the entry there.
                // The entry at this offset is delta-encoded, but we know the
                // absolute doc_id from the skip pointer, so we decode the delta
                // (to advance Pos) and then use sp.DocId as the actual value.
                Pos = sp.Offset;

                ui64 delta = 0;
                size_t consumed = DecodeVarint64(Data.data() + Pos, Data.size() - Pos, delta);
                Y_ENSURE(consumed > 0, "Failed to decode doc_id varint at skip pointer");
                Pos += consumed;

                CurrentDocId = sp.DocId;
                CurrentFreq = 0;
                CurrentPositions.clear();
                EntryIndex = 1; // non-zero so subsequent DecodeNext uses delta mode
                IsValid = true;

                // Decode freq and positions if needed
                if (Type == EPostingListType::Weighted || Type == EPostingListType::Positional) {
                    ui32 freq = 0;
                    consumed = DecodeVarint32(Data.data() + Pos, Data.size() - Pos, freq);
                    Y_ENSURE(consumed > 0, "Failed to decode freq varint at skip pointer");
                    Pos += consumed;
                    CurrentFreq = freq;

                    if (Type == EPostingListType::Positional) {
                        CurrentPositions.reserve(freq);
                        ui32 lastPos = 0;
                        for (ui32 i = 0; i < freq; ++i) {
                            ui32 posDelta = 0;
                            consumed = DecodeVarint32(Data.data() + Pos, Data.size() - Pos, posDelta);
                            Y_ENSURE(consumed > 0, "Failed to decode position varint at skip pointer");
                            Pos += consumed;
                            ui32 pos = (i == 0) ? posDelta : (lastPos + posDelta);
                            CurrentPositions.push_back(pos);
                            lastPos = pos;
                        }
                    }
                }

                if (CurrentDocId >= targetDocId) {
                    return true;
                }
            }
        }
    }

    // Linear scan from current position
    while (DecodeNext()) {
        if (CurrentDocId >= targetDocId) {
            return true;
        }
    }
    return false;
}

bool TPostingListReader::Valid() const {
    return IsValid;
}

ui64 TPostingListReader::DocId() const {
    Y_ENSURE(IsValid, "Reader is not positioned at a valid entry");
    return CurrentDocId;
}

ui32 TPostingListReader::Freq() const {
    Y_ENSURE(IsValid, "Reader is not positioned at a valid entry");
    Y_ENSURE(Type == EPostingListType::Weighted || Type == EPostingListType::Positional,
        "Freq() is only valid for Weighted or Positional posting lists");
    return CurrentFreq;
}

const TVector<ui32>& TPostingListReader::Positions() const {
    Y_ENSURE(IsValid, "Reader is not positioned at a valid entry");
    Y_ENSURE(Type == EPostingListType::Positional,
        "Positions() is only valid for Positional posting lists");
    return CurrentPositions;
}

} // namespace NKikimr::NPostingList
