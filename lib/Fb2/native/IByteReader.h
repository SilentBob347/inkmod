// IByteReader.h
//
// Minimal seekable byte-stream interface. The FB2 parser is written entirely
// against this interface and never touches Arduino/FS.h directly, so:
//   - it compiles and unit-tests on a desktop toolchain (g++/clang++), and
//   - wiring it into the firmware is a ~15-line adapter (see FsFileReader.h)
//     around fs::File, which already exposes read()/seek()/position()/size().
//
// No dynamic allocation happens in implementations of this interface that
// wrap a real file — everything is a pass-through to the SD driver.

#pragma once
#include <cstdint>
#include <cstddef>

class IByteReader {
public:
    virtual ~IByteReader() = default;

    // Reads up to `len` bytes into `buf`. Returns the number of bytes
    // actually read (0 at EOF). Never blocks indefinitely.
    virtual size_t read(uint8_t* buf, size_t len) = 0;

    // Absolute seek from start of stream. Returns false on failure.
    virtual bool seek(uint32_t pos) = 0;

    // Current absolute read position.
    virtual uint32_t position() const = 0;

    // Total stream size in bytes.
    virtual uint32_t size() const = 0;

    bool eof() const { return position() >= size(); }
};
