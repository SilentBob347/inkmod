// FsFileReader.h
//
// Adapter around HalFile (this firmware's storage HAL, see HalStorage.h) so
// Fb2Parser can be handed a real on-device file without the parser itself
// ever including HalStorage.h. CrossPoint's other native modules (ZipFile,
// Epub) all go through HalFile/Storage rather than raw fs::File/SD.open, so
// this mirrors that convention instead of the upstream module's original
// fs::File-based adapter.
//
// Usage:
//
//     HalFile f;
//     if (Storage.openFileForRead("FB2", path, f)) {
//         FsFileReader reader(f);
//         Fb2Parser parser;
//         Fb2ScanResult scanResult;
//         parser.scan(reader, scanResult);
//     }

#pragma once

#include <HalStorage.h>

#include "IByteReader.h"

class FsFileReader final : public IByteReader {
public:
    explicit FsFileReader(HalFile& file) : file_(file) {}

    size_t read(uint8_t* buf, size_t len) override {
        const int got = file_.read(buf, len);
        return got > 0 ? static_cast<size_t>(got) : 0;
    }
    bool seek(uint32_t pos) override {
        return file_.seek(static_cast<size_t>(pos));
    }
    uint32_t position() const override {
        return static_cast<uint32_t>(file_.position());
    }
    uint32_t size() const override {
        return static_cast<uint32_t>(file_.fileSize());
    }

private:
    HalFile& file_;
};
