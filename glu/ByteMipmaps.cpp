#include <neoshared/glu/ByteMipmaps.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace neoshared::glu {
namespace {

// The Android implementation uses separate scalar SSE instructions. Preserve
// each binary32 result on hosts that contract expressions or retain excess
// precision; in particular, a weighted accumulation is not an FMA.
float add(float a, float b) { volatile float value=a+b; return value; }
float sub(float a, float b) { volatile float value=a-b; return value; }
float mul(float a, float b) { volatile float value=a*b; return value; }
float div(float a, float b) { volatile float value=a/b; return value; }

ByteImage allocate(unsigned width, unsigned height, unsigned components) {
    return {width,height,components,
        std::vector<std::uint8_t>(std::size_t(width)*height*components)};
}

ByteImage halveUbyte(const ByteImage& source) {
    // 0x008fe660: two-dimensional halves round by adding two; the separate
    // one-dimensional paths at 0x008fe745 and 0x008fe7a6 truncate instead.
    auto result=allocate(std::max(1u,source.width/2u),
                         std::max(1u,source.height/2u),source.components);
    const auto stride=std::size_t(source.width)*source.components;
    for(unsigned y=0;y<result.height;++y) {
        for(unsigned x=0;x<result.width;++x) {
            const auto input=(std::size_t(source.height==1u?0u:y*2u)*source.width+
                              (source.width==1u?0u:x*2u))*source.components;
            const auto output=(std::size_t(y)*result.width+x)*source.components;
            for(unsigned c=0;c<source.components;++c) {
                unsigned total=source.bytes[input+c];
                if(source.height==1u) {
                    total+=source.bytes[input+source.components+c];
                    result.bytes[output+c]=std::uint8_t(total>>1u);
                } else if(source.width==1u) {
                    total+=source.bytes[input+stride+c];
                    result.bytes[output+c]=std::uint8_t(total>>1u);
                } else {
                    total+=source.bytes[input+source.components+c];
                    total+=source.bytes[input+stride+c];
                    total+=source.bytes[input+stride+source.components+c];
                    result.bytes[output+c]=std::uint8_t((total+2u)>>2u);
                }
            }
        }
    }
    return result;
}

} // namespace

unsigned nearestPower(unsigned value) {
    // 0x008fe512..0x008fe540 / 0x008fe55d..0x008fe58e.
    unsigned result=1;
    for(;;) {
        if(value==1u)return result;
        if(value==3u)return result*4u;
        value>>=1u;
        result*=2u;
    }
}

ByteImage scaleUbyte(const ByteImage& source,unsigned width,unsigned height) {
    // The builder bypasses scaling when dimensions already match
    // (0x008f97b4..0x008f9807). Its UBYTE dispatch is table 0x00b068b8,
    // slot 1 -> 0x008f9b31, with the general kernel at 0x008fabc7.
    if(source.width==width&&source.height==height)return source;
    if(source.width==width*2u&&source.height==height*2u)return halveUbyte(source);

    auto result=allocate(width,height,source.components);
    const float convX=div(float(source.width),float(width));
    const float convY=div(float(source.height),float(height));
    const int stepX=int(std::floor(convX)),stepY=int(std::floor(convY));
    const float fractionX=sub(convX,float(stepX)),fractionY=sub(convY,float(stepY));
    const float area=mul(convX,convY);
    const auto pixel=std::ptrdiff_t(source.components);
    const auto stride=std::ptrdiff_t(source.width)*pixel;
    int lowY=0,highY=std::max(1,stepY)-1;
    float lowYFraction=0.0f,highYFraction=fractionY;

    for(unsigned y=0;y<height;++y) {
        // These initial endpoints and clamps are the binary's own operations
        // (0x008fac71..0x008fad4a), including its endpoint-minus-one behavior.
        highY=std::max(0,highY);
        const float ySpan=sub(highYFraction,lowYFraction);
        const float firstY=sub(1.0f,lowYFraction);
        int lowX=0,highX=stepX-1;
        float lowXFraction=0.0f,highXFraction=fractionX;
        for(unsigned x=0;x<width;++x) {
            std::array<float,4> totals{};
            const auto xOffset=std::max(std::ptrdiff_t(0),std::ptrdiff_t(lowX)*pixel);
            const auto topLeft=std::ptrdiff_t(lowY)*stride+xOffset;
            const float firstX=sub(1.0f,lowXFraction);
            const float xSpan=sub(highXFraction,lowXFraction);
            const auto accumulate=[&](std::ptrdiff_t offset,float weight) {
                for(unsigned c=0;c<source.components;++c)
                    totals[c]=add(totals[c],mul(float(source.bytes[std::size_t(offset)+c]),weight));
            };

            if(highY>lowY&&highX>lowX) {
                // First row, last row, paired side edges, then interior.
                // The order follows 0x008fae56..0x008fb3a7.
                accumulate(topLeft,mul(firstY,firstX));
                auto topRight=topLeft+pixel;
                for(int sx=lowX+1;sx<highX;++sx,topRight+=pixel)
                    accumulate(topRight,firstY);
                accumulate(topRight,mul(firstY,highXFraction));
                auto bottomRight=std::ptrdiff_t(highY)*stride+xOffset;
                accumulate(bottomRight,mul(highYFraction,firstX));
                bottomRight+=pixel;
                for(int sx=lowX+1;sx<highX;++sx,bottomRight+=pixel)
                    accumulate(bottomRight,highYFraction);
                accumulate(bottomRight,mul(highYFraction,highXFraction));

                auto left=topLeft,right=topRight;
                for(int sy=lowY+1;sy<highY;++sy) {
                    left+=stride;
                    right+=stride;
                    for(unsigned c=0;c<source.components;++c) {
                        const float a=mul(float(source.bytes[std::size_t(left)]),firstX);
                        const float b=mul(float(source.bytes[std::size_t(right)]),highXFraction);
                        totals[c]=add(totals[c],add(b,a));
                        ++left;
                        ++right;
                    }
                    // The native loop retains these advanced pointers when
                    // adding the next row stride (0x008fb388..0x008fb3a7).
                }
            } else if(highY>lowY) {
                accumulate(topLeft,mul(firstY,xSpan));
                auto offset=topLeft;
                for(int sy=lowY+1;sy<highY;++sy) {
                    offset+=stride;
                    accumulate(offset,xSpan);
                }
                accumulate(offset+stride,mul(xSpan,highYFraction));
            } else if(highX>lowX) {
                accumulate(topLeft,mul(firstX,ySpan));
                auto offset=topLeft+pixel;
                for(int sx=lowX+1;sx<highX;++sx,offset+=pixel)
                    accumulate(offset,ySpan);
                accumulate(offset,mul(ySpan,highXFraction));
            } else {
                accumulate(topLeft,mul(xSpan,ySpan));
            }

            for(int sy=lowY+1;sy<highY;++sy) {
                auto offset=std::ptrdiff_t(sy)*stride+xOffset+pixel;
                for(int sx=lowX+1;sx<highX;++sx,offset+=pixel)
                    for(unsigned c=0;c<source.components;++c)
                        totals[c]=add(totals[c],float(source.bytes[std::size_t(offset)+c]));
            }
            const auto output=(std::size_t(y)*width+x)*source.components;
            for(unsigned c=0;c<source.components;++c) {
                // DIVSS -> CVTTSS2SI -> low byte, with no rounding addition
                // or saturation (0x008fb470..0x008fb487).
                result.bytes[output+c]=std::uint8_t(int(div(totals[c],area)));
            }
            lowX=highX;
            lowXFraction=highXFraction;
            highX+=stepX;
            highXFraction=add(highXFraction,fractionX);
            if(highXFraction>1.0f) {
                highXFraction=add(highXFraction,-1.0f);
                ++highX;
            }
        }
        lowY=highY;
        lowYFraction=highYFraction;
        highY+=stepY;
        highYFraction=add(highYFraction,fractionY);
        if(highYFraction>1.0f) {
            highYFraction=add(highYFraction,-1.0f);
            ++highY;
        }
    }
    return result;
}

std::vector<ByteImage> buildByteMipmaps(const ByteImage& source,unsigned maxTextureSize) {
    // GL_MAX_TEXTURE_SIZE is applied independently after nearestPower
    // (0x008fe500..0x008fe594), including cube-face callers.
    const auto width=std::min(nearestPower(source.width),maxTextureSize);
    const auto height=std::min(nearestPower(source.height),maxTextureSize);
    std::vector<ByteImage> result;
    result.push_back(scaleUbyte(source,width,height));
    // All subsequent UBYTE levels use 0x008fe660, not the general scaler:
    // dispatch 0x00b06940, slot 1 -> 0x008fa100 -> 0x008fa11a.
    while(result.back().width>1u||result.back().height>1u)
        result.push_back(halveUbyte(result.back()));
    return result;
}

} // namespace neoshared::glu
