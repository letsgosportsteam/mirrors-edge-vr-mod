#pragma once
#include <d3d9.h>
#include <cstddef>
#include <cstdint>
#include <vector>
#include <cstring>

// Exact captured glass VS: keep TEXCOORD5 in the mono reflection capture's
// projective space while POSITION continues to use the current eye matrix.
// c252..255 and r5 are unused by this fingerprint. Reject every other shader.
inline bool StereoPatchGlassProjection(const DWORD* code,size_t count,std::vector<DWORD>* result)
{
    if(!code||count<2||code[0]!=D3DVS_VERSION(3,0))return false;
    uint32_t hash=2166136261u;for(size_t i=0;i<count;++i){hash^=code[i];hash*=16777619u;}
    if(hash!=0x8A7B075Bu)return false;
    result->clear();result->push_back(code[0]);unsigned inserted=0,replaced=0;
    for(size_t at=1;at<count;){
        const DWORD instruction=code[at];const unsigned op=instruction&D3DSI_OPCODE_MASK;
        if(op==D3DSIO_END){result->push_back(instruction);return inserted==1&&replaced==1&&at+1==count;}
        const size_t n=op==D3DSIO_COMMENT?((instruction>>16)&0x7fff):((instruction>>24)&15);
        if(n>count-at-1)return false;
        // mul r1, r0.y, c1: r0 still contains the world-space vertex here.
        if(op==D3DSIO_MUL&&n==3&&code[at+1]==0x800f0001u&&code[at+2]==0x80550000u&&code[at+3]==0xa0e40001u){
            const DWORD capture[]={
                0x03000005u,0x800f0005u,0x80550000u,0xa0e400fdu,
                0x04000004u,0x800f0005u,0xa0e400fcu,0x80000000u,0x80e40005u,
                0x04000004u,0x800f0005u,0xa0e400feu,0x80aa0000u,0x80e40005u,
                0x04000004u,0x800f0005u,0xa0e400ffu,0x80ff0000u,0x80e40005u};
            result->insert(result->end(),capture,capture+sizeof(capture)/sizeof(*capture));++inserted;
        }
        if(op==D3DSIO_MOV&&n==2&&code[at+1]==0xe00f0003u&&code[at+2]==0x80e40000u){
            result->insert(result->end(),{instruction,code[at+1],0x80e40005u});++replaced;
        }else result->insert(result->end(),code+at,code+at+n+1);
        at+=n+1;
    }
    return false;
}

// SM2/3 CTAB offsets are relative to the header AFTER the CTAB FourCC.
// Return only a scalar/vector or sampler of the requested register set/count.
inline int StereoConstantRegister(const DWORD* code,size_t count,const char* name,
                                  unsigned set,unsigned registers=1)
{
    if(!code||count<2||!name)return -1;
    for(size_t at=1;at<count;){
        const DWORD token=code[at++];const unsigned op=token&0xffff;
        if(op==D3DSIO_END)break;
        if(op!=D3DSIO_COMMENT){const size_t n=(token>>24)&15;if(n>count-at)return -1;at+=n;continue;}
        const size_t words=(token>>16)&0x7fff;
        if(words>count-at)return -1;
        if(words>=8&&code[at]==0x42415443u){
            const BYTE* base=(const BYTE*)(code+at+1);const size_t bytes=(words-1)*4;
            auto u32=[&](size_t off){DWORD v;memcpy(&v,base+off,4);return v;};
            const DWORD total=u32(12),offset=u32(16);
            if(offset>bytes||total>(bytes-offset)/20)return -1;
            for(DWORD i=0;i<total;++i){const size_t entry=offset+i*20;WORD type,index,number;
                memcpy(&type,base+entry+4,2);memcpy(&index,base+entry+6,2);memcpy(&number,base+entry+8,2);
                const DWORD text=u32(entry);
                if(text>=bytes||!memchr(base+text,0,bytes-text))return -1;
                if(type==set&&number==registers&&!strcmp((const char*)base+text,name))return index;
            }
        }
        at+=words;
    }
    return -1;
}

// Follow constant dependencies to POSITION, rather than trusting whatever c0
// happened to contain before this shader was bound. Shader model 2/3 only.
inline bool StereoShaderUsesViewProjection(const DWORD* code,size_t count,unsigned first)
{
    if(!code||count<2||first>252||(code[0]>>16)!=0xfffe)return false;
    const unsigned major=(code[0]>>8)&255;
    if(major!=2&&major!=3)return false;
    uint8_t temps[32][4]{},outputs[12][4]{};
    bool literal[256]{};
    int position=major==2?0:-1;
    auto type=[](DWORD t){return ((t&D3DSP_REGTYPE_MASK)>>D3DSP_REGTYPE_SHIFT)|
        ((t&D3DSP_REGTYPE_MASK2)>>D3DSP_REGTYPE_SHIFT2);};
    for(size_t at=1;at<count;){
        const DWORD instruction=code[at++];const unsigned op=instruction&D3DSI_OPCODE_MASK;
        if(op==D3DSIO_END){
            if(position<0)return false;
            unsigned dependencies=0;for(int c=0;c<4;++c)dependencies|=outputs[position][c];
            return dependencies==15;
        }
        if(op==D3DSIO_COMMENT){const size_t n=(instruction&D3DSI_COMMENTSIZE_MASK)>>D3DSI_COMMENTSIZE_SHIFT;
            if(n>count-at)return false;at+=n;continue;}
        const size_t n=(instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT;
        if(n>count-at)return false;
        if(op==D3DSIO_DCL){
            if(n==2&&major==3&&(code[at]&D3DSP_DCL_USAGE_MASK)==D3DDECLUSAGE_POSITION &&
                type(code[at+1])==D3DSPR_OUTPUT&&(code[at+1]&D3DSP_REGNUM_MASK)<12)
                position=(int)(code[at+1]&D3DSP_REGNUM_MASK);
        } else if(op==D3DSIO_DEF){
            if(n!=5)return false;
            const unsigned index=code[at]&D3DSP_REGNUM_MASK;if(index<256)literal[index]=true;
        } else if(op==D3DSIO_DEFI||op==D3DSIO_DEFB||op==D3DSIO_NOP){
            // Literals and declarations carry no camera dependence.
        } else {
            // Branches need control-flow analysis. Relative constant operands
            // are legal in straight-line particle code and carry a separate
            // address-register token inside the instruction's length.
            if(n<2||op==D3DSIO_IF||op==D3DSIO_IFC||op==D3DSIO_ELSE||op==D3DSIO_ENDIF ||
                op==D3DSIO_LOOP||op==D3DSIO_ENDLOOP||op==D3DSIO_REP||op==D3DSIO_ENDREP ||
                (instruction&D3DSHADER_INSTRUCTION_PREDICATED))return false;
            uint8_t dependencies=0;
            for(size_t s=1;s<n;++s){
                const DWORD token=code[at+s];const unsigned reg=token&D3DSP_REGNUM_MASK,kind=type(token);
                if(!(token&0x80000000u))return false;
                if(token&D3DSHADER_ADDRESSMODE_MASK){
                    if(kind!=D3DSPR_CONST||s+1>=n)return false;
                    const DWORD address=code[at+(++s)];
                    if(!(address&0x80000000u)||type(address)!=D3DSPR_ADDR||
                        (address&D3DSHADER_ADDRESSMODE_MASK))return false;
                    // An indexed array is not evidence of reading c0..c3.
                    // Continue tracking the explicit VP reads that follow it.
                    continue;
                }
                if(kind==D3DSPR_CONST){
                    unsigned rows=1;
                    if(s==2&&(op==D3DSIO_M4x4||op==D3DSIO_M3x4))rows=4;
                    if(s==2&&(op==D3DSIO_M4x3||op==D3DSIO_M3x3))rows=3;
                    if(s==2&&op==D3DSIO_M3x2)rows=2;
                    for(unsigned r=reg;r<reg+rows;++r)if(r>=first&&r<first+4&&!literal[r])dependencies|=1u<<(r-first);
                } else if(kind==D3DSPR_TEMP&&reg<32){
                    for(int c=0;c<4;++c)dependencies|=temps[reg][(token>>(D3DSP_SWIZZLE_SHIFT+2*c))&3];
                }
            }
            const DWORD dest=code[at];const unsigned reg=dest&D3DSP_REGNUM_MASK,kind=type(dest);
            uint8_t* values=nullptr;
            if(kind==D3DSPR_TEMP&&reg<32)values=temps[reg];
            if(major==2&&kind==D3DSPR_RASTOUT&&reg==0)values=outputs[0];
            if(major==3&&kind==D3DSPR_OUTPUT&&reg<12)values=outputs[reg];
            if(values)for(int c=0;c<4;++c)if(dest&(1u<<(16+c)))values[c]=dependencies;
        }
        at+=n;
    }
    return false;
}

// The captured sun-haze shader passes v1 UV straight to o0. Reserve two unused
// constants to map those UVs into the selected half of the packed scene texture.
// The caller checks the complete shader fingerprint before using this rewrite.
inline bool StereoPatchHazeUV(const DWORD* code,size_t count,std::vector<DWORD>* result)
{
    if(!code||count<2||code[0]!=D3DVS_VERSION(3,0))return false;
    result->clear();result->push_back(code[0]);unsigned patched=0;
    for(size_t at=1;at<count;){
        const DWORD instruction=code[at];const unsigned op=instruction&D3DSI_OPCODE_MASK;
        if(op==D3DSIO_END){result->push_back(instruction);return patched==1&&at+1==count;}
        const size_t n=op==D3DSIO_COMMENT?
            ((instruction&D3DSI_COMMENTSIZE_MASK)>>D3DSI_COMMENTSIZE_SHIFT):
            ((instruction&D3DSI_INSTLENGTH_MASK)>>D3DSI_INSTLENGTH_SHIFT);
        if(n>count-at-1)return false;
        // SM3 permits only one distinct constant register per instruction.
        // r2 is unused in the fingerprint-checked shader.
        // mov r2, c255; mad o0.xy, v1, c254, r2
        if(op==D3DSIO_MOV&&n==2&&code[at+1]==0xe0030000u&&code[at+2]==0x90e40001u){
            result->push_back((2u<<D3DSI_INSTLENGTH_SHIFT)|D3DSIO_MOV);
            result->push_back(0x800f0002u);result->push_back(0xa0e400ffu);
            result->push_back((4u<<D3DSI_INSTLENGTH_SHIFT)|D3DSIO_MAD);
            result->push_back(code[at+1]);result->push_back(code[at+2]);
            result->push_back(0xa0e400feu);result->push_back(0x80e40002u);++patched;
        }else result->insert(result->end(),code+at,code+at+n+1);
        at+=n+1;
    }
    return false;
}
