#include "a5pack.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <stdexcept>

namespace hydra {

namespace {

constexpr std::size_t kHeaderSize = 0x5B;
constexpr std::size_t kFieldSize = 20;
constexpr std::size_t kSectionHeaderSize = 2 * kFieldSize + 4;

std::string fixedString(std::span<const std::uint8_t> field) {
    const auto* begin = reinterpret_cast<const char*>(field.data());
    return std::string(begin, std::find(begin, begin + field.size(), '\0'));
}

std::uint32_t readLe32(const std::uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (std::uint32_t(p[3]) << 24);
}

}  // namespace

FirmwarePack FirmwarePack::parse(std::span<const std::uint8_t> bytes) {
    if (bytes.size() < kHeaderSize || std::memcmp(bytes.data(), "A5Pack", 7) != 0)
        throw std::runtime_error("not an A5Pack firmware file");

    FirmwarePack pack;
    pack.model = fixedString(bytes.subspan(7, 4));

    std::size_t offset = kHeaderSize;
    while (offset < bytes.size()) {
        if (bytes.size() - offset < kSectionHeaderSize)
            throw std::runtime_error("A5Pack: truncated section header");
        const std::uint8_t* header = bytes.data() + offset;
        const std::uint32_t size = readLe32(header + 2 * kFieldSize);
        offset += kSectionHeaderSize;
        if (bytes.size() - offset < size)
            throw std::runtime_error("A5Pack: section exceeds file size");

        FirmwareSection section;
        section.version = fixedString({header, kFieldSize});
        section.name = fixedString({header + kFieldSize, kFieldSize});
        section.data.assign(bytes.begin() + offset, bytes.begin() + offset + size);
        pack.sections.push_back(std::move(section));
        offset += size;
    }
    return pack;
}

FirmwarePack FirmwarePack::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("cannot open firmware " + path.string());
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return parse(bytes);
}

const FirmwareSection* FirmwarePack::find(std::string_view name) const {
    for (const auto& section : sections)
        if (section.name == name)
            return &section;
    return nullptr;
}

}  // namespace hydra
