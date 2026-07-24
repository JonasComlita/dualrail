#include "tests_next/00_harness/next_test_harness.h"
#include "ternary_os.h"

#include <string>
#include <vector>

namespace {

using tests_next::TestCase;
using tests_next::TestContext;
using namespace sandbox::os;

bool containsWord(const std::vector<long long>& words, long long needle) {
    for (long long word : words) {
        if (word == needle) return true;
    }
    return false;
}

void journalPhaseReplayMatrix(TestContext& ctx) {
    const std::vector<JournalWritePhase> phases = {
        JournalWritePhase::BeforeBegin,
        JournalWritePhase::AfterBegin,
        JournalWritePhase::AfterRecord,
        JournalWritePhase::AfterCommit,
        JournalWritePhase::AfterApply,
        JournalWritePhase::AfterCheckpoint,
    };

    for (JournalWritePhase phase : phases) {
        JournalReplayHarness journal(4);
        ctx.check(journal.seed(1, 10).ok(), "journal seed succeeds");
        const StatusResult crashed = journal.simulateWriteCrash(1, 99, phase);
        ctx.check(crashed.ok() || crashed.status == T1_PENDING,
                  "journal crash phase is represented");
        ctx.check(journal.recover().ok(), "journal recovery succeeds");
        const bool committed =
            static_cast<int>(phase) >= static_cast<int>(JournalWritePhase::AfterCommit);
        ctx.equal(journal.read(1), committed ? 99LL : 10LL,
                  "journal recovery returns old or new value by commit boundary");
        ctx.equal(journal.pendingRecords(), 0,
                  "journal recovery drains pending records");
    }

    JournalReplayHarness invalid(2);
    ctx.equal(invalid.seed(9, 1).detail, ERR_INVALID,
              "journal rejects invalid seed address");
    ctx.equal(invalid.simulateWriteCrash(9, 2, JournalWritePhase::AfterRecord).detail,
              ERR_INVALID, "journal rejects invalid crash address");
}

void bootRecoveryCorruptSuperblock(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "kernel boots before corruption");
    ctx.check(kernel.fs().createFile("/etc", InodeKind::Directory).ok(),
              "etc directory creates");
    ctx.check(kernel.fs().createFile("/etc/config", InodeKind::File).ok(),
              "config file creates");
    ctx.check(kernel.fs().writeFile("/etc/config", {84, 82, 73, 84}).ok(),
              "config file writes");
    ctx.check(kernel.fs().createFile("/bin", InodeKind::Directory).ok(),
              "bin directory creates");

    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 2;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 32;
    ctx.check(kernel.installExecutable("/bin/init", {900, 901, 902}, header).ok(),
              "executable installs before fsck");
    ctx.check(kernel.checkFilesystemConsistency().ok(),
              "filesystem checker accepts clean image");

    std::vector<long long> image = kernel.diskImage();
    ctx.check(!image.empty(), "disk image exists before corruption");
    image[0] = 0;
    OSKernel broken(image);
    ctx.check(!broken.boot().ok(), "corrupt superblock fails normal boot");

    BootRecoveryReport recovered = broken.bootWithRecovery();
    ctx.check(recovered.mode == BootMode::Recovery,
              "boot recovery mode is selected");
    ctx.check(recovered.status.ok(), "boot recovery formats rescue root");
    ctx.check(recovered.fsck.ok(), "recovered root passes fsck");
    std::vector<long long> recovery_log;
    ctx.check(broken.fs().readFile("/var/log/recovery", recovery_log).ok(),
              "recovery mode writes recovery log");
    ctx.check(!recovery_log.empty(), "recovery log records failure reason");
}

void diskImageExecutableReboot(TestContext& ctx) {
    OSKernel kernel(160);
    ctx.check(kernel.boot().ok(), "kernel boots for disk-backed executable test");
    ctx.check(kernel.fs().createFile("/home", InodeKind::Directory).ok(),
              "home directory creates");
    ctx.check(kernel.fs().createFile("/home/note", InodeKind::File).ok(),
              "note file creates");
    ctx.check(kernel.fs().writeFile("/home/note", {84, 82, 73, 84}).ok(),
              "note file writes");
    ctx.check(kernel.fs().createFile("/bin", InodeKind::Directory).ok(),
              "bin directory creates");

    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 7;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 32;
    const std::vector<long long> app = {9001, 9002, 9003};
    ctx.check(kernel.installExecutable("/bin/app", app, header).ok(),
              "executable image installs into root filesystem");
    ctx.check(kernel.shutdownSync().ok(), "kernel syncs before reboot");

    OSKernel rebooted(kernel.diskImage());
    ctx.check(rebooted.boot().ok(), "rebooted kernel mounts disk image");
    std::vector<long long> note;
    ctx.check(rebooted.fs().readFile("/home/note", note).ok(),
              "note survives reboot");
    ctx.check(note == std::vector<long long>({84, 82, 73, 84}),
              "note payload survives reboot");

    const StatusResult exec = rebooted.sysExec(1, "/bin/app");
    const Process* proc = rebooted.process(1);
    ctx.check(exec.ok() && exec.payload == header.entry_virtual_pc,
              "installed executable execs after reboot");
    ctx.check(proc != nullptr && proc->exec_header.entry_virtual_pc == 7,
              "exec metadata survives disk image reboot");
    ctx.check(proc != nullptr && proc->memory == app,
              "exec payload survives disk image reboot");

    ctx.check(rebooted.fs().createFile("/home/after", InodeKind::File).ok(),
              "post-reboot file creates");
    ctx.check(rebooted.fs().writeFile("/home/after", {1, 2, 3}).ok(),
              "post-reboot file writes");
    ctx.check(rebooted.shutdownSync().ok(), "post-reboot image syncs");
    OSKernel rebooted_again(rebooted.diskImage());
    ctx.check(rebooted_again.boot().ok(), "second reboot mounts disk image");
    std::vector<long long> after;
    ctx.check(rebooted_again.fs().readFile("/home/after", after).ok(),
              "post-reboot write survives second reboot");
    ctx.check(after == std::vector<long long>({1, 2, 3}),
              "post-reboot payload persists");
}

void signedPackageReleaseImage(TestContext& ctx) {
    sandbox::vm::ExecutableImageHeader header;
    header.entry_virtual_pc = 3;
    header.text_pages = 1;
    header.data_pages = 1;
    header.stack_words = 64;
    const std::vector<long long> app = {700, 701, 702, 703};
    const std::string secret = "production-signing-key";
    const SignedExecutableMetadata metadata =
        signExecutableMetadata(app, header, "trit-release", secret);
    ctx.check(verifySignedExecutableMetadata(app, header, metadata, secret),
              "signed executable metadata verifies");

    std::vector<long long> tampered = app;
    tampered[0] = 999;
    ctx.check(!verifySignedExecutableMetadata(tampered, header, metadata, secret),
              "signed executable metadata rejects tampered payload");

    PackageImageBuilder package_builder("base", 42);
    ctx.check(package_builder.addFile("/etc/motd", {84, 82, 73, 84}).ok(),
              "package records configuration file");
    ctx.check(package_builder.addExecutable("/bin/app", app, header, metadata).ok(),
              "package records signed executable");
    PackageImage package = package_builder.build();
    const std::vector<long long> manifest = encodePackageManifest(package);
    ctx.check(manifest.size() > 4 &&
                  manifest[0] == PACKAGE_MAGIC &&
                  manifest[1] == PACKAGE_FORMAT_VERSION &&
                  manifest[2] == 42,
              "package manifest encodes format and update epoch");

    OSKernel kernel(192);
    ctx.check(kernel.boot().ok(), "package install kernel boots");
    ctx.check(installPackage(kernel, package).ok(), "signed package installs");
    std::vector<long long> stored_manifest;
    ctx.check(kernel.fs().readFile("/var/packages/base.manifest", stored_manifest).ok(),
              "package manifest persists in root filesystem");
    ctx.check(stored_manifest == manifest,
              "persisted package manifest matches encoded format");
    std::vector<long long> signature_sidecar;
    ctx.check(kernel.fs().readFile("/bin/app.sig", signature_sidecar).ok(),
              "signed executable metadata sidecar persists");
    ctx.check(kernel.sysExec(1, "/bin/app").ok(),
              "installed package executable execs");
    const Process* proc = kernel.process(1);
    ctx.check(proc != nullptr && proc->memory == app,
              "installed package executable payload loads by exec");
    ctx.check(kernel.checkFilesystemConsistency().ok(),
              "package install leaves filesystem consistent");

    PackageImage tampered_package = package;
    tampered_package.entries[1].words[0] = 12345;
    OSKernel rejected(192);
    ctx.check(rejected.boot().ok(), "tampered package test kernel boots");
    ctx.equal(installPackage(rejected, tampered_package).detail, ERR_SIGNATURE,
              "package installer rejects executable payload/signature mismatch");

    ReleaseImageBuilder release("trit-production", 42, 224);
    ctx.check(release.addPackage(package).ok(),
              "release image builder accepts signed package");
    const std::vector<long long> release_image = release.image();
    ctx.check(!release_image.empty() && release_image[0] == NATIVE_VFS_MAGIC,
              "release image uses native VFS superblock");
    ctx.check(containsWord(release_image, RELEASE_IMAGE_MAGIC),
              "release image embeds release metadata");
    ctx.check(containsWord(release_image, PACKAGE_MAGIC),
              "release image embeds package manifest metadata");
}

} // namespace

int main() {
    sandbox::LongTriple::initPowTable();
    const std::vector<TestCase> cases = {
        {"vfs.recovery.journal_phase_matrix", "vfs.persistence_contract",
         journalPhaseReplayMatrix},
        {"vfs.recovery.corrupt_superblock_boot", "vfs.persistence_contract",
         bootRecoveryCorruptSuperblock},
        {"vfs.executable.disk_image_reboot_exec", "vfs.persistence_contract",
         diskImageExecutableReboot},
        {"distribution.release.signed_package_image", "distribution.image_contract",
         signedPackageReleaseImage},
    };
    return tests_next::runCases("next_vfs_recovery", cases);
}
