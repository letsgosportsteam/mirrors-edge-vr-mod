// uedecompress3 - decompress a chunk-compressed UE3 package into a flat file.
//
// The Singularity project's `uedecompress` handles only FULLY-compressed packages and fails
// on these with "chunk 0 runs past end of file". Mirror's Edge uses the other UE3 scheme: a
// plaintext header carrying a CompressedChunks table, each chunk split into 128 KB blocks.
//
// Measured on TdGame.u (UE3 536 / licensee 43):
//   CompressionFlags = 2  (COMPRESS_LZO)
//   55 chunks, block size 131072
//   chunk 0 is the whole 4,729,004-byte header - names, imports and exports
//
// The header layout is not hardcoded from a spec. Field offsets that vary by version are
// located by reading forward from the fixed part and validating as we go, and the chunk table
// is cross-checked against the FCompressedChunkHeader magic actually present in the file.
//
// Output is a flat package: the header verbatim, then every chunk expanded at its recorded
// uncompressed offset. That is what a decompiler expects.
//
// ⚠️ Output is game-derived content. It is written outside the repository by default and
// `script_dump/` is gitignored. Do not commit it, and do not paste decompiled script into
// notes - summarise findings instead.

#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include "lzo1x.h"

static const uint32_t UE3_MAGIC = 0x9E2A83C1u;

struct Chunk { int32_t uOff, uSize, cOff, cSize; };

static bool ReadAll(const char* path, std::vector<uint8_t>& out)
{
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    out.resize((size_t)n);
    size_t got = fread(out.data(), 1, (size_t)n, f);
    fclose(f);
    return got == (size_t)n;
}

int main(int argc, char** argv)
{
    if (argc < 2) {
        printf("usage: uedecompress3 <in.u|.upk> [out.u]\n");
        printf("       expands a chunk-compressed UE3 package (COMPRESS_LZO).\n");
        return 1;
    }
    const char* inPath = argv[1];
    std::string outPath = (argc >= 3) ? argv[2] : (std::string(inPath) + ".dec");

    std::vector<uint8_t> f;
    if (!ReadAll(inPath, f)) { printf("ERROR: cannot read %s\n", inPath); return 1; }
    printf("%s  %zu bytes\n", inPath, f.size());

    auto I32 = [&](size_t o) -> int32_t {
        if (o + 4 > f.size()) return 0;
        int32_t v; memcpy(&v, f.data() + o, 4); return v;
    };
    auto U32 = [&](size_t o) -> uint32_t { return (uint32_t)I32(o); };

    if (U32(0) != UE3_MAGIC) { printf("ERROR: not a UE3 package (magic 0x%08X)\n", U32(0)); return 1; }
    uint16_t ver = (uint16_t)(U32(4) & 0xFFFF);
    uint16_t lic = (uint16_t)(U32(4) >> 16);
    int32_t  totalHeader = I32(8);
    printf("version %u / licensee %u   TotalHeaderSize %d\n", ver, lic, totalHeader);

    // FolderName, then the fixed run of counts and offsets.
    size_t p = 12;
    int32_t fnLen = I32(p); p += 4;
    if (fnLen >= 0) p += (size_t)fnLen; else p += (size_t)(-fnLen) * 2;

    const size_t pkgFlagsAt = p;
    uint32_t pkgFlags = U32(p); p += 4;
    int32_t nameCount = I32(p); p += 4;
    int32_t nameOff   = I32(p); p += 4;
    int32_t expCount  = I32(p); p += 4;
    int32_t expOff    = I32(p); p += 4;
    int32_t impCount  = I32(p); p += 4;
    int32_t impOff    = I32(p); p += 4;
    int32_t dependsOff= I32(p); p += 4;
    printf("flags 0x%08X  names %d@0x%X  exports %d@0x%X  imports %d@0x%X\n",
           pkgFlags, nameCount, nameOff, expCount, expOff, impCount, impOff);
    (void)dependsOff;

    p += 16;                       // FGuid
    int32_t genCount = I32(p); p += 4;
    if (genCount < 0 || genCount > 64) { printf("ERROR: implausible generation count %d\n", genCount); return 1; }
    p += (size_t)genCount * 12;    // ExportCount, NameCount, NetObjectCount per generation

    int32_t engineVer = I32(p); p += 4;
    int32_t cookerVer = I32(p); p += 4;
    const size_t compFlagsAt = p;
    int32_t compFlags = I32(p); p += 4;
    const size_t chunkCountAt = p;
    int32_t chunkCount= I32(p); p += 4;
    printf("engine %d  cooker %d  CompressionFlags %d (%s)  chunks %d\n",
           engineVer, cookerVer, compFlags,
           compFlags == 1 ? "ZLIB" : compFlags == 2 ? "LZO" : compFlags == 4 ? "LZX" : "?",
           chunkCount);

    if (compFlags != 2) { printf("ERROR: only COMPRESS_LZO is implemented here\n"); return 1; }
    if (chunkCount <= 0 || chunkCount > 100000) { printf("ERROR: implausible chunk count\n"); return 1; }

    std::vector<Chunk> chunks((size_t)chunkCount);
    for (int i = 0; i < chunkCount; ++i) {
        chunks[i].uOff  = I32(p); p += 4;
        chunks[i].uSize = I32(p); p += 4;
        chunks[i].cOff  = I32(p); p += 4;
        chunks[i].cSize = I32(p); p += 4;
    }

    // The chunk table is trusted only after it agrees with the file. Every recorded chunk
    // offset must actually carry the FCompressedChunkHeader magic; a layout misread would
    // otherwise surface as silent garbage far downstream.
    for (int i = 0; i < chunkCount; ++i) {
        if ((size_t)chunks[i].cOff + 16 > f.size() || U32((size_t)chunks[i].cOff) != UE3_MAGIC) {
            printf("ERROR: chunk %d at 0x%X does not carry the chunk magic - header misread\n",
                   i, chunks[i].cOff);
            return 1;
        }
    }
    printf("chunk table validated against the file: all %d carry the chunk magic\n", chunkCount);

    // Total size of the flat output.
    size_t outSize = 0;
    for (auto& c : chunks) {
        size_t end = (size_t)c.uOff + (size_t)c.uSize;
        if (end > outSize) outSize = end;
    }
    if ((size_t)totalHeader > outSize) outSize = (size_t)totalHeader;
    printf("decompressed size will be %zu bytes\n", outSize);

    std::vector<uint8_t> out(outSize, 0);
    // The plaintext header up to the first chunk is copied verbatim.
    size_t headCopy = (size_t)chunks[0].uOff;
    if (headCopy > f.size()) headCopy = f.size();
    memcpy(out.data(), f.data(), headCopy);

    size_t blocksDone = 0;
    for (int i = 0; i < chunkCount; ++i) {
        const Chunk& c = chunks[i];
        size_t hp = (size_t)c.cOff;
        int32_t blockSize = I32(hp + 4);
        int32_t cTotal    = I32(hp + 8);
        int32_t uTotal    = I32(hp + 12);
        (void)cTotal;
        if (blockSize <= 0 || uTotal <= 0) { printf("ERROR: chunk %d header implausible\n", i); return 1; }

        int nBlocks = (int)(((int64_t)uTotal + blockSize - 1) / blockSize);
        size_t infoAt = hp + 16;
        size_t dataAt = infoAt + (size_t)nBlocks * 8;
        size_t writeAt = (size_t)c.uOff;

        for (int b = 0; b < nBlocks; ++b) {
            int32_t bc = I32(infoAt + (size_t)b * 8);
            int32_t bu = I32(infoAt + (size_t)b * 8 + 4);
            if (bc <= 0 || bu <= 0 || dataAt + (size_t)bc > f.size()) {
                printf("ERROR: chunk %d block %d out of range\n", i, b); return 1;
            }
            if (writeAt + (size_t)bu > out.size()) {
                printf("ERROR: chunk %d block %d writes past the output\n", i, b); return 1;
            }
            size_t produced = 0;
            int r = lzo1x_decompress(f.data() + dataAt, (size_t)bc,
                                     out.data() + writeAt, (size_t)bu, &produced);
            if (r != LZO_OK) {
                printf("ERROR: LZO failed on chunk %d block %d (code %d, %d -> %d)\n", i, b, r, bc, bu);
                return 1;
            }
            if (produced != (size_t)bu)
                printf("  warn: chunk %d block %d produced %zu, expected %d\n", i, b, produced, bu);
            dataAt  += (size_t)bc;
            writeAt += (size_t)bu;
            blocksDone++;
        }
    }
    printf("decompressed %zu blocks across %d chunks\n", blocksDone, chunkCount);

    // ---- clear the compression markers, or the output is unreadable ----
    //
    // The header is copied verbatim, so it still advertises CompressionFlags=2 and 55 chunks.
    // A reader that believes it then tries to decompress data that is already expanded.
    // UELib did exactly that and died with an OutOfMemoryException - a failure that looks
    // nothing like "your header says the wrong thing".
    //
    // Zeroing the count is NOT sufficient on its own, and the first attempt to do only that
    // failed instructively: UELib then read PackageSource out of the stale chunk table and
    // got 109, after which an array length read from misaligned bytes threw
    // OutOfMemoryException. Every field AFTER the chunk table has to move up into its place.
    //
    // The compressed and decompressed header layouts genuinely differ. In the compressed
    // file the chunk table sits between chunkCount and the trailing fields; in the
    // decompressed one there is no table, so those trailing fields land directly after
    // chunkCount and the name table begins at chunks[0].uOff exactly as the header claims.
    if (compFlagsAt + 4 <= out.size() && chunkCountAt + 4 <= out.size() && pkgFlagsAt + 4 <= out.size()) {
        int32_t zero = 0;
        memcpy(out.data() + compFlagsAt,  &zero, 4);
        uint32_t cleared = pkgFlags & ~0x02000000u;   // PKG_StoreCompressed
        memcpy(out.data() + pkgFlagsAt, &cleared, 4);

        // PackageSource and whatever follows it, lifted from after the table to where a
        // reader that saw chunkCount == 0 will look for them.
        const size_t tableEnd = chunkCountAt + 4 + (size_t)chunkCount * 16;
        const size_t trailerSrc = tableEnd;
        const size_t trailerDst = chunkCountAt + 4;
        const size_t trailerLen = (size_t)chunks[0].cOff > tableEnd
                                ? (size_t)chunks[0].cOff - tableEnd : 0;

        if (trailerLen > 0 && trailerSrc + trailerLen <= f.size()
            && trailerDst + trailerLen <= out.size()) {
            memcpy(out.data() + trailerDst, f.data() + trailerSrc, trailerLen);
            memcpy(out.data() + chunkCountAt, &zero, 4);
            printf("cleared compression markers and moved %zu trailer byte(s) from 0x%zX to 0x%zX\n",
                   trailerLen, trailerSrc, trailerDst);
            if (trailerDst + trailerLen != (size_t)chunks[0].uOff)
                printf("  warn: trailer ends at 0x%zX but the name table starts at 0x%X\n",
                       trailerDst + trailerLen, chunks[0].uOff);
        } else {
            printf("WARNING: could not relocate the post-table trailer (len %zu) - the header "
                   "will be misaligned for readers\n", trailerLen);
        }
        printf("  PackageFlags 0x%08X -> 0x%08X, CompressionFlags -> 0\n", pkgFlags, cleared);
    } else {
        printf("WARNING: could not clear compression markers - readers may try to decompress\n");
    }

    // ---- correctness check that cannot pass by accident ----
    //
    // NOT "the first three names are None/ByteProperty/IntProperty". That is the order of the
    // RUNTIME GNames array, which the engine populates by interning its own types first. A
    // COOKED PACKAGE's name table holds only the names that package references, in cooker
    // order - so it legitimately starts with whatever asset the cooker saw first. The initial
    // version of this check asserted the runtime order and failed on a correct decompression.
    //
    // What cannot pass by accident is the whole table parsing as printable strings AND
    // specific expected names being present. Wrong LZO output would not produce 20,592
    // consecutive well-formed length-prefixed strings.
    {
        printf("verifying the name table at 0x%X (%d entries):\n", nameOff, nameCount);
        size_t q = (size_t)nameOff;
        int good = 0, bad = 0;
        std::vector<std::string> names;
        names.reserve((size_t)nameCount);

        for (int i = 0; i < nameCount; ++i) {
            if (q + 4 > out.size()) { bad++; break; }
            int32_t len; memcpy(&len, out.data() + q, 4); q += 4;
            if (len <= 0 || len > 1024 || q + (size_t)len > out.size()) { bad++; break; }
            std::string s((const char*)out.data() + q, (size_t)len);
            while (!s.empty() && s.back() == '\0') s.pop_back();
            q += (size_t)len;
            q += 8;                       // FName flags (qword) follow each entry

            bool printable = !s.empty();
            for (char ch : s) if ((unsigned char)ch < 32 || (unsigned char)ch > 126) { printable = false; break; }
            if (printable) { good++; names.push_back(s); } else bad++;
            if (i < 8) printf("  name[%d] = \"%s\"\n", i, s.c_str());
        }
        printf("  parsed %d well-formed of %d (%d rejected)\n", good, nameCount, bad);

        const size_t inLen = strlen(inPath);
        const bool enginePackage = inLen >= 8 &&
            _stricmp(inPath + inLen - 8, "Engine.u") == 0;
        const char* expectTdGame[] = { "Class", "Function", "TdPlayerCamera",
            "TdPlayerController", "TdPlayerPawn", "Rotation", "Location" };
        const char* expectEngine[] = { "Class", "Function", "SkelControlBase",
            "SkelControlLimb", "ControlStrength", "StrengthTarget", "BlendTimeToGo" };
        const char** expect = enginePackage ? expectEngine : expectTdGame;
        const int expectCount = enginePackage
            ? (int)(sizeof(expectEngine) / sizeof(*expectEngine))
            : (int)(sizeof(expectTdGame) / sizeof(*expectTdGame));
        int found = 0;
        for (int i = 0; i < expectCount; ++i) {
            const char* e = expect[i];
            bool hit = false;
            for (const std::string& s : names) if (s == e) { hit = true; break; }
            printf("  expected name %-22s %s\n", e, hit ? "FOUND" : "missing");
            if (hit) found++;
        }

        const bool ok = (good >= (int)(nameCount * 0.99)) && (found >= 5);
        if (!ok) {
            printf("*** VERIFICATION FAILED - %d/%d names well-formed, %d/%d expected names "
                   "found. Output is NOT trustworthy.\n", good, nameCount, found, expectCount);
            return 1;
        }
        printf("*** VERIFIED: %d names parsed, %d expected names present.\n", good, found);
    }

    FILE* o = nullptr;
    if (fopen_s(&o, outPath.c_str(), "wb") != 0 || !o) { printf("ERROR: cannot write %s\n", outPath.c_str()); return 1; }
    fwrite(out.data(), 1, out.size(), o);
    fclose(o);
    printf("wrote %s (%zu bytes)\n", outPath.c_str(), out.size());
    return 0;
}
