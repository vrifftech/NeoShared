#include "neoshared/texture/Txi.hpp"
#include "neoshared/texture/Image.hpp"
#include <algorithm>
#include "neoshared/texture/Error.hpp"
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace neoshared::texture {
namespace {
constexpr std::size_t maxBytes=16u*1024u*1024u, maxRows=65536u, maxChannels=4096u;
bool space(char c) {return c==' '||c=='\t'||c=='\r'||c=='\n'||c=='\v'||c=='\f';}
std::string trim(std::string_view v) {
    while(!v.empty()&&space(v.front()))v.remove_prefix(1);
    while(!v.empty()&&space(v.back()))v.remove_suffix(1);
    return std::string(v);
}
struct Count {bool present{}, binary{};long value{};};
Count count(const std::string& value) {
    errno=0;char* end=nullptr;const long n=std::strtol(value.c_str(),&end,10);
    if(end==value.c_str())return {};
    if(errno==ERANGE||n<INT_MIN||n>INT_MAX)throw TextureError("TXI list count exceeds native signed-int range");
    while(*end&&space(*end))++end;
    return {true,*end!=0,n}; // Native sscanf("%d %c"): any second character.
}
void fields(std::string_view raw,std::string& key,std::string& value) {
    auto v=trim(raw);const auto end=v.find_first_of(" \t\v\f");
    key=v.substr(0,end);for(auto& c:key)if(c>='A'&&c<='Z')c=char(c-'A'+'a');
    if(end!=std::string::npos)value=trim(std::string_view(v).substr(end));
}
}
std::vector<TxiStreamLine> scanTxiStream(std::string_view bytes) {
    if(bytes.size()>maxBytes)throw TextureError("TXI stream exceeds 16 MiB bound");
    std::vector<TxiStreamLine> lines;
    std::size_t pos=0,lineNumber=0,remaining=0;bool open=false;
    while(pos<bytes.size()) {
        if(bytes[pos]=='\0') {
            if(std::any_of(bytes.begin()+pos,bytes.end(),[](char c){return c!=0;}))
                throw TextureError("Embedded NUL outside a counted TXI binary payload");
            break;
        }
        if(lines.size()>=maxRows)throw TextureError("TXI stream exceeds text-line bound");
        TxiStreamLine line;line.begin=pos;line.lineNumber=++lineNumber;
        const auto e=bytes.find_first_of(std::string_view("\r\n\0",3),pos);
        line.end=e==std::string_view::npos?bytes.size():e;
        if(line.end-pos>65536u)throw TextureError("TXI text line exceeds 64 KiB bound");
        line.next=line.end;
        if(line.next<bytes.size()&&bytes[line.next]!='\0') {
            const char delimiter=bytes[line.next++];
            if(delimiter=='\r'&&line.next<bytes.size()&&bytes[line.next]=='\n')++line.next;
        }
        fields(bytes.substr(pos,line.end-pos),line.key,line.value);
        if(remaining||open) {
            line.listData=true;
            if(remaining)--remaining;
            else if(line.key.rfind("endlist",0)==0)open=false;
        } else if(const auto d=findTxiDirective(line.key);d&&
                  (d->valueKind==TxiDirectiveValueKind::FloatList||d->valueKind==TxiDirectiveValueKind::Vector3List)) {
            const auto c=count(line.value);
            const bool channel=line.key=="channelscale"||line.key=="channeltranslate";
            if(channel&&c.present&&c.binary) {
                if(c.value<0||std::size_t(c.value)>maxChannels)
                    throw TextureError("Binary TXI channel count outside 0..4096");
                const auto n=std::size_t(c.value);
                // A binary block starts at the cursor AFTER the complete CRLF
                // delimiter; its contents must never be processed as lines.
                if(n && (line.next==line.end || bytes[line.next-1]!='\n'))
                    throw TextureError("Binary TXI channel header requires the native LF delimiter");
                if(n>(bytes.size()-line.next)/4u)throw TextureError("Truncated binary TXI channel array");
                line.binary=true;line.payloadBegin=line.next;line.binaryValues.reserve(n);
                for(std::size_t i=0;i<n;++i) {
                    const auto* p=reinterpret_cast<const unsigned char*>(bytes.data()+line.next+4u*i);
                    const std::uint32_t bits=std::uint32_t(p[0])|(std::uint32_t(p[1])<<8u)|
                        (std::uint32_t(p[2])<<16u)|(std::uint32_t(p[3])<<24u);
                    float f;static_assert(sizeof(f)==4,"TXI requires IEEE binary32");
                    std::memcpy(&f,&bits,4);line.binaryValues.push_back(f);
                }
                line.next+=4u*n;
            } else if(c.present&&!c.binary&&c.value>=0) {
                if(std::size_t(c.value)>maxRows)throw TextureError("TXI counted list exceeds row bound");
                remaining=std::size_t(c.value);
            } else if(!c.present)open=true;
        }
        lines.push_back(std::move(line));pos=lines.back().next;
        // A trailing terminator is left outside every record's preserved span.
        if(pos==lines.back().end&&pos<bytes.size()&&bytes[pos]==0) {
            if(std::any_of(bytes.begin()+pos,bytes.end(),[](char c){return c!=0;}))
                throw TextureError("Embedded NUL outside a counted TXI binary payload");
            break;
        }
    }
    return lines;
}
TxiTextView txiTextView(std::string_view bytes) {
    TxiTextView result;
    const auto append=[&](const std::string& line,std::size_t number) {
        result.text+=line;result.text+='\n';result.sourceLines.push_back(number);
    };
    for(const auto& line:scanTxiStream(bytes)) {
        if(!line.binary)append(std::string(bytes.substr(line.begin,line.end-line.begin)),line.lineNumber);
        else {
            append(line.key+" "+std::to_string(line.binaryValues.size()),line.lineNumber);
            for(float f:line.binaryValues) {
                std::ostringstream value;value.imbue(std::locale::classic());
                value<<std::setprecision(std::numeric_limits<float>::max_digits10)<<f;
                append(value.str(),line.lineNumber);
            }
        }
    }
    return result;
}
std::string trimTxiPadding(std::string bytes) {
    const auto lines=scanTxiStream(bytes);
    const auto keep=lines.empty()?0u:lines.back().next;
    // Keep every counted byte, even a final +0.0f or a float ending in 0x00.
    bytes.resize(keep);return bytes;
}
} // namespace neoshared::texture
