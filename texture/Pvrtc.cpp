/* PVRTC4 decoder adapted from PowerVR Native SDK PVRTDecompress.cpp.
https://github.com/powervr-graphics/Native_SDK
The MIT License (MIT)
Copyright (c) Imagination Technologies Ltd.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#include "BlockDecode.hpp"

#include <algorithm>
#include <cstring>
#include <vector>

namespace neoshared::texture::detail {
namespace {

struct Pixel32
{
    uint8_t red, green, blue, alpha;
};

struct Pixel128S
{
    int32_t red, green, blue, alpha;
};

struct PVRTCWord
{
    uint32_t modulationData;
    uint32_t colorData;
};

struct PVRTCWordIndices
{
    int P[2], Q[2], R[2], S[2];
};

static Pixel32 getColorA(uint32_t colorData)
{
    Pixel32 color;

    // Opaque Color Mode - RGB 554
    if ((colorData & 0x8000) != 0)
    {
        color.red = static_cast<uint8_t>((colorData & 0x7c00) >> 10); // 5->5 bits
        color.green = static_cast<uint8_t>((colorData & 0x3e0) >> 5); // 5->5 bits
        color.blue = static_cast<uint8_t>(colorData & 0x1e) | ((colorData & 0x1e) >> 4); // 4->5 bits
        color.alpha = static_cast<uint8_t>(0xf); // 0->4 bits
    }
    // Transparent Color Mode - ARGB 3443
    else
    {
        color.red = static_cast<uint8_t>((colorData & 0xf00) >> 7) | ((colorData & 0xf00) >> 11); // 4->5 bits
        color.green = static_cast<uint8_t>((colorData & 0xf0) >> 3) | ((colorData & 0xf0) >> 7); // 4->5 bits
        color.blue = static_cast<uint8_t>((colorData & 0xe) << 1) | ((colorData & 0xe) >> 2); // 3->5 bits
        color.alpha = static_cast<uint8_t>((colorData & 0x7000) >> 11); // 3->4 bits - note 0 at right
    }

    return color;
}

static Pixel32 getColorB(uint32_t colorData)
{
    Pixel32 color;

    // Opaque Color Mode - RGB 555
    if (colorData & 0x80000000)
    {
        color.red = static_cast<uint8_t>((colorData & 0x7c000000) >> 26); // 5->5 bits
        color.green = static_cast<uint8_t>((colorData & 0x3e00000) >> 21); // 5->5 bits
        color.blue = static_cast<uint8_t>((colorData & 0x1f0000) >> 16); // 5->5 bits
        color.alpha = static_cast<uint8_t>(0xf); // 0 bits
    }
    // Transparent Color Mode - ARGB 3444
    else
    {
        color.red = static_cast<uint8_t>(((colorData & 0xf000000) >> 23) | ((colorData & 0xf000000) >> 27)); // 4->5 bits
        color.green = static_cast<uint8_t>(((colorData & 0xf00000) >> 19) | ((colorData & 0xf00000) >> 23)); // 4->5 bits
        color.blue = static_cast<uint8_t>(((colorData & 0xf0000) >> 15) | ((colorData & 0xf0000) >> 19)); // 4->5 bits
        color.alpha = static_cast<uint8_t>((colorData & 0x70000000) >> 27); // 3->4 bits - note 0 at right
    }

    return color;
}

static void interpolateColors(Pixel32 P, Pixel32 Q, Pixel32 R, Pixel32 S, Pixel128S* pPixel)
{
    uint32_t wordWidth = 4;
    uint32_t wordHeight = 4;

    Pixel128S hP = { static_cast<int32_t>(P.red), static_cast<int32_t>(P.green), static_cast<int32_t>(P.blue), static_cast<int32_t>(P.alpha) };
    Pixel128S hQ = { static_cast<int32_t>(Q.red), static_cast<int32_t>(Q.green), static_cast<int32_t>(Q.blue), static_cast<int32_t>(Q.alpha) };
    Pixel128S hR = { static_cast<int32_t>(R.red), static_cast<int32_t>(R.green), static_cast<int32_t>(R.blue), static_cast<int32_t>(R.alpha) };
    Pixel128S hS = { static_cast<int32_t>(S.red), static_cast<int32_t>(S.green), static_cast<int32_t>(S.blue), static_cast<int32_t>(S.alpha) };

    Pixel128S QminusP = { hQ.red - hP.red, hQ.green - hP.green, hQ.blue - hP.blue, hQ.alpha - hP.alpha };
    Pixel128S SminusR = { hS.red - hR.red, hS.green - hR.green, hS.blue - hR.blue, hS.alpha - hR.alpha };

    hP.red *= wordWidth;
    hP.green *= wordWidth;
    hP.blue *= wordWidth;
    hP.alpha *= wordWidth;
    hR.red *= wordWidth;
    hR.green *= wordWidth;
    hR.blue *= wordWidth;
    hR.alpha *= wordWidth;

    for (uint32_t y = 0; y < wordHeight; y++)
    {
        Pixel128S result = { 4 * hP.red, 4 * hP.green, 4 * hP.blue, 4 * hP.alpha };
        Pixel128S dY = { hR.red - hP.red, hR.green - hP.green, hR.blue - hP.blue, hR.alpha - hP.alpha };

        for (uint32_t x = 0; x < wordWidth; x++)
        {
            pPixel[y * wordWidth + x].red = static_cast<int32_t>((result.red >> 6) + (result.red >> 1));
            pPixel[y * wordWidth + x].green = static_cast<int32_t>((result.green >> 6) + (result.green >> 1));
            pPixel[y * wordWidth + x].blue = static_cast<int32_t>((result.blue >> 6) + (result.blue >> 1));
            pPixel[y * wordWidth + x].alpha = static_cast<int32_t>((result.alpha >> 4) + (result.alpha));

            result.red += dY.red;
            result.green += dY.green;
            result.blue += dY.blue;
            result.alpha += dY.alpha;
        }

        hP.red += QminusP.red;
        hP.green += QminusP.green;
        hP.blue += QminusP.blue;
        hP.alpha += QminusP.alpha;

        hR.red += SminusR.red;
        hR.green += SminusR.green;
        hR.blue += SminusR.blue;
        hR.alpha += SminusR.alpha;
    }
}

static void unpackModulations(const PVRTCWord& word, int32_t offsetX, int32_t offsetY, int32_t modulationValues[8][8]) {
    static constexpr int32_t weights[2][4] = {{0, 3, 5, 8}, {0, 4, 14, 8}};
    auto bits = word.modulationData;
    for (uint32_t y = 0; y < 4; ++y) {
        for (uint32_t x = 0; x < 4; ++x) {
            modulationValues[y + offsetY][x + offsetX] = weights[word.colorData & 1u][bits & 3u];
            bits >>= 2;
        }
    }
}

static void pvrtcGetDecompressedPixels(const PVRTCWord& P, const PVRTCWord& Q, const PVRTCWord& R, const PVRTCWord& S, Pixel32* pColorData)
{
    int32_t modulationValues[8][8];
    Pixel128S upscaledColorA[16];
    Pixel128S upscaledColorB[16];

    uint32_t wordWidth = 4;
    uint32_t wordHeight = 4;

    // Get the modulations from each word.
    unpackModulations(P, 0, 0, modulationValues);
    unpackModulations(Q, wordWidth, 0, modulationValues);
    unpackModulations(R, 0, wordHeight, modulationValues);
    unpackModulations(S, wordWidth, wordHeight, modulationValues);

    // Bilinear upscale image data from 2x2 -> 4x4
    interpolateColors(getColorA(P.colorData), getColorA(Q.colorData), getColorA(R.colorData), getColorA(S.colorData), upscaledColorA);
    interpolateColors(getColorB(P.colorData), getColorB(Q.colorData), getColorB(R.colorData), getColorB(S.colorData), upscaledColorB);

    for (uint32_t y = 0; y < wordHeight; y++)
    {
        for (uint32_t x = 0; x < wordWidth; x++)
        {
            int32_t mod = modulationValues[x + wordWidth / 2][y + wordHeight / 2];
            bool punchthroughAlpha = false;
            if (mod > 10)
            {
                punchthroughAlpha = true;
                mod -= 10;
            }

            Pixel128S result;
            result.red = (upscaledColorA[y * wordWidth + x].red * (8 - mod) + upscaledColorB[y * wordWidth + x].red * mod) / 8;
            result.green = (upscaledColorA[y * wordWidth + x].green * (8 - mod) + upscaledColorB[y * wordWidth + x].green * mod) / 8;
            result.blue = (upscaledColorA[y * wordWidth + x].blue * (8 - mod) + upscaledColorB[y * wordWidth + x].blue * mod) / 8;
            if (punchthroughAlpha) { result.alpha = 0; }
            else
            {
                result.alpha = (upscaledColorA[y * wordWidth + x].alpha * (8 - mod) + upscaledColorB[y * wordWidth + x].alpha * mod) / 8;
            }

            // Convert the 32bit precision Result to 8 bit per channel color.

            pColorData[y + x * wordHeight].red = static_cast<uint8_t>(result.red);
            pColorData[y + x * wordHeight].green = static_cast<uint8_t>(result.green);
            pColorData[y + x * wordHeight].blue = static_cast<uint8_t>(result.blue);
            pColorData[y + x * wordHeight].alpha = static_cast<uint8_t>(result.alpha);

        }
    }
}

static uint32_t wrapWordIndex(uint32_t numWords, int word) { return ((word + numWords) % numWords); }

static uint32_t TwiddleUV(uint32_t XSize, uint32_t YSize, uint32_t XPos, uint32_t YPos)
{
    // Initially assume X is the larger size.
    uint32_t MinDimension = XSize;
    uint32_t MaxValue = YPos;
    uint32_t Twiddled = 0;
    uint32_t SrcBitPos = 1;
    uint32_t DstBitPos = 1;
    int ShiftCount = 0;

    // If Y is the larger dimension - switch the min/max values.
    if (YSize < XSize)
    {
        MinDimension = YSize;
        MaxValue = XPos;
    }

    // Step through all the bits in the "minimum" dimension
    while (SrcBitPos < MinDimension)
    {
        if (YPos & SrcBitPos) { Twiddled |= DstBitPos; }

        if (XPos & SrcBitPos) { Twiddled |= (DstBitPos << 1); }

        SrcBitPos <<= 1;
        DstBitPos <<= 2;
        ShiftCount += 1;
    }

    // Prepend any unused bits
    MaxValue >>= ShiftCount;
    Twiddled |= (MaxValue << (2 * ShiftCount));

    return Twiddled;
}

static void mapDecompressedData(Pixel32* pOutput, uint32_t width, const Pixel32* pWord, const PVRTCWordIndices& words)
{
    uint32_t wordWidth = 4;
    uint32_t wordHeight = 4;

    for (uint32_t y = 0; y < wordHeight / 2; y++)
    {
        for (uint32_t x = 0; x < wordWidth / 2; x++)
        {
            pOutput[(((words.P[1] * wordHeight) + y + wordHeight / 2) * width + words.P[0] * wordWidth + x + wordWidth / 2)] = pWord[y * wordWidth + x]; // map P

            pOutput[(((words.Q[1] * wordHeight) + y + wordHeight / 2) * width + words.Q[0] * wordWidth + x)] = pWord[y * wordWidth + x + wordWidth / 2]; // map Q

            pOutput[(((words.R[1] * wordHeight) + y) * width + words.R[0] * wordWidth + x + wordWidth / 2)] = pWord[(y + wordHeight / 2) * wordWidth + x]; // map R

            pOutput[(((words.S[1] * wordHeight) + y) * width + words.S[0] * wordWidth + x)] = pWord[(y + wordHeight / 2) * wordWidth + x + wordWidth / 2]; // map S
        }
    }
}
static void pvrtcDecompress(const uint8_t* pCompressedData, Pixel32* pDecompressedData, uint32_t width, uint32_t height)
{
    uint32_t wordWidth = 4;
    uint32_t wordHeight = 4;

    auto readWord = [pCompressedData](uint32_t index) {
        const auto* p = pCompressedData + index * 4u;
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8u) | (uint32_t(p[2]) << 16u) | (uint32_t(p[3]) << 24u);
    };
    Pixel32* pOutData = pDecompressedData;

    int i32NumXWords = static_cast<int>(width / wordWidth);
    int i32NumYWords = static_cast<int>(height / wordHeight);

    PVRTCWordIndices indices;
    Pixel32 pPixels[16];

    for (int32_t wordY = -1; wordY < i32NumYWords - 1; wordY++)
    {
        for (int32_t wordX = -1; wordX < i32NumXWords - 1; wordX++)
        {
            indices.P[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX));
            indices.P[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY));
            indices.Q[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX + 1));
            indices.Q[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY));
            indices.R[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX));
            indices.R[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY + 1));
            indices.S[0] = static_cast<int>(wrapWordIndex(i32NumXWords, wordX + 1));
            indices.S[1] = static_cast<int>(wrapWordIndex(i32NumYWords, wordY + 1));

            // Work out the offsets into the twiddle structs, multiply by two as there are two members per word.
            uint32_t WordOffsets[4] = {
                TwiddleUV(i32NumXWords, i32NumYWords, indices.P[0], indices.P[1]) * 2,
                TwiddleUV(i32NumXWords, i32NumYWords, indices.Q[0], indices.Q[1]) * 2,
                TwiddleUV(i32NumXWords, i32NumYWords, indices.R[0], indices.R[1]) * 2,
                TwiddleUV(i32NumXWords, i32NumYWords, indices.S[0], indices.S[1]) * 2,
            };

            PVRTCWord P, Q, R, S;
            P.colorData = static_cast<uint32_t>(readWord(WordOffsets[0] + 1));
            P.modulationData = static_cast<uint32_t>(readWord(WordOffsets[0]));
            Q.colorData = static_cast<uint32_t>(readWord(WordOffsets[1] + 1));
            Q.modulationData = static_cast<uint32_t>(readWord(WordOffsets[1]));
            R.colorData = static_cast<uint32_t>(readWord(WordOffsets[2] + 1));
            R.modulationData = static_cast<uint32_t>(readWord(WordOffsets[2]));
            S.colorData = static_cast<uint32_t>(readWord(WordOffsets[3] + 1));
            S.modulationData = static_cast<uint32_t>(readWord(WordOffsets[3]));

            pvrtcGetDecompressedPixels(P, Q, R, S, pPixels);
            mapDecompressedData(pOutData, width, pPixels, indices);

        } // for each word
    } // for each row of words
}

} // namespace

void decodePvrtc4(const std::uint8_t* data, std::uint32_t width, std::uint32_t height, std::uint8_t* rgba) {
    const auto storedWidth = std::max(width, 8u);
    const auto storedHeight = std::max(height, 8u);
    if (storedWidth == width && storedHeight == height) {
        pvrtcDecompress(data, reinterpret_cast<Pixel32*>(rgba), width, height);
    } else {
        std::vector<Pixel32> pixels(storedWidth * storedHeight);
        pvrtcDecompress(data, pixels.data(), storedWidth, storedHeight);
        for (std::uint32_t y = 0; y < height; ++y)
            std::memcpy(rgba + y * width * 4u, pixels.data() + y * storedWidth, width * 4u);
    }
}

} // namespace neoshared::texture::detail
