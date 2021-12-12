#include <string>
#include <map>
#include <vector>
#include <iostream>
#include <fstream>
#include <algorithm>
#include <cassert>
#include <chrono>
#include <array>

static const uint8_t kEndTrackMarker = 0x3f;
static const int kMaxDelay = 33;
static const int kMaxRefOffset = 16384;

enum Flags
{
    none = 0,
    fastDepack = 1, //< Reduce compression level, but make packet PSG more fast to play
    
    cleanRegs  = 2,
    cleanToneA = 4,
    cleanToneB = 8,
    cleanToneC = 16,
    cleanEnvelope = 32,
    cleanEnvForm = 64,
    cleanNoise = 128,
    
    dumpPsg = 256,
    dumpTimings = 512
};

static const int kDefaultFlags = cleanNoise - 1;

class PgsPacker
{
public:

    struct Stats
    {
        int psgFrames = 0;
        int emptyCnt = 0;
        int emptyFrames = 0;

        int singleRepeat = 0;
        int allRepeat = 0;
        int allRepeatFrames = 0;

        int ownCnt = 0;
        int ownBytes = 0;

        std::map<int, int> frameRegs;
        std::map<int, int> regsChange;

        std::map<int, int> firstHalfRegs;
        std::map<int, int> secondHalfRegs;

        int unusedToneA = 0;
        int unusedToneB = 0;
        int unusedToneC = 0;
        int unusedEnvelope = 0;
        int unusedEnvForm = 0;
        int unusedNoise = 0;
    };

    using RegMap = std::map<int, int>;
    using RegVector = std::array<int, 14>;

    std::map<RegMap, uint16_t> regsToSymbol;
    std::map<uint16_t, RegMap> symbolToRegs;
    std::vector<uint16_t> ayFrames;

    RegMap changedRegs;

    RegVector lastOrigRegs;
    RegVector lastCleanedRegs;
    RegVector prevCleanedRegs;
    RegVector prevTonePeriod;
    RegVector prevEnvelopePeriod;
    RegVector prevEnvelopeForm;
    RegVector prevNoisePeriod;

    Stats stats;

    std::vector<uint8_t> srcPsgData;
    std::vector<uint8_t> updatedPsgData;
    std::vector<uint8_t> compressedData;
    std::vector<int> refCount;
    std::vector<int> frameOffsets;
    int flags = kDefaultFlags;
    bool firstFrame = false;
    std::vector<int> timingsData;

private:

    bool isPsg2(const RegMap& regs) const
    {
        bool usePsg2 = regs.size() > 2;
        return usePsg2;
    }

    uint16_t toSymbol(const RegMap& regs)
    {
        auto itr = regsToSymbol.find(regs);
        if (itr != regsToSymbol.end())
            return itr->second;

        uint16_t value = regsToSymbol.size();
        regsToSymbol.emplace(regs, value);
        symbolToRegs.emplace(value, regs);
        return value;
    }

    // This code ported from PHP to cpp from tmk&bfox ayPacker.
    void doCleanRegs()
    {
        // Normalize regs values (only usage bits).

        lastCleanedRegs = lastOrigRegs;

        lastCleanedRegs[1] &= 15;
        lastCleanedRegs[3] &= 15;
        lastCleanedRegs[5] &= 15;
        lastCleanedRegs[6] &= 31;
        lastCleanedRegs[7] &= 63;
        lastCleanedRegs[8] &= 31;
        lastCleanedRegs[9] &= 31;
        lastCleanedRegs[10] &= 31;
        lastCleanedRegs[13] &= 15;

        lastCleanedRegs[7] &= 63;

        // clean volume (do AND_16 if envelope mode)

        for (int i : {8, 9, 10})
        {
            if (lastCleanedRegs[i] & 16)
                lastCleanedRegs[i] = 16;
        }

        // Clean tone period.

        /* toneA */
        if (flags & cleanToneA) 
        {
            if (lastOrigRegs[8] == 0 || (lastOrigRegs[7] & 1) != 0)
            {
                lastCleanedRegs[0] = prevTonePeriod[0];
                lastCleanedRegs[1] = prevTonePeriod[1];
                stats.unusedToneA++;
            }
            else 
            {
                prevTonePeriod[0] = lastOrigRegs[0];
                prevTonePeriod[1] = lastOrigRegs[1];
            }
        }
        /* toneB */
        if (flags & cleanToneB)
        {
            if (lastOrigRegs[9] == 0 || (lastOrigRegs[7] & 2) != 0)
            {
                lastCleanedRegs[2] = prevTonePeriod[2];
                lastCleanedRegs[3] = prevTonePeriod[3];
                stats.unusedToneB++;
            }
            else 
            {
                prevTonePeriod[2] = lastOrigRegs[2];
                prevTonePeriod[3] = lastOrigRegs[3];
            }
        }
        /* toneC */
        if (flags & cleanToneC)
        {
            if (lastOrigRegs[10] == 0 || (lastOrigRegs[7] & 4) != 0)
            {
                lastCleanedRegs[4] = prevTonePeriod[4];
                lastCleanedRegs[5] = prevTonePeriod[5];
                stats.unusedToneC++;
            }
            else 
            {
                prevTonePeriod[4] = lastOrigRegs[4];
                prevTonePeriod[5] = lastOrigRegs[5];
            }
        }

        // Clean envelope period.

        if (flags & cleanEnvelope)
        {
            if ((lastOrigRegs[8] & 16) == 0 && (lastOrigRegs[9] & 16) == 0 && (lastOrigRegs[10] & 16) == 0)
            {
                lastCleanedRegs[11] = prevEnvelopePeriod[11];
                lastCleanedRegs[12] = prevEnvelopePeriod[12];
                stats.unusedEnvelope++;
            }
            else 
            {
                prevEnvelopePeriod[11] = lastOrigRegs[11];
                prevEnvelopePeriod[12] = lastOrigRegs[12];
            }
        }

        /* clean envelope form */

        if (flags & cleanEnvForm) 
        {
            if ((lastOrigRegs[8] & 16) == 0 && (lastOrigRegs[9] & 16) == 0 && (lastOrigRegs[10] & 16) == 0)
            {
                lastCleanedRegs[13] = prevEnvelopeForm[13];
                stats.unusedEnvForm++;
            }
            else 
            {
                prevEnvelopeForm[13] = lastOrigRegs[13];
            }
        }

        /* clean noise period */

        if (flags & cleanNoise) 
        {
            if ((lastOrigRegs[7] & 8) != 0 && (lastOrigRegs[7] & 16) != 0 && (lastOrigRegs[7] & 32) != 0)
            {
                lastCleanedRegs[6] = prevNoisePeriod[6];
                stats.unusedNoise++;
            }
            else 
            {
                prevNoisePeriod[6] = lastCleanedRegs[6];
            }
        }
    }

    void extendToFullChangeIfNeed()
    {
        decltype(changedRegs) firstReg, secondReg;
        for (const auto& reg : changedRegs)
        {
            if (reg.first < 6)
                firstReg.insert(reg);
            else if (reg.first != 13)
                secondReg.insert(reg);
        }

        if (firstReg.size() == 5)
        {
            // Regs are about to full. Extend them to full regs.
            for (int i = 0; i < 6; ++i)
                changedRegs[i] = lastCleanedRegs[i];
        }

        if (secondReg.size() == 6 || secondReg.size() == 5)
        {
            // Regs are about to full. Extend them to full regs (exclude reg13)
            for (int i = 6; i < 13; ++i)
                changedRegs[i] = lastCleanedRegs[i];
        }
    }

    bool writeRegs()
    {
        if (changedRegs.empty())
            return false;

        if (prevTonePeriod.empty())
        {
            for (int i = 0; i < 13; ++i)
            {
                changedRegs.emplace(i, 0);
                lastOrigRegs[i] = 0;
            }

            // Initial value
            prevTonePeriod = lastOrigRegs;
            prevEnvelopePeriod = lastOrigRegs;
            prevEnvelopeForm = lastOrigRegs;
            prevNoisePeriod = lastOrigRegs;
        }

        int unusedEnvForm = stats.unusedEnvForm;
        lastCleanedRegs = lastOrigRegs;
        if (flags & cleanRegs)
            doCleanRegs();


        RegMap delta;
        for (int i = 0; i < 14; ++i)
        {
            if (firstFrame || lastCleanedRegs[i] != prevCleanedRegs[i])
                delta[i] = lastCleanedRegs[i];
        }
        firstFrame = false;
        prevCleanedRegs = lastCleanedRegs;

        if (changedRegs.count(13) && !(flags & cleanRegs))
            delta[13] = changedRegs[13]; //< Can be retrig.

        changedRegs = delta;
        if (changedRegs.empty())
            return false;

        if (flags & dumpPsg)
        {
            if (updatedPsgData.empty())
                updatedPsgData.insert(updatedPsgData.end(), srcPsgData.begin(), srcPsgData.begin() + 16);

            updatedPsgData.push_back(0xff);
            for (const auto& reg: changedRegs)
            {
                updatedPsgData.push_back(reg.first);
                updatedPsgData.push_back(reg.second);
            }
        }

        if (flags & fastDepack)
            extendToFullChangeIfNeed();

        uint16_t symbol = toSymbol(changedRegs);
        ayFrames.push_back(symbol); //< Flush previous frame.

        changedRegs.clear();
        return true;
    }

    void writeDelay(int delay)
    {
        if (flags & dumpPsg)
        {
            for (int i = 0; i < delay; ++i)
                updatedPsgData.push_back(0xff);
        }

        while (delay > 0)
        {
            int d = std::min(kMaxDelay, delay);
            ayFrames.push_back(d); //< Special code for delay
            delay -= d;
        }
    }

    void serializeEmptyFrames(int count)
    {
        while (count > 0)
        {
            uint8_t value = std::min(33, count);
            uint8_t header = 30;
            compressedData.push_back(header + value - 1);
            count -= value;
        }
    };

    uint8_t reverseBits(uint8_t value)
    {
        uint8_t b = value;
        b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
        b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
        b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
        return b;
    }

    void serializeRef(uint16_t pos, uint8_t size)
    {
        int offset = frameOffsets[pos];
        int recordSize = size == 1 ? 2 : 3;
        int16_t delta = offset - compressedData.size() - recordSize;
        if (size > 1)
            ++delta;
        assert(delta < 0);

        uint8_t* ptr = (uint8_t*)&delta;

        if (size == 1)
            ptr[1] &= ~0x40; // reset 6-th bit

        // Serialize in network byte order
        compressedData.push_back(ptr[1]);
        compressedData.push_back(ptr[0]);

        if (size > 1)
            compressedData.push_back(size - 1);
    };
    
    uint8_t makeRegMask(const RegMap& regs, int from, int to)
    {
        uint8_t result = 0;
        uint8_t bit = 0x80;
        for (int i = from; i < to; ++i)
        {
            const auto reg = regs.find(i);
            if (reg == regs.end())
            {
                result += bit;
            }
            bit >>= 1;
        }
        return result;
    }

    int pl00TimeForFrame(const RegMap& regs)
    {
        int result = 140;
        if (regs.size() == 1)
            result += -(16 + 4 + 16 + 7) + 5;
        return result;
    }

    auto splitRegs(const RegMap& regs)
    {
        int firstRegs = 0;
        int secondRegs = 0;
        for (const auto& reg : regs)
        {
            if (reg.first < 6)
                ++firstRegs;
            else
                ++secondRegs;
        }
        return std::tuple<int, int>(firstRegs, secondRegs);
    }

    int pl0xTimings(const RegMap& regs)
    {
        const auto [firstRegs, secondRegs] = splitRegs(regs);
        bool psg2 = isPsg2(regs);
        if (!psg2)
            return 21 + 5 + pl00TimeForFrame(regs);

        // PSG2 timings
        int result = 44; //< Till jump to play_all_0_5
        if (firstRegs < 6)
        {
            // play_by_mask_0_5
            if (regs.count(0) == 0)
                result += 4 + 12; //< There is no reg 0.
            else
                result += 47;
        }

        return result;
    }

    int getFrameDuration(const RegMap& regs)
    {
        int result = 32; //< before pl_frame
        result += pl0xTimings(regs);
        result += 33; //< before trb_rep
        result += 7+7+10; //< There is no trb_rep counter;

        return result;
    }

    void serializeFrame(uint16_t pos)
    {
        int prevSize = compressedData.size();

        uint16_t symbol = ayFrames[pos];
        auto regs = symbolToRegs[symbol];

        if (flags & dumpTimings)
           timingsData.push_back(getFrameDuration(regs));

        bool usePsg2 = isPsg2(regs);

        uint8_t header1 = 0;
        if (usePsg2)
        {
            auto mask = (makeRegMask(regs, 0, 6) >> 2);
            header1 = 0x40 + mask;
            compressedData.push_back(header1);

            int firstsHalfRegs = 0; //< Statistics
            for (const auto& reg : regs)
            {
                if (reg.first < 6)
                {
                    compressedData.push_back(reg.second); // reg value
                    ++firstsHalfRegs;
                }
            }
            ++stats.firstHalfRegs[firstsHalfRegs];
            ++stats.secondHalfRegs[regs.size() - firstsHalfRegs];

            uint8_t header2 = makeRegMask(regs, 6, 14);
            header2 = reverseBits(header2);
            compressedData.push_back(header2);

            if ((header2 & 0x7f) == 0)
            {
                // play_all branch. Serialize regs in regular order
                for (auto itr = regs.begin(); itr != regs.end(); ++itr)
                {
                    if (itr->first >= 6)
                        compressedData.push_back(itr->second);
                }
            }
            else
            {
                // play_by_mask branch. Serialize regs in backward order
                for (auto itr = regs.rbegin(); itr != regs.rend(); ++itr)
                {
                    if (itr->first >= 6)
                        compressedData.push_back(itr->second); // reg value
                }
            }
        }
        else
        {
            header1 = regs.size() == 1 ? 0x10 : 0;
            for (const auto& reg : regs)
            {
                compressedData.push_back(reg.first + header1);
                compressedData.push_back(reg.second); // reg value
                header1 = 0;
            }
        }

        stats.ownBytes += compressedData.size() - prevSize;

    }

    int serializedFrameSize(uint16_t pos)
    {
        const uint16_t symbol = ayFrames[pos];
        if (symbol <= kMaxDelay)
            return 1;

        auto regs = symbolToRegs[symbol];
        if (isPsg2(regs))
            return 2 + regs.size();
        return regs.size() * 2;
    };

    auto findRef(int pos)
    {
        const int maxLength = std::min(255, (int)ayFrames.size() - pos);

        int maxChainLen = -1;
        int chainPos = -1;
        int bestBenifit = 0;
        int maxReducedLen = -1;

        for (int i = 0; i < pos; ++i)
        {
            if (frameOffsets[pos] - frameOffsets[i] + 3 > kMaxRefOffset)
                continue;

            if (ayFrames[i] == ayFrames[pos] && refCount[i] == 0)
            {
                int chainLen = 0;
                int reducedLen = 0;
                int serializedSize = 0;
                std::vector<int> sizes;
                
                for (int j = 0; j < maxLength && i + j < pos; ++j)
                {
                    if (ayFrames[i + j] != ayFrames[pos + j] || refCount[i + j] > 1)
                        break;
                    ++chainLen;
                    if (refCount[i + j] == 0)
                        ++reducedLen; //< Don't count 1-symbol refs during ref serialization
                    serializedSize += serializedFrameSize(pos + j);
                    sizes.push_back(serializedSize);
                }
                while (refCount[i + chainLen - 1] == 1)
                {
                    sizes.pop_back();
                    --chainLen;
                }
                   
                int benifit = *sizes.rbegin() - (chainLen == 1 ? 2 : 3);
                if (benifit > bestBenifit)
                {
                    bestBenifit = benifit;
                    maxChainLen = chainLen;
                    maxReducedLen = reducedLen;
                    chainPos = i;
                }
            }
        }

#if 0
        if (flags & fastDepack)
        {
            if (maxChainLen == 1)
            {
                const auto regs = symbolToRegs[ayFrames[chainPos]];
                auto [firstRegs, secondRegs] = splitRegs(regs);

                if (firstRegs >= 4 && secondRegs >= 5)
                    forceLongRef = bool; //< Long refs is faster for 19t
            }
        }
#endif

        return std::tuple<int, int, int> { chainPos, maxChainLen, maxReducedLen};
    }

public:
    int parsePsg(const std::string& inputFileName)
    {
        using namespace std;

        ifstream fileIn;
        fileIn.open(inputFileName, std::ios::binary);
        if (!fileIn.is_open())
        {
            std::cerr << "Can't open input file " << inputFileName << std::endl;
            return -1;
        }

        fileIn.seekg(0, ios::end);
        int fileSize = fileIn.tellg();
        fileIn.seekg(0, ios::beg);

        srcPsgData.resize(fileSize);
        fileIn.read((char*)srcPsgData.data(), fileSize);
        firstFrame = true;

        const uint8_t* pos = srcPsgData.data() + 16;
        const uint8_t* end = srcPsgData.data() + srcPsgData.size();

        for (int i = 0; i <= kMaxDelay; ++i)
        {
            RegMap fakeRegs;
            fakeRegs[-1] = i;
            regsToSymbol.emplace(fakeRegs, i);
            symbolToRegs.emplace(i, fakeRegs);
        }

        int delayCounter = 0;

        while (pos < end)
        {
            uint8_t value = *pos;
            if (value >= 0xfe)
            {
                ++stats.psgFrames;
                if (!changedRegs.empty())
                {
                    if (!writeRegs())
                        ++delayCounter; //< Regs were cleaned up.
                }

                if (value == 0xff)
                {
                    ++delayCounter;
                    ++pos;
                }
                else
                {
                    delayCounter += pos[1] * 4;
                    pos += 2;
                }
            }
            else if (value == 0xfd)
            {
                break;
            }
            else
            {
                writeDelay(delayCounter - 1);
                delayCounter = 0;

                assert(value <= 13);
                changedRegs[value] = pos[1];
                lastOrigRegs[value] = pos[1];
                ++stats.regsChange[value];
                pos += 2;
            }
        }

        if (!changedRegs.empty())
        {
            if (!writeRegs())
                ++delayCounter; //< Regs were cleaned up.
        }
        writeDelay(delayCounter);

        return 0;
    }

    int packPsg(const std::string& outputFileName)
    {
        using namespace std;

        ofstream fileOut;
        fileOut.open(outputFileName, std::ios::binary | std::ios::trunc);
        if (!fileOut.is_open())
        {
            std::cerr << "Can't open output file " << outputFileName << std::endl;
            return -1;
        }


        // compressData
        refCount.resize(ayFrames.size());

        for (int i = 0; i < ayFrames.size();)
        {
            while (frameOffsets.size() <= i)
                frameOffsets.push_back(compressedData.size());

            if (ayFrames[i] <= kMaxDelay)
            {
                serializeEmptyFrames(ayFrames[i]);
                stats.emptyFrames += ayFrames[i];
                ++stats.emptyCnt;
                ++i;
            }
            else
            {
                const auto symbol = ayFrames[i];
                const auto currentRegs = symbolToRegs[symbol];

                const auto [pos, len, reducedLen] = findRef(i);
                if (len > 0)
                {
                    serializeRef(pos, reducedLen);

                    for (int j = i; j < i + len; ++j)
                        refCount[j] = len;

                    i += len;
                    if (len == 1)
                        stats.singleRepeat++;
                    stats.allRepeat++;
                    stats.allRepeatFrames += len;
                }
                else
                {
                    serializeFrame(i);
                    ++i;
                    ++stats.ownCnt;
                }
            }
        }

        compressedData.push_back(kEndTrackMarker);

        for (const auto& v : symbolToRegs)
            ++stats.frameRegs[v.second.size()];


        fileOut.write((const char*)compressedData.data(), compressedData.size());
        fileOut.close();

        return 0;
    }

    int writeRawPsg(const std::string& outputFileName)
    {
        using namespace std;

        ofstream fileOut;
        fileOut.open(outputFileName, std::ios::binary | std::ios::trunc);
        if (!fileOut.is_open())
        {
            std::cerr << "Can't open output file " << outputFileName << std::endl;
            return -1;
        }

        fileOut.write((const char*) updatedPsgData.data(), updatedPsgData.size());

        return 0;
    }

};

bool hasShortOpt(const std::string& s, char option)
{
    if (s.size() >= 2 && s[0] == '-' && s[1] == '-')
        return false;
    if (s.empty() || s[0] != '-')
        return false;
    return s.find(option) != std::string::npos;
}

int main(int argc, char** argv)
{
    PgsPacker packer;

    std::cout << "Fast PSG packer v.0.5a" << std::endl;
    if (argc < 3)
    {
        std::cout << "Usage: psg_pack [OPTION] input_file output_file" << std::endl;
        std::cout << "Example: psg_pack -fc file1.psg packetd.mus" << std::endl;
        std::cout << "Default options: --fast --clean" << std::endl;
        std::cout << "" << std::endl;
        std::cout << "Options:" << std::endl;
        std::cout << "-f, --fast\t Optimize archive for unpacking (max 799t)." << std::endl;
        std::cout << "-n, --normal\t Better compression but more slow unpacking (~920t)." << std::endl;
        std::cout << "-c, --clean\t Clean AY registers before packing. Improve compression level but incompatible with some tracks." << std::endl;
        std::cout << "-k, --keep\t --Don't clean AY regiaters." << std::endl;
        std::cout << "-i, --info\t Print timings info for each compresed frame." << std::endl;
        std::cout << "-d, --dump\t Dump uncompressed PSG frame to the separate file." << std::endl;
        return -1;
    }

    for (int i = 1; i < argc - 2; ++i)
    {
        const std::string s = argv[i];
        if (hasShortOpt(s,'f') || s == "--fast")
        {
            packer.flags |= fastDepack;
        }
        else if (hasShortOpt(s, 'n') || s == "--normal")
        {
            packer.flags &= ~fastDepack;
        }
        else if (hasShortOpt(s, 'c') || s == "--clean")
        {
            packer.flags |= cleanRegs;
        }
        else if (hasShortOpt(s, 'k') || s == "--keep")
        {
            packer.flags &= ~cleanRegs;
        }
        else if (hasShortOpt(s, 'd') || s == "--dump")
        {
            packer.flags |= dumpPsg;
        }
        else if (hasShortOpt(s, 'i') || s == "--info")
        {
            packer.flags |= dumpTimings;
            return -1;
        }
        else
        {
            std::cerr << "Unknown parameter " << s << std::endl;
            return -1;
        }
    }

    using namespace std::chrono;

    std::cout << "Starting compression..." << std::endl;
    auto timeBegin = std::chrono::steady_clock::now();
    auto result = packer.parsePsg(argv[argc-2]);
    if (result == 0)
        result = packer.packPsg(argv[argc - 1]);
    if (result != 0)
        return result;
    if (packer.flags & dumpPsg)
        packer.writeRawPsg(std::string(argv[argc - 1]) + ".psg");

    auto timeEnd = steady_clock::now();

    std::cout << "Compression done in " << duration_cast<milliseconds>(timeEnd - timeBegin).count() / 1000.0 << " second(s)" << std::endl;
    std::cout << "Input size:\t" << packer.srcPsgData.size() << std::endl;
    std::cout << "Packed size:\t" << packer.compressedData.size() << std::endl;
    std::cout << "1-byte refs:\t" << packer.stats.singleRepeat << std::endl;
    std::cout << "Total refs:\t" << packer.stats.allRepeat << std::endl;
    std::cout << "Total frames:\t" << packer.ayFrames.size() << std::endl;
    std::cout << "Ref frames:\t" << packer.stats.allRepeatFrames << std::endl;
    std::cout << "Empty frames:\t" << packer.stats.emptyCnt << std::endl;
    std::cout << "PSG frames:\t" << packer.stats.psgFrames << std::endl;

    return 0;
}
