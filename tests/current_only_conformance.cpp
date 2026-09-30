#include "ternary_os.h"

#include <cassert>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

int main() {
    using namespace sandbox;
    using namespace sandbox::vm;

    static_assert(architecture::v2::ISA_VERSION == 2);
    static_assert(architecture::v3::EXECUTABLE_VERSION == 3);
    static_assert(architecture::v3::FUNCTION_ABI_VERSION == 3);
    static_assert(architecture::v3::SYSCALL_ABI_VERSION == 2);
    static_assert(architecture::v3::VECTOR_ABI_VERSION == 1);
    static_assert(architecture::v2::TBOOT_WRITE_VERSION == 3);
    static_assert(architecture::v2::TDISK_WRITE_VERSION == 2);

    ExecutableImageHeaderV3 header = makeExecutableHeaderV3(7, 19, 3, 27);
    assert(validateExecutableHeaderV3(header));
    const std::vector<TernaryValue> encoded = encodeExecutableHeaderV3(header);
    assert(encoded.size() == static_cast<std::size_t>(EXEC_V3_HEADER_WORDS));

    ExecutableImageHeaderV3 decoded;
    assert(decodeExecutableHeaderV3(encoded, 0, decoded));
    assert(decoded.executable_version == architecture::v3::EXECUTABLE_VERSION);
    assert(decoded.function_abi_version == architecture::v3::FUNCTION_ABI_VERSION);
    assert(decoded.isa_version == architecture::v2::ISA_VERSION);
    assert(decoded.vector_lane_count == architecture::v3::VECTOR_LANE_COUNT);

    auto retired_header = encoded;
    retired_header[EXEC_V3_EXECUTABLE_VERSION] = sandbox::vm::ops::fromLong(2);
    ExecutableHeaderVariant retired_variant;
    assert(!decodeExecutableHeaderVersioned(retired_header, 0, retired_variant));

    const std::filesystem::path disk_path =
        std::filesystem::temp_directory_path() /
        "trit-current-platform-conformance.tdisk";
    std::error_code cleanup_error;
    std::filesystem::remove(disk_path, cleanup_error);
    {
        os::BlockDevice disk(8, disk_path.string());
        std::vector<long long> block(STORAGE_BLOCK_WORDS, 0);
        block[0] = 42;
        assert(disk.writeBlock(2, block).ok());
    }
    {
        std::ifstream image(disk_path, std::ios::binary);
        std::uint64_t magic = 0;
        std::uint32_t version = 0;
        std::uint32_t block_words = 0;
        image.read(reinterpret_cast<char*>(&magic), sizeof(magic));
        image.read(reinterpret_cast<char*>(&version), sizeof(version));
        image.read(reinterpret_cast<char*>(&block_words), sizeof(block_words));
        assert(image.good());
        assert(magic == 0x54524954535032ULL);
        assert(version == 2);
        assert(block_words == STORAGE_BLOCK_WORDS);

        os::BlockDevice restored(8, disk_path.string());
        std::vector<long long> restored_block;
        assert(restored.readBlock(2, restored_block).ok());
        assert(restored_block.size() == static_cast<std::size_t>(STORAGE_BLOCK_WORDS));
        assert(restored_block[0] == 42);
    }
    std::filesystem::remove(disk_path, cleanup_error);

    {
        std::ofstream retired(disk_path, std::ios::binary | std::ios::trunc);
        const std::uint64_t retired_magic = 0x54524954535031ULL;
        retired.write(reinterpret_cast<const char*>(&retired_magic),
                      sizeof(retired_magic));
        const std::int32_t retired_count = 0;
        retired.write(reinterpret_cast<const char*>(&retired_count),
                      sizeof(retired_count));
    }
    os::BlockDevice retired_disk(8);
    assert(!retired_disk.attachBackingFile(disk_path.string()).ok());
    std::filesystem::remove(disk_path, cleanup_error);

    std::cout << "current platform conformance: ISA v2, executable ABI v3\n";
    return 0;
}
