#include "MetroBinArchive.h"
#include "MetroReflection.h"

MetroBinArchive::MetroBinArchive(const CharString& name, const MemStream& stream, const size_t headerSize) {
    // Get raw memory stream
    mFileName = name;
    mFileStream = stream;

    // Search for header
    mHeaderSize = 0;

    if (headerSize != kHeaderNotExist) {
        if (headerSize == kHeaderDoAutoSearch) {
            // Try to autosearch first chunk location, to get flags and header size
            // TODO: tricky and unreliable method, may fail
            size_t firstChunkPos = 0;

            // Try to search for .bin flags
            size_t _maxOffsetToSeek = 0x10;

            for (size_t offs = 0; offs <= _maxOffsetToSeek; offs++) {
                mFileStream.SetCursor(offs);
                uint32_t value = mFileStream.ReadTyped<uint32_t>();
                if (value == 1) { // looking for chunkIdx == 1
                    firstChunkPos = offs;
                    break;
                }
            }

            // Restore cursor position
            mFileStream.SetCursor(0);

            //#NOTE_SK: when no chunk marker is found firstChunkPos stays 0, and the old
            //          `firstChunkPos - 1` underflowed size_t into a colossal header size.
            //          A zero header is the right answer there - the flags byte sits first.
            mHeaderSize = (firstChunkPos > 0) ? (firstChunkPos - 0x1 /*bin flags*/) : 0;
        } else {
            mHeaderSize = headerSize;
        }
    }

    // Read .bin flags
    mFileStream.SetCursor(GetOffsetBinFlags());
    mBinFlags = mFileStream.ReadTyped<uint8_t>();

    // Read chunks
    if (TestBit(mBinFlags, MetroReflectionFlags::StringsTable)) {
        //#NOTE_SK: guard the walk - a wrong header guess produces garbage sizes here, and the
        //          loop used to spin forever appending chunks until it fell over
        while (mFileStream.Remains() >= (sizeof(uint32_t) * 2)) {
            const size_t chunkId = mFileStream.ReadTyped<uint32_t>();
            const size_t chunkSize = mFileStream.ReadTyped<uint32_t>();

            if (chunkSize > mFileStream.Remains()) {
                mChunks.clear();
                break;
            }

            mChunks.push_back({
                chunkId,
                mFileStream.GetCursor(),
                chunkSize
            });

            mFileStream.SkipBytes(chunkSize);
        }

        // read strings table chunk
        if (this->GetNumChunks() == 2) {
            this->ReadStringsTable();
        }
    }
    // Restore cursor position
    mFileStream.SetCursor(0);
}

MetroReflectionReader MetroBinArchive::ReflectionReader() const {
    MetroReflectionReader result;

    if (this->HasChunks()) {
        const ChunkData& chunk = this->GetFirstChunk();

        result = MetroReflectionReader(mFileStream.Substream(chunk.offset, chunk.size), mBinFlags);
    } else {
        result = MetroReflectionReader(mFileStream.Substream(1, mFileStream.Length() - 1), mBinFlags);
    }

    if (!mSTable.data.empty()) {
        result.SetSTable(&mSTable);
    }

    return std::move(result);
}

void MetroBinArchive::ReadStringsTable() {
    const ChunkData& stableChunk = this->GetLastChunk();

    MemStream stream = mFileStream.Substream(stableChunk.offset, stableChunk.size);
    const size_t numStrings = stream.ReadTyped<uint32_t>();

    const size_t dataSize = stream.Remains();

    //#NOTE_SK: the count comes straight off the wire, and on an archive whose layout we
    //          guessed wrong it is nonsense. The walk below used to run off the end of the
    //          buffer and segfault - every string needs at least its terminator, so that
    //          gives us a hard ceiling to sanity check against.
    if (!dataSize || numStrings > dataSize) {
        return;
    }

    mSTable.data.resize(dataSize + 1);
    stream.ReadToBuffer(mSTable.data.data(), dataSize);
    mSTable.data[dataSize] = 0;     // guarantee the last string is terminated

    mSTable.strings.reserve(numStrings);

    const char* s = mSTable.data.data();
    const char* end = s + dataSize;
    for (size_t i = 0; i < numStrings && s < end; ++i) {
        mSTable.strings.push_back(s);
        while (s < end && *s++);
    }
}
