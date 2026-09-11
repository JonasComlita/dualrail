#include "ternary_vm_state.h"
#include "ternary_compiler.h"
#include "ternary_os.h"
#include "ternary_vm.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const std::string& message) {
    if (condition) return;
    ++failures;
    std::cout << "FAIL: " << message << "\n";
}

void testVersionedHeaders() {
    using namespace sandbox;
    using namespace sandbox::vm;

    const auto v3 = makeExecutableHeaderV3(7, 81, 54, 27);
    expect(validateExecutableHeaderV3(v3),
           "default v3 header satisfies the exact geometry contract");
    expect(v3.header_words == 20 &&
               v3.vector_register_count == architecture::v3::VECTOR_REGISTER_COUNT &&
               v3.vector_lane_count == architecture::v3::VECTOR_LANE_COUNT &&
               v3.vector_context_words == architecture::v3::VECTOR_CONTEXT_WORDS,
           "v3 header exposes fixed vector geometry and 279-word context");
    expect(validateExecutableHeaderCommon(
               executableHeaderCommonView(v3), executableHeaderV3Contract()),
           "v3 common view validates against the v3 profile");

    const std::vector<TernaryValue> encoded = encodeExecutableHeaderV3(v3);
    expect(static_cast<int>(encoded.size()) == EXEC_V3_HEADER_WORDS,
           "v3 codec emits exactly 20 words");
    ExecutableImageHeaderV3 decoded;
    expect(decodeExecutableHeaderV3(encoded, 0, decoded),
           "v3 codec round-trips a valid header");
    expect(decoded.required_features == v3.required_features &&
               decoded.vector_spill_words == v3.vector_spill_words,
           "v3 codec preserves feature and spill geometry fields");

    auto bad_geometry = v3;
    bad_geometry.vector_lane_count = 26;
    expect(!validateExecutableHeaderV3(bad_geometry),
           "v3 rejects a non-authoritative lane count");
    auto bad_feature = v3;
    bad_feature.required_features |=
        featureBit(architecture::v3::FEATURE_V3_LAST + 1);
    bad_feature.header_checksum = executableHeaderV3Checksum(bad_feature);
    expect(!validateExecutableHeaderV3(bad_feature),
           "v3 rejects feature bits outside its exact supported mask");
    ExecutableImageHeaderV2 v2;
    expect(!decodeExecutableHeaderV2(encoded, 0, v2),
           "v2 decoder fails closed on a v3 envelope");

    const auto legacy = makeExecutableHeaderV2(2, 9, 0, 9);
    expect(validateExecutableHeaderV2(legacy),
           "v2 header remains valid after common-view refactor");
    expect(!validateExecutableHeaderCommon(
               executableHeaderCommonView(legacy), executableHeaderV3Contract()),
           "v2 common view is not accepted as a v3 header");
}

void testExactTaskContextRoundTrip() {
    using namespace sandbox;
    using namespace sandbox::vm;

    expect(TASK_CONTEXT_V3_WORDS == 279 &&
               TASK_CONTEXT_V3_FAULT_BASE == 7 &&
               TASK_CONTEXT_V3_VECTOR_BASE == 34 &&
               TASK_CONTEXT_V3_RESERVED_BASE == 250 &&
               TASK_CONTEXT_V3_FIRST_FAILING_LANE == 6,
           "C++ v3 context offsets total exactly 279 words");

    VMState vm(64, 2048);
    vm.reset();
    vm.epc = 17;
    vm.status = VMStatus::WAITING;
    vm.user_imem_ptbr = 101;
    vm.user_imem_pages = 4;
    vm.user_dmem_ptbr = 202;
    vm.user_dmem_pages = 6;
    vm.regfile.write(3, sandbox::vm::ops::fromLong(123));
    vm.vregfile.reg[2].write(4, sandbox::vm::ops::fromLong(9, TernaryMode::T20));
    vm.vector_faults.setLane(4, TrapCode::TRAP_ILLEGAL_OP);
    vm.accumulator = sandbox::vm::ops::fromLong(8);
    vm.privilege = PrivilegeMode::Kernel;
    vm.required_features = architecture::v3::REQUIRED_FEATURES;
    vm.supported_features = architecture::v3::SUPPORTED_FEATURES;

    const auto header = makeExecutableHeaderV3(17, 81, 54, 27);
    TernaryMemory context(4096);
    constexpr int context_addr = 108;
    expect(initializeTaskContextV3(context, context_addr, header, 300, 400),
           "v3 context initializer prevalidates and initializes 279 words");
    auto version_word = context.load(context_addr + TASK_CONTEXT_V3_HEADER_VERSION);
    auto length_word = context.load(context_addr + TASK_CONTEXT_V3_HEADER_LENGTH);
    auto vector_abi_word = context.load(
        context_addr + TASK_CONTEXT_V3_HEADER_VECTOR_ABI);
    auto register_count_word = context.load(
        context_addr + TASK_CONTEXT_V3_HEADER_REGISTER_COUNT);
    expect(version_word.second == MemFaultCode::OK &&
               sandbox::vm::ops::toLong(version_word.first) == 1 &&
               vector_abi_word.second == MemFaultCode::OK &&
               sandbox::vm::ops::toLong(vector_abi_word.first) == 1 &&
               register_count_word.second == MemFaultCode::OK &&
               sandbox::vm::ops::toLong(register_count_word.first) == 8,
           "v3 context initializer records version and vector geometry");
    expect(length_word.second == MemFaultCode::OK &&
               sandbox::vm::ops::toLong(length_word.first) == 279,
           "v3 context initializer records the exact context length");
    auto vlen_word = context.load(context_addr + TASK_CONTEXT_V3_VLEN);
    expect(vlen_word.second == MemFaultCode::OK &&
               sandbox::vm::ops::toLong(vlen_word.first) == 27,
           "v3 context initializer records fixed VLEN");

    expect(saveTaskContextV3(vm, context, context_addr),
           "v3 context save serializes vector-owned tagged state");
    vm.vregfile.reset(vm.vector_length);
    vm.vector_faults.clear();
    vm.accumulator = sandbox::vm::ops::fromLong(0);
    const int saved_epc = vm.epc;
    const VMStatus saved_status = vm.status;
    vm.regfile.reset();
    expect(restoreTaskContextV3(vm, context, context_addr),
           "v3 context restore validates then restores vector state");
    expect(vm.epc == saved_epc && vm.status == saved_status &&
               sandbox::vm::ops::toLong(vm.vregfile.reg[2].read(4)) == 9 &&
               vm.vector_faults.first_failing_lane == 4 &&
               vm.vector_faults.any() &&
               sandbox::vm::ops::toLong(vm.accumulator) == 8,
           "v3 context round-trip preserves tagged vectors, faults, and accumulator");

    const auto before = vm.regfile.readPhysical(3);
    expect(context.store(context_addr + TASK_CONTEXT_V3_VLEN,
                         sandbox::vm::ops::fromLong(26)) == MemFaultCode::OK,
           "test can corrupt a context word");
    expect(!restoreTaskContextV3(vm, context, context_addr),
           "restore rejects a context with the wrong fixed VLEN");
    expect(vm.regfile.readPhysical(3) == before,
           "failed restore does not partially mutate live state");
    expect(!saveTaskContextV3(vm, context, context.size() - 278),
           "save rejects a truncated context span before writing");
}

void testVectorContextInstructions() {
    using namespace sandbox;
    using namespace sandbox::isa;
    using namespace sandbox::vm;

    for (int reg = 0; reg < REG_COUNT; ++reg) {
        for (const Opcode opcode : {Opcode::VCTXSTORE, Opcode::VCTXLOAD}) {
            const int offset = opcode == Opcode::VCTXSTORE ? -121 : 121;
            const TritWord27 encoded = encodeVectorContext(
                opcode, static_cast<uint8_t>(reg), offset);
            const VectorContextInstruction decoded =
                decodeVectorContext(encoded);
            expect(decoded.valid && decoded.opcode == opcode &&
                       decoded.context_register == reg &&
                       decoded.offset == offset,
                   "VCTX codec preserves every base register and signed offset");
        }
    }

    const TritWord27 canonical =
        encodeVectorContext(Opcode::VCTXSTORE, R5, -121);
    for (int trit = 0; trit < ISA_WORD_TRITS; ++trit) {
        TritWord27 malformed = canonical;
        const std::uint64_t shift = static_cast<std::uint64_t>(2 * trit);
        malformed.bits = (malformed.bits & ~(0x3ULL << shift)) |
                         (0x3ULL << shift);
        expect(!decodeVectorContext(malformed).valid,
               "VCTX decoder rejects invalid trits in every word position");
    }
    TritWord27 noncanonical = canonical;
    noncanonical.setTrit(FIELD_VCTX_RSVD_LSB, T_POS);
    expect(!decodeVectorContext(noncanonical).valid,
           "VCTX decoder requires neutral reserved trits");

    VMState vm(64, 4096);
    vm.reset();
    vm.executable_version = architecture::v3::EXECUTABLE_VERSION;
    vm.required_features = architecture::v3::REQUIRED_FEATURES;
    vm.supported_features = architecture::v3::SUPPORTED_FEATURES;
    vm.privilege = PrivilegeMode::Kernel;
    vm.vector_length = architecture::v3::VECTOR_LANE_COUNT;
    constexpr int context_addr = 513; // 57 * the 9-word stack alignment.
    vm.regfile.write(R5, sandbox::vm::ops::fromLong(context_addr));
    vm.vregfile.reg[0].write(2, sandbox::vm::ops::fromLong(11, TernaryMode::T20));
    vm.vregfile.reg[1].write(7, sandbox::vm::ops::fromLong(-6, TernaryMode::T20));
    vm.vector_faults.setLane(7, TrapCode::TRAP_DIV_ZERO);
    vm.accumulator = sandbox::vm::ops::fromLong(14);

    const auto header = makeExecutableHeaderV3(0, 3, 0, 27);
    expect(initializeTaskContextV3(vm.dmem, context_addr, header, 0, 0),
           "direct v3 VCTX context setup succeeds");
    expect(vm.imem.write(0, encodeVectorContext(Opcode::VCTXSTORE, R5)) ==
               MemFaultCode::OK &&
               vm.imem.write(1, encodeVectorContext(Opcode::VCTXLOAD, R5)) ==
               MemFaultCode::OK &&
               vm.imem.write(2, InstructionWord::encodeSemanticB(
                   Opcode::HALT, R0_ZERO, 0)) == MemFaultCode::OK,
           "VCTXSTORE/VCTXLOAD instructions encode into executable memory");

    const RunResult stored = run(vm, 1);
    expect(stored.timeout() && vm.pc == 1,
           "privileged VCTXSTORE commits and advances one instruction");
    vm.vregfile.reg[0].write(2, sandbox::vm::ops::fromLong(0));
    vm.vregfile.reg[1].write(7, sandbox::vm::ops::fromLong(0));
    vm.vector_faults.clear();
    vm.accumulator = sandbox::vm::ops::fromLong(0);
    const RunResult loaded = run(vm, 4);
    expect(loaded.halted() &&
               sandbox::vm::ops::toLong(vm.vregfile.reg[0].read(2)) == 11 &&
               sandbox::vm::ops::toLong(vm.vregfile.reg[1].read(7)) == -6 &&
               vm.vector_faults.first_failing_lane == 7 &&
               sandbox::vm::ops::toLong(vm.accumulator) == 14,
           "VCTXLOAD restores tagged lanes, lane fault, and accumulator");

    VMState user_vm(16, 4096);
    user_vm.reset();
    user_vm.executable_version = architecture::v3::EXECUTABLE_VERSION;
    user_vm.required_features = architecture::v3::REQUIRED_FEATURES;
    user_vm.supported_features = architecture::v3::SUPPORTED_FEATURES;
    user_vm.privilege = PrivilegeMode::User;
    user_vm.vector_length = architecture::v3::VECTOR_LANE_COUNT;
    user_vm.regfile.write(R6, sandbox::vm::ops::fromLong(context_addr));
    expect(user_vm.imem.write(0, encodeVectorContext(Opcode::VCTXSTORE, R6)) ==
               MemFaultCode::OK,
           "user-mode rejection fixture encodes VCTXSTORE");
    const RunResult rejected = run(user_vm, 2);
    expect(rejected.trapped() &&
               decodeTrap(user_vm.trap_reg) == TrapCode::TRAP_ILLEGAL_OP,
            "VCTXSTORE fails closed outside kernel privilege");

    VMState malformed_vm(16, 4096);
    malformed_vm.reset();
    malformed_vm.executable_version = architecture::v3::EXECUTABLE_VERSION;
    malformed_vm.required_features = architecture::v3::REQUIRED_FEATURES;
    malformed_vm.supported_features = architecture::v3::SUPPORTED_FEATURES;
    malformed_vm.privilege = PrivilegeMode::Kernel;
    malformed_vm.regfile.write(R5, sandbox::vm::ops::fromLong(context_addr));
    TritWord27 malformed_instruction =
        encodeVectorContext(Opcode::VCTXSTORE, R5);
    malformed_instruction.bits |= 0x3ULL;
    expect(malformed_vm.imem.write(0, malformed_instruction) == MemFaultCode::OK,
           "malformed VCTX fixture reaches instruction memory");
    const RunResult malformed_result = run(malformed_vm, 1);
    expect(malformed_result.trapped() && malformed_vm.pc == 0 &&
               decodeTrap(malformed_vm.trap_reg) == TrapCode::TRAP_ILLEGAL_OP,
           "malformed VCTX traps before privileged execution or PC retirement");
}

void testCompilerVectorOptIn() {
    using namespace sandbox;
    using namespace sandbox::compiler;

    const TypeRef vector_type = TypeRef::vector(
        TypeRef::numeric(sandbox::ir::Type::T20));
    CompilerOptions legacy;
    expect(compilerSpillSlotWidth(vector_type, legacy) == 0,
           "vector spill geometry is disabled by default");

    CompilerOptions opt_in;
    opt_in.target_abi_version = FunctionAbiContract::version_v3;
    opt_in.target_executable_version = architecture::v3::EXECUTABLE_VERSION;
    opt_in.enable_vector_abi = true;
    opt_in.enable_vector_spilling = true;
    expect(compilerVectorAbiEnabled(opt_in),
           "v3 vector allocator profile requires explicit opt-in");
    expect(compilerSpillSlotWidth(vector_type, opt_in) == 27,
           "opt-in vector spills reserve one VLEN-sized slot");

    Module module;
    module.name = "vector_allocator_contract";
    Function fn;
    fn.name = "vector_pressure";
    BasicBlock block;
    block.name = "entry";
    for (int value = 1; value <= 9; ++value) {
        Instr constant;
        constant.def = value;
        constant.opcode = InstrOpcode::Const;
        constant.type = vector_type;
        constant.imm = value;
        block.instructions.push_back(constant);
    }
    Instr call;
    call.def = 10;
    call.opcode = InstrOpcode::Call;
    call.type = vector_type;
    call.symbol = "vector_sink";
    for (int value = 1; value <= 9; ++value) call.args.push_back(value);
    block.instructions.push_back(call);
    block.terminator.kind = TerminatorKind::Return;
    fn.blocks.push_back(block);
    module.functions.push_back(fn);

    const AllocationResult rejected = allocateRegisters(module);
    expect(!rejected.success && !rejected.diagnostics.empty(),
           "default allocator fails closed for vector values");
    const AllocationResult allocated = allocateRegisters(module, opt_in);
    expect(allocated.success && !allocated.vector_registers.empty() &&
               allocated.spills > 0 && allocated.vector_spill_words == 27,
           "opt-in allocator uses vector registers and 27-word spill geometry");
}

void testV3BuildPackageLaunchIdentity() {
    using namespace sandbox;
    using namespace sandbox::compiler;
    using namespace sandbox::vm;

    CompilerOptions compile_options;
    compile_options.target_abi_version = FunctionAbiContract::version_v3;
    compile_options.target_executable_version =
        architecture::v3::EXECUTABLE_VERSION;
    compile_options.enable_vector_abi = true;
    compile_options.enable_vector_spilling = true;
    const CompileResult compiled = compileSource(
        "abi_v3_vector_probe.trit",
        "fn identity(v: vec<t20>) -> vec<t20> { return v; }\n"
        "fn main() -> t40 { return 7; }\n",
        compile_options);
    expect(compiled.success,
           "focused ABI v3 vector application compiles");
    if (!compiled.success) return;

    LinkOptions link_options;
    link_options.function_abi_version = FunctionAbiContract::version_v3;
    link_options.executable_version = architecture::v3::EXECUTABLE_VERSION;
    link_options.enable_vector_abi = true;
    link_options.enable_vector_spilling = true;
    link_options.vector_abi_version = architecture::v3::VECTOR_ABI_VERSION;
    const LinkResult linked = linkModules({compiled.object}, link_options);
    expect(linked.success && linked.assembled.executable_headers_v3.count(
               "phase7_exec") == 1,
           "focused ABI v3 vector application packages as executable v3");
    if (!linked.success) return;
    expect(validateExecutableHeaderV3(linked.executable_header_v3) &&
               linked.executable_header_v3.function_abi_version ==
                   FunctionAbiContract::version_v3 &&
               linked.executable_header_v3.vector_context_words == 279,
           "ABI identity remains consistent through the linker header");

    VMState vm(4096, 4096);
    vm.setExecutionBackend(VMExecutionBackend::Interpreter);
    expect(assembler::loadAndReset(vm, linked.assembled),
           "focused ABI v3 vector application loads in the VM");
    const auto run_result = sandbox::vm::run(vm, 1000000);
    expect(run_result.halted() &&
               sandbox::vm::ops::toLong(vm.regfile.read(13)) == 7,
           "focused ABI v3 vector application launches and returns correctly");
}

void testV3VectorCallsAndStackExecution() {
    using namespace sandbox;
    using namespace sandbox::compiler;
    using namespace sandbox::vm;

    CompilerOptions compile_options;
    compile_options.target_abi_version = FunctionAbiContract::version_v3;
    compile_options.target_executable_version =
        architecture::v3::EXECUTABLE_VERSION;
    compile_options.enable_vector_abi = true;
    compile_options.enable_vector_spilling = true;
    const CompileResult compiled = compileSource(
        "abi_v3_vector_calls.trit",
        R"TRIT(
            fn identity(v: vec<t20>) -> vec<t20> { return v; }
            fn select_fifth(a: vec<t20>, b: vec<t20>, c: vec<t20>,
                            d: vec<t20>, e: vec<t20>) -> vec<t20> { return e; }
            fn main(v: vec<t20>) -> vec<t20> {
                return select_fifth(v, v, v, v, identity(v));
            }
        )TRIT",
        compile_options);
    expect(compiled.success,
           "ABI v3 vector calls, fifth stack argument, and vector return compile");
    if (!compiled.success) {
        for (const auto& diagnostic : compiled.diagnostics)
            std::cout << "vector-call diagnostic: " << diagnostic.format() << "\n";
        return;
    }

    LinkOptions link_options;
    link_options.function_abi_version = FunctionAbiContract::version_v3;
    link_options.executable_version = architecture::v3::EXECUTABLE_VERSION;
    link_options.enable_vector_abi = true;
    link_options.enable_vector_spilling = true;
    link_options.vector_abi_version = architecture::v3::VECTOR_ABI_VERSION;
    const LinkResult linked = linkModules({compiled.object}, link_options);
    expect(linked.success,
           "ABI v3 vector calls and stack arguments link into executable v3");
    if (!linked.success) return;

    VMState vm(4096, 8192);
    vm.setExecutionBackend(VMExecutionBackend::Interpreter);
    expect(assembler::loadAndReset(vm, linked.assembled),
           "ABI v3 vector call image loads in the VM");
    vm.vector_length = architecture::v3::VECTOR_LANE_COUNT;
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        vm.vregfile.reg[0].write(
            lane, sandbox::vm::ops::fromLong(lane - 4, TernaryMode::T20));
    }
    const RunResult result = run(vm, 1000000);
    expect(result.halted(), "ABI v3 vector call image halts");
    bool preserved = true;
    for (int lane = 0; lane < vm.vector_length; ++lane) {
        preserved = preserved &&
            sandbox::vm::ops::toLong(vm.vregfile.reg[0].read(lane)) ==
                lane - 4;
    }
    expect(preserved,
           "ABI v3 vector return and fifth 27-word stack argument preserve lanes");
}

void testV3VectorOperationsExecution() {
    using namespace sandbox;
    using namespace sandbox::compiler;
    using namespace sandbox::vm;

    auto runVectorProgram = [](const std::string& source,
                               const std::vector<long long>& expected,
                               const std::string& label,
                               bool require_spill = false) {
        CompilerOptions compile_options;
        compile_options.target_abi_version = FunctionAbiContract::version_v3;
        compile_options.target_executable_version =
            architecture::v3::EXECUTABLE_VERSION;
        compile_options.enable_vector_abi = true;
        compile_options.enable_vector_spilling = true;
        const CompileResult compiled = compileSource(
            "abi_v3_vector_operations.trit", source, compile_options);
        expect(compiled.success, label + " compiles");
        if (!compiled.success) {
            for (const auto& diagnostic : compiled.diagnostics)
                std::cout << label << " diagnostic: " << diagnostic.format() << "\n";
            return;
        }
        if (require_spill) {
            expect(compiled.allocation.spills > 0,
                   label + " exercises the 27-word vector spill class");
        }

        LinkOptions link_options;
        link_options.function_abi_version = FunctionAbiContract::version_v3;
        link_options.executable_version = architecture::v3::EXECUTABLE_VERSION;
        link_options.enable_vector_abi = true;
        link_options.enable_vector_spilling = true;
        link_options.vector_abi_version = architecture::v3::VECTOR_ABI_VERSION;
        const LinkResult linked = linkModules({compiled.object}, link_options);
        expect(linked.success, label + " links into executable v3");
        if (!linked.success) return;

        VMState vm(4096, 8192);
        vm.setExecutionBackend(VMExecutionBackend::Interpreter);
        expect(assembler::loadAndReset(vm, linked.assembled),
               label + " image loads in the VM");
        vm.vector_length = architecture::v3::VECTOR_LANE_COUNT;
        for (int lane = 0; lane < vm.vector_length; ++lane) {
            vm.vregfile.reg[0].write(
                lane, sandbox::vm::ops::fromLong(lane - 4, TernaryMode::T20));
        }
        const RunResult result = run(vm, 1000000);
        expect(result.halted(), label + " image halts");
        bool matched = expected.size() ==
            static_cast<std::size_t>(vm.vector_length);
        for (int lane = 0; lane < vm.vector_length && matched; ++lane) {
            matched = sandbox::vm::ops::toLong(
                          vm.vregfile.reg[0].read(lane)) == expected[lane];
        }
        expect(matched, label + " produces the expected vector lanes");
    };

    std::vector<long long> doubled_negated(
        architecture::v3::VECTOR_LANE_COUNT);
    for (int lane = 0; lane < architecture::v3::VECTOR_LANE_COUNT; ++lane)
        doubled_negated[lane] = -2LL * (lane - 4);
    std::vector<long long> preserved_input(
        architecture::v3::VECTOR_LANE_COUNT);
    for (int lane = 0; lane < architecture::v3::VECTOR_LANE_COUNT; ++lane)
        preserved_input[lane] = lane - 4;
    runVectorProgram(
        R"TRIT(
            fn negate_sum(a: vec<t20>, b: vec<t20>) -> vec<t20> {
                return -(a + b);
            }
            fn main(v: vec<t20>) -> vec<t20> {
                return negate_sum(v, v);
            }
        )TRIT",
        doubled_negated,
        "ABI v3 vector arithmetic and unary lowering");

    std::vector<long long> comparisons(
        architecture::v3::VECTOR_LANE_COUNT);
    for (int lane = 0; lane < architecture::v3::VECTOR_LANE_COUNT; ++lane)
        comparisons[lane] = lane < 4 ? -1 : lane == 4 ? 0 : 1;
    runVectorProgram(
        R"TRIT(
            fn compare_neg(a: vec<t20>) -> vec<trit> {
                return a < -a;
            }
            fn main(v: vec<t20>) -> vec<trit> {
                return compare_neg(v);
            }
        )TRIT",
        comparisons,
        "ABI v3 vector comparison lowering");

    runVectorProgram(
        R"TRIT(
            fn identity_for_spill(v: vec<t20>) -> vec<t20> { return v; }
            fn select_ninth(a: vec<t20>, b: vec<t20>, c: vec<t20>,
                           d: vec<t20>, e: vec<t20>, f: vec<t20>,
                           g: vec<t20>, h: vec<t20>, i: vec<t20>) -> vec<t20> {
                return i;
            }
            fn main(v: vec<t20>) -> vec<t20> {
                return select_ninth(
                    identity_for_spill(v), identity_for_spill(v),
                    identity_for_spill(v), identity_for_spill(v),
                    identity_for_spill(v), identity_for_spill(v),
                    identity_for_spill(v), identity_for_spill(v),
                    identity_for_spill(v));
            }
        )TRIT",
        preserved_input,
        "ABI v3 vector call-live spill execution",
        true);
}

void testV3OsPackageLaunchIdentity() {
    using namespace sandbox;
    using namespace sandbox::os;
    using namespace sandbox::vm;

    const std::vector<long long> image = {700, 701, 702, 703};
    const auto header = makeExecutableHeaderV3(
        2, static_cast<int>(image.size()), 0, 27);
    const SignedExecutableMetadata metadata = signExecutableMetadata(
        image, header, "abi-v3-test", "test-signing-key");
    PackageImageBuilder package_builder("abi-v3", 7);
    expect(package_builder.addExecutable(
               "/bin/abi-v3", image, header, metadata).ok(),
           "v3 executable package entry validates and records its ABI");
    const PackageImage package = package_builder.build();
    const std::vector<long long> manifest = encodePackageManifest(package);
    expect(!manifest.empty() && manifest.back() == architecture::v3::EXECUTABLE_VERSION,
           "v3 package metadata carries the selected executable version");

    OSKernel kernel(256);
    expect(kernel.boot().ok(), "v3 package test kernel boots");
    expect(installPackage(kernel, package).ok(),
           "v3 package installs through the OS package path");
    FileStat stat;
    expect(kernel.fs().stat("/bin/abi-v3", stat).ok() &&
               stat.executable_version == architecture::v3::EXECUTABLE_VERSION,
           "filesystem metadata reports executable ABI v3");
    expect(kernel.sysExec(1, "/bin/abi-v3").ok(),
           "v3 executable launches through sysExec");
    const Process* process = kernel.process(1);
    expect(process != nullptr && process->executable_header_is_v3 &&
               process->exec_header_v3.function_abi_version ==
                   architecture::v3::FUNCTION_ABI_VERSION &&
               process->architecture.executable_version ==
                   architecture::v3::EXECUTABLE_VERSION &&
               process->architecture.vector_abi_version ==
                   architecture::v3::VECTOR_ABI_VERSION,
           "process identity preserves v3 function/vector ABI metadata");
    ProcessInfo info;
    expect(kernel.sysGetProc(1, info).ok() &&
               info.executable_version == architecture::v3::EXECUTABLE_VERSION &&
               info.function_abi_version == architecture::v3::FUNCTION_ABI_VERSION &&
               info.vector_abi_version == architecture::v3::VECTOR_ABI_VERSION,
           "process information reports the same v3 architecture identity");

    const std::vector<long long> disk = kernel.diskImage();
    OSKernel rebooted(disk);
    expect(rebooted.boot().ok(), "v3 executable metadata survives filesystem reboot");
    expect(rebooted.sysExec(1, "/bin/abi-v3").ok(),
           "rebooted filesystem launches the v3 executable");
    const Process* rebooted_process = rebooted.process(1);
    expect(rebooted_process != nullptr &&
               rebooted_process->architecture.executable_version ==
                   architecture::v3::EXECUTABLE_VERSION,
           "rebooted process keeps the v3 executable identity");
}

void testV3ProcessVectorContextSwitches() {
    using namespace sandbox;
    using namespace sandbox::os;
    using namespace sandbox::vm;

    const std::vector<long long> image = {901, 902, 903, 904};
    const auto header = makeExecutableHeaderV3(
        2, static_cast<int>(image.size()), 0, 27);
    OSKernel kernel(256);
    expect(kernel.boot().ok() &&
               kernel.fs().createFile("/bin", InodeKind::Directory).ok() &&
               kernel.installExecutable("/bin/vector-task", image, header).ok() &&
               kernel.sysExec(1, "/bin/vector-task").ok(),
           "v3 process context switch fixture launches");
    const StatusResult forked = kernel.sysFork(1);
    expect(forked.ok(), "v3 fork creates a second vector-owning process");
    const int child_pid = forked.payload;
    expect(kernel.sysExec(child_pid, "/bin/vector-task").ok(),
           "child v3 exec resets its vector context");
    const Process* parent = kernel.process(1);
    const Process* child = kernel.process(child_pid);
    expect(parent && child && parent->vector_context && child->vector_context &&
               parent->vector_context.get() != child->vector_context.get(),
           "fork owns a distinct vector context allocation");

    auto makeState = [](long long seed) {
        VMState state(64, 4096);
        state.reset();
        state.executable_version = architecture::v3::EXECUTABLE_VERSION;
        state.function_abi_version = architecture::v3::FUNCTION_ABI_VERSION;
        state.vector_abi_version = architecture::v3::VECTOR_ABI_VERSION;
        state.required_features = architecture::v3::REQUIRED_FEATURES;
        state.supported_features = architecture::v3::SUPPORTED_FEATURES;
        state.privilege = PrivilegeMode::Kernel;
        state.vector_length = architecture::v3::VECTOR_LANE_COUNT;
        for (int reg = 0; reg < architecture::v3::VECTOR_REGISTER_COUNT; ++reg) {
            for (int lane = 0; lane < architecture::v3::VECTOR_LANE_COUNT; ++lane) {
                state.vregfile.reg[static_cast<std::size_t>(reg)].write(
                    lane, sandbox::vm::ops::fromLong(
                        seed + reg * 100 + lane, TernaryMode::T20));
            }
        }
        state.vector_faults.setLane(static_cast<int>(seed % 16),
                                    TrapCode::TRAP_ILLEGAL_OP);
        state.accumulator = sandbox::vm::ops::fromLong(
            seed * 7, TernaryMode::T40);
        return state;
    };

    VMState core = makeState(3);
    expect(kernel.saveProcessVectorContext(1, core).ok(),
           "scheduler saves exact parent vector context");
    core = makeState(11);
    expect(kernel.saveProcessVectorContext(child_pid, core).ok(),
           "scheduler saves exact child vector context");

    for (int switch_count = 0; switch_count < 100; ++switch_count) {
        expect(kernel.restoreProcessVectorContext(1, core).ok() &&
                   sandbox::vm::ops::toLong(core.vregfile.reg[7].read(26)) ==
                       3 + 700 + 26 &&
                   sandbox::vm::ops::toLong(core.accumulator) == 21,
               "parent vector context survives forced switch");
        expect(kernel.restoreProcessVectorContext(child_pid, core).ok() &&
                   sandbox::vm::ops::toLong(core.vregfile.reg[7].read(26)) ==
                       11 + 700 + 26 &&
                   sandbox::vm::ops::toLong(core.accumulator) == 77,
               "child vector context survives forced switch");
    }

    expect(kernel.sysExit(child_pid, 0).ok() &&
               kernel.sysWaitPid(1, child_pid).ok(),
           "vector context is released on exit and reap");
    const StatusResult reused = kernel.sysFork(1);
    expect(reused.ok() && kernel.sysExec(reused.payload, "/bin/vector-task").ok(),
           "reused task slot receives a fresh v3 vector context");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    testVersionedHeaders();
    testExactTaskContextRoundTrip();
    testVectorContextInstructions();
    testCompilerVectorOptIn();
    testV3BuildPackageLaunchIdentity();
    testV3VectorCallsAndStackExecution();
    testV3VectorOperationsExecution();
    testV3OsPackageLaunchIdentity();
    testV3ProcessVectorContextSwitches();
    if (failures != 0) {
        std::cout << failures << " executable ABI v3 failure(s)\n";
        return EXIT_FAILURE;
    }
    std::cout << "Executable ABI v3 and vector-boundary contracts passed\n";
    return EXIT_SUCCESS;
}
