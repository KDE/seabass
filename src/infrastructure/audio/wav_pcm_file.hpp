// SPDX-FileCopyrightText: 2026 Sebastian Kügler <sebas@kde.org>
//
// SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>

#include "domain/recording_length.hpp"

namespace seabass::infrastructure::stick_backup
{
class PosixArchiveFile;
}

namespace seabass::infrastructure::audio
{

// Where the samples of a PCM WAV are and what they are.
struct WavLayout
{
    int sampleRate = 0;
    int channels = 0;
    // 16 or 24: the bits each sample holds, which is also the bits it
    // takes in the file (a 20 bit recording in 24 bit slots is refused).
    int bitsPerSample = 0;
    // Bytes of one frame: one sample of every channel.
    int blockAlign = 0;
    std::uint64_t dataOffset = 0;
    std::uint64_t dataBytes = 0;
    std::uint64_t frames = 0;
    // An RF64 or BW64 file, whose sizes are in its ds64 chunk.
    bool rf64 = false;
};

// The sample rates a recording is taken at. Others are refused: no
// player Seabass knows records at another, and none of them is tested.
inline constexpr int RecordingSampleRates[] = {44100, 48000, 88200, 96000};

// How a WAV whose header the recorder never finished is read.
enum class WavReading
{
    // As its header declares: an unfinished header is not read
    // (WavPcmFile::State::Unfinished).
    AsDeclared,
    // An unfinished header (see WavUnfinished) is read to the end of the
    // file: every whole frame after the data chunk's header, the last
    // partial frame left out. Only when the person asked for it. A file
    // whose header is finished is read as declared all the same.
    ToEndOfFile,
};

// A WAV whose header was not completed, in one of the two forms that are
// evidence of a recorder that stopped before it wrote the length (a player
// switched off or a stick pulled while recording; the file cannot say which).
struct WavUnfinished
{
    enum class Kind
    {
        NoSize,   // the data size is 0 or 0xFFFFFFFF in a plain RIFF
        PastEnd,  // the data size is more than the file holds (cut short)
    };
    Kind kind = Kind::NoSize;
    // Whole frames from the start of the data chunk's audio to the end of
    // the file, and the bytes of a last, partial frame after them.
    std::uint64_t framesHeld = 0;
    std::uint64_t partialFrameBytes = 0;
    // What the header declares, in whole frames (0 for NoSize).
    std::uint64_t framesDeclared = 0;
    // PastEnd: how many bytes less the file holds than the header
    // declares, and whether the declared length is one a recording can
    // have (declaredLengthUsable()). 0 and true for NoSize.
    std::uint64_t bytesMissing = 0;
    bool declaredUsable = true;
};

// A declared length longer than this is not one a recording has: it is
// not said as a length ("the header's length is not usable").
inline constexpr std::uint64_t MaxUsableDeclaredSeconds = 100ULL * 3600;
// Nor is an RF64 or BW64 data size (its ds64 chunk's, 64 bits) of more
// than this many times what the file holds: a recording cut to a
// thousandth of its length is not one, a garbled 64 bit size is.
inline constexpr std::uint64_t MaxUsableRf64DeclaredFactor = 1000;

// Whether a header that declares `bytesDeclared` (`framesDeclared` whole
// frames at `sampleRate`) for a file holding `bytesHeld` declares a usable
// length: at most MaxUsableDeclaredSeconds, and for RF64 at most
// MaxUsableRf64DeclaredFactor times what is held.
bool declaredLengthUsable(std::uint64_t framesDeclared, int sampleRate, std::uint64_t bytesDeclared,
                          std::uint64_t bytesHeld, bool rf64);

// The lengths of an unfinished header, for domain::unfinishedHeaderText().
domain::UnfinishedHeaderLengths unfinishedLengths(const WavUnfinished &unfinished, int sampleRate);

// A PCM WAV as DJ hardware records it, read piece by piece: the header is
// read when the file is opened, the samples only as asked for, so a
// recording of hours is never in memory. Nothing is ever written to it.
//
// Reads the plain RIFF form (sizes of 32 bits, so up to 4 GB) and the
// RF64 and BW64 forms that recorders switch to past it. PCM samples of 16
// or 24 bits, one or two channels, at one of RecordingSampleRates, in a
// WAVE_FORMAT_PCM or a WAVE_FORMAT_EXTENSIBLE (PCM sub format) header.
//
// After the data chunk the file may hold: whole chunks (a recorder's cue,
// LIST or bext) to the end, the last one without its pad byte; zero bytes
// to the end (padding), no more than one second of audio of them; an ID3v1
// block (128 bytes from "TAG") at the very end; and after a data chunk of
// an odd size, chunks that start right after it without its pad byte, when
// they then end exactly at the end of the file.
//
// Unfinished (state() Unfinished, unfinished() says how): a data size of 0
// or 0xFFFFFFFF in a plain RIFF, and a data size more than the file holds.
// Read to the end of the file only with WavReading::ToEndOfFile;
// unfinished() is set either way, unless the file holds no whole frame
// (then refused, holdsNoAudio()).
//
// A plain RIFF over 4 GB is unfinished (NoSize) when its data size is 0
// or 0xFFFFFFFF, a recording of hours the player never finished.
//
// Refused, with the reason in error(): a file that is not a WAV, another
// sample format (float, 8 or 32 bit, compressed), a plain RIFF over 4 GB
// with any other data size,
// an RF64 or BW64 whose ds64 chunk is missing or gives a data size of 0,
// and a file that holds more than its header describes in any other way,
// zero bytes of more than a second included (the reason then gives the
// length the header declares and the length the file could hold, and says
// only that the header does not match the file).
//
// Never throws from the constructor; readFrames() throws on a read error
// (the file was taken away meanwhile).
class WavPcmFile
{
public:
    enum class State
    {
        Open,
        Unfinished,  // read as declared, and the header is unfinished
        Refused,
    };

    explicit WavPcmFile(const std::filesystem::path &path, WavReading reading = WavReading::AsDeclared);
    ~WavPcmFile();
    WavPcmFile(const WavPcmFile &) = delete;
    WavPcmFile &operator=(const WavPcmFile &) = delete;

    State state() const { return m_state; }
    bool isOpen() const { return m_state == State::Open; }
    // Why not open, in words; empty when open.
    const std::string &error() const { return m_error; }
    // Set whenever the header is unfinished, read to the end or not. The
    // sample format is in layout() then too.
    const std::optional<WavUnfinished> &unfinished() const { return m_unfinished; }
    // Refused because there is not one whole frame of audio in it, or no
    // data chunk at all.
    bool holdsNoAudio() const { return m_noAudio; }
    // When open: what is read. With WavReading::ToEndOfFile and an
    // unfinished header, `frames` is unfinished()->framesHeld.
    const WavLayout &layout() const { return m_layout; }

    // The bytes of `out.size() / blockAlign` frames from `firstFrame`,
    // as the file has them (little endian, interleaved). The span must
    // hold whole frames that lie inside what is read.
    void readFrames(std::uint64_t firstFrame, std::span<std::byte> out) const;

    // How many bytes were read from the file so far. Opening it reads the
    // header, the chunks after the audio and at most one second of audio's
    // worth of zero bytes after it, never the audio itself, however long
    // a run of zero bytes follows; readFrames() adds what it reads.
    std::uint64_t bytesRead() const { return m_bytesRead; }

private:
    // Whether the bytes from `at` to the end are what may follow the
    // audio (see the class comment). `strict`: whole chunks only, ending
    // exactly at the end of the file. A run of zero bytes to the end is
    // padding only up to `maxZeroBytes`.
    bool onlyChunksFrom(std::uint64_t at, std::uint64_t fileSize, bool strict, std::uint64_t maxZeroBytes) const;
    bool zeroBytesFrom(std::uint64_t at, std::uint64_t fileSize) const;
    void readAt(std::uint64_t at, std::span<std::byte> out) const;
    void refuse(std::string why);

    std::unique_ptr<stick_backup::PosixArchiveFile> m_file;
    WavLayout m_layout;
    State m_state = State::Refused;
    std::optional<WavUnfinished> m_unfinished;
    bool m_noAudio = false;
    std::string m_error;
    mutable std::uint64_t m_bytesRead = 0;
};

}  // namespace seabass::infrastructure::audio
