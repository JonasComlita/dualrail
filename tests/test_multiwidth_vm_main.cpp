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
void testHelloWorldConsole();

int main() {
    sandbox::LongTriple::initPowTable();

    testUInt128Core();
    testFormatTraits();
    testIntegerFormats();
    testFloatFormats();
    testFractionalAlignmentAndSqrt();
    testIsaAndAsmWidths();
    testVmWidths();
    testPhase35Infrastructure();
    testOsSubstrate();
    testTernaryAtomicsAndLockAbi();
    testNoBridgeInExecutionHeaders();
    testHelloWorldConsole();

    if (g_failures != 0) {
        std::cout << "\n" << g_failures << " multi-width test failure(s)\n";
        return EXIT_FAILURE;
    }

    std::cout << "\nAll multi-width tests passed\n";
    return EXIT_SUCCESS;
}
