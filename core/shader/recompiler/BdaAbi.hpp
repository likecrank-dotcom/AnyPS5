#ifndef CORE_SHADER_RECOMPILER_BDAABI_HPP
#define CORE_SHADER_RECOMPILER_BDAABI_HPP

#include <bit>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace ShaderRecompiler::BdaAbi {

inline constexpr std::uint32_t Version = 2;
inline constexpr std::uint32_t Read = 1;
inline constexpr std::uint32_t Write = 2;

struct Header {
    std::uint32_t version;
    std::uint32_t count;
    std::uint32_t entryBytes;
    std::uint32_t reserved;
};

struct Range {
    std::uint64_t begin;
    std::uint64_t end;
    std::uint64_t deviceAddress;
    std::uint32_t permissions;
    std::uint32_t reserved;
};

enum class FaultState : std::uint32_t { Empty, Writing, Ready };
enum class FaultReason : std::uint32_t { Unmapped = 1, Permission, Overflow, InvalidTable, InvalidRectangle, LoopLimit, Unaligned, InvalidDescriptor };

struct Fault {
    FaultState state;
    FaultReason reason;
    std::uint64_t address;
    std::uint32_t bytes;
    std::uint32_t stage;
    std::uint32_t instruction;
    std::uint32_t reserved;
};

static_assert(std::is_standard_layout_v<Header> && std::is_trivially_copyable_v<Header> && sizeof(Header) == 16);
static_assert(std::is_standard_layout_v<Range> && std::is_trivially_copyable_v<Range> && sizeof(Range) == 32);
static_assert(offsetof(Range, deviceAddress) == 16 && offsetof(Range, permissions) == 24);
static_assert(std::is_standard_layout_v<Fault> && std::is_trivially_copyable_v<Fault> && sizeof(Fault) == 32);
static_assert(offsetof(Fault, address) == 8 && offsetof(Fault, instruction) == 24);

inline constexpr std::uint32_t WrittenPageShift = 12;
inline constexpr std::uint64_t WrittenAddressLimit = std::uint64_t{1} << 48;
inline constexpr std::uint32_t WrittenOverflowWord = sizeof(Fault) / sizeof(std::uint32_t);
inline constexpr std::size_t FaultBufferBytes = (WrittenOverflowWord + 1u) * sizeof(std::uint32_t);
inline constexpr std::uint32_t WrittenBitmapWord = static_cast<std::uint32_t>(FaultBufferBytes / sizeof(std::uint32_t));
inline constexpr std::uint32_t NoWrittenBitmap = UINT32_MAX;
inline constexpr std::uint64_t WrittenPageBytes = std::uint64_t{1} << WrittenPageShift;
static_assert(WrittenBitmapWord * sizeof(std::uint32_t) == FaultBufferBytes);

inline constexpr std::uint64_t WrittenBitmapPageCount(const Range& range) {
    return range.end <= range.begin ? 0 : ((range.end - 1u) >> WrittenPageShift) - (range.begin >> WrittenPageShift) + 1u;
}

inline constexpr std::uint64_t WrittenBitmapWordCount(const Range& range) {
    const auto pages = WrittenBitmapPageCount(range);
    return (pages + 31u) / 32u;
}

template<typename TMark>
bool ForEachWrittenRange(const Header& header, std::span<const Range> ranges, const std::uint32_t* words, std::size_t wordCount, TMark&& mark) {
    if (header.version != Version || header.entryBytes != sizeof(Range) || header.count != ranges.size()) return false;
    if (wordCount < WrittenBitmapWord || header.reserved > wordCount - WrittenBitmapWord) return false;
    std::uint64_t expectedBitmapWord = 0;
    std::uint64_t previousEnd = 0;
    for (const auto& range : ranges) {
        if (range.begin >= range.end || range.begin < previousEnd) return false;
        previousEnd = range.end;
        if ((range.permissions & Write) == 0) {
            if (range.reserved != NoWrittenBitmap) return false;
            continue;
        }
        if (range.end > WrittenAddressLimit || range.reserved != expectedBitmapWord) return false;
        const auto bitmapWords = WrittenBitmapWordCount(range);
        if (bitmapWords > header.reserved - expectedBitmapWord) return false;
        expectedBitmapWord += bitmapWords;
        const auto pages = WrittenBitmapPageCount(range);
        const auto finalPageBits = static_cast<std::uint32_t>(pages & 31u);
        if (finalPageBits != 0u && bitmapWords != 0u) {
            const auto last = words[WrittenBitmapWord + range.reserved + bitmapWords - 1u];
            if ((last >> finalPageBits) != 0u) return false;
        }
    }
    if (expectedBitmapWord != header.reserved) return false;

    bool hasRun = false;
    std::uint64_t runBegin = 0;
    std::uint64_t lastPage = 0;
    const auto flush = [&] {
        if (!hasRun) return;
        mark(runBegin, static_cast<std::size_t>(lastPage - runBegin + WrittenPageBytes));
        hasRun = false;
    };
    for (const auto& range : ranges) {
        if ((range.permissions & Write) == 0) continue;
        const auto firstPage = range.begin >> WrittenPageShift;
        const auto pages = WrittenBitmapPageCount(range);
        const auto bitmapWords = WrittenBitmapWordCount(range);
        for (std::uint64_t word = 0; word < bitmapWords; ++word) {
            auto mask = words[WrittenBitmapWord + range.reserved + word];
            while (mask != 0u) {
                const auto bit = static_cast<std::uint32_t>(std::countr_zero(mask));
                const auto pageIndex = word * 32u + bit;
                if (pageIndex >= pages) return false;
                const auto pageAddress = (firstPage + pageIndex) << WrittenPageShift;
                if (!hasRun) {
                    runBegin = lastPage = pageAddress;
                    hasRun = true;
                } else if (pageAddress == lastPage) {
                } else if (pageAddress == lastPage + WrittenPageBytes) {
                    lastPage = pageAddress;
                } else if (pageAddress > lastPage) {
                    flush();
                    runBegin = lastPage = pageAddress;
                    hasRun = true;
                } else {
                    return false;
                }
                mask &= mask - 1u;
            }
        }
    }
    flush();
    return true;
}

}

#endif
