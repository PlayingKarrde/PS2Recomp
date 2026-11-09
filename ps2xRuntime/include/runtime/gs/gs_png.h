#pragma once

// FPWR: a small PNG codec for the texture dump and the texture packs (task t1b; docs/formats/fprmod.md "Texture packs").
//
// Why it exists. The dumper writes the game's textures as PNG files, and the texture pack loader reads PNG files an author made.
// Both live in the GPU renderer's library, which has no zlib, so this file carries its own inflate (the decoder side, about 150
// lines, written after the public-domain "puff" layout) and a deflate that uses fixed Huffman codes (the encoder side; the files are
// for people and scripts, not to be small). It is independent of SDL, of the mod manager and of the renderer.
//
// What it reads: non-interlaced PNG, all colour types (grey, RGB, paletted, grey+alpha, RGBA) at 8 bits, 16 bits (the high byte is
// kept) and, for grey and paletted, 1, 2 and 4 bits. Every chunk's CRC and the zlib checksum are checked. What it refuses has a
// one-line reason in `error` (written for a texture author: "interlaced PNGs are not supported; save without interlacing").
// Gamma, colour profiles and text chunks are ignored. A decoded image is limited to 16384 x 16384 and 64 Mi pixels.
//
// Threads: every function is safe to call from any thread (no shared state).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace GSPng
{
    // What the header of a PNG file says (without decoding the pixels).
    struct Info
    {
        uint32_t width = 0, height = 0;
        uint8_t bitDepth = 0;
        uint8_t colorType = 0; // 0 grey, 2 RGB, 3 paletted, 4 grey + alpha, 6 RGBA
        bool interlaced = false;
    };
    bool ReadInfo(const uint8_t *data, size_t size, Info &out, std::string &error);

    // A decoded image. Rgba8: four bytes per pixel (r, g, b, a), straight alpha. Index8: one byte per pixel, the raw sample of a
    // grey or paletted image (for a paletted image that is the palette index, not the colour).
    struct Image
    {
        enum class Format : uint8_t { Rgba8, Index8 };
        uint32_t width = 0, height = 0;
        Format format = Format::Rgba8;
        std::vector<uint8_t> pixels; // rows without padding
        bool wasPaletted = false;    // the file was a paletted PNG (information for messages)
    };

    // Any PNG, converted to RGBA8.
    bool DecodeRgba(const uint8_t *data, size_t size, Image &out, std::string &error);
    // A grey or paletted PNG (any bit depth up to 8), as its raw samples. Other colour types are refused with a reason.
    bool DecodeIndex(const uint8_t *data, size_t size, Image &out, std::string &error);

    // colorType 6: RGBA8 (`pixels` = w * h * 4 bytes); 3: paletted 8 bit (`pixels` = w * h indices; `plte` = RGB triples, `trns` = the
    // alpha of each entry or empty); 0: grey 8 bit. Rows have no padding and no filter bytes.
    std::vector<uint8_t> Encode(uint32_t width, uint32_t height, int colorType, const std::vector<uint8_t> &pixels,
                                const std::vector<uint8_t> &plte = {}, const std::vector<uint8_t> &trns = {});

    // zlib building blocks, exposed for the unit tests.
    uint32_t Crc32(const uint8_t *data, size_t size, uint32_t crc = 0);
    // Inflates a raw deflate stream. `maxOut` bounds the output (a larger stream is an error). False with a reason on failure.
    bool Inflate(const uint8_t *data, size_t size, std::vector<uint8_t> &out, size_t maxOut, std::string &error, size_t *consumed = nullptr);
    void DeflateFixed(const uint8_t *data, size_t size, std::vector<uint8_t> &out);
}
