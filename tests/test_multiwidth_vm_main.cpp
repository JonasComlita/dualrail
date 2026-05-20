#include "test_multiwidth_vm_common.h"

int g_failures = 0;

// Forward declarations of modular test functions
void testUInt128Core();
void testFormatTraits();
void testIntegerFormats();
void testFloatFormats();
void testFractionalAlignmentAndSqrt();
void testIsaAndAsmWidths();
void testVmWidths();
void testPhase35Infrastructure();
void testOsSubstrate();
void testTernaryAtomicsAndLockAbi();
void testNoBridgeInExecutionHeaders();

int main() {
    std::cerr << "DEBUG: main started" << std::endl;
    std::cerr << "DEBUG: calling initPowTable" << std::endl;
    sandbox::LongTriple::initPowTable();

    std::cerr << "DEBUG: calling testUInt128Core" << std::endl;
    testUInt128Core();
    std::cerr << "DEBUG: calling testFormatTraits" << std::endl;
    testFormatTraits();
    std::cerr << "DEBUG: calling testIntegerFormats" << std::endl;
    testIntegerFormats();
    std::cerr << "DEBUG: calling testFloatFormats" << std::endl;
    testFloatFormats();
    std::cerr << "DEBUG: calling testFractionalAlignmentAndSqrt" << std::endl;
    testFractionalAlignmentAndSqrt();
    std::cerr << "DEBUG: calling testIsaAndAsmWidths" << std::endl;
    testIsaAndAsmWidths();
    std::cerr << "DEBUG: calling testVmWidths" << std::endl;
    testVmWidths();
    std::cerr << "DEBUG: calling testPhase35Infrastructure" << std::endl;
    testPhase35Infrastructure();
    std::cerr << "DEBUG: calling testOsSubstrate" << std::endl;
    testOsSubstrate();
    std::cerr << "DEBUG: calling testTernaryAtomicsAndLockAbi" << std::endl;
    testTernaryAtomicsAndLockAbi();
    std::cerr << "DEBUG: calling testNoBridgeInExecutionHeaders" << std::endl;
    testNoBridgeInExecutionHeaders();


    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " multi-width test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll multi-width tests passed\n";
    return EXIT_SUCCESS;
}
