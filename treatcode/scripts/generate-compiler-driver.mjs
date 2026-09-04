import fs from "node:fs";
import path from "node:path";
import { spawnSync } from "node:child_process";
import { fileURLToPath } from "node:url";

const appRoot = path.resolve(path.dirname(fileURLToPath(import.meta.url)), "..");
const repositoryRoot = path.resolve(appRoot, "..");
const outputPath = path.join(appRoot, "compiler_driver.txe");
const temporaryRoot = path.join(repositoryRoot, "build", "treatcode-native-driver");
const sourceFiles = [
  "ulib_mini.trit",
  "tcl_token.trit",
  "tcl_lexer.trit",
  "tcl_ast.trit",
  "tcl_type.trit",
  "tcl_parser.trit",
  "tcl_infer.trit",
  "tcl_ir.trit",
  "tcl_asm.trit",
  "tcl_backend.trit",
  "tcl_frontend.trit",
  path.join("treatcode", "compiler_driver.trit"),
].map((file) => path.join(repositoryRoot, file));

// TritFileHeader is naturally aligned by the C++ compiler: the uint64_t
// required_features field starts at byte 24, making the complete header 72
// bytes even though its serialized fields occupy 68 bytes.
const TXE4_HEADER_BYTES = 72;

function compilerCandidates() {
  const names = process.platform === "win32" ? ["tritc.exe", "tritc"] : ["tritc", "tritc.exe"];
  return names.map((name) => path.join(repositoryRoot, "build", name));
}

function findCompiler() {
  return compilerCandidates().find((candidate) => fs.existsSync(candidate));
}

function validateImage(filePath) {
  const image = fs.readFileSync(filePath);
  if (image.length < TXE4_HEADER_BYTES) {
    throw new Error(`${filePath} is too small to contain a TXE4 header`);
  }

  const magic = image.toString("ascii", 0, 4);
  const version = image.readUInt32LE(4);
  const endianness = image.readUInt32LE(8);
  const headerSize = image.readUInt32LE(12);
  const isaVersion = image.readUInt32LE(16);
  const requiredFeatures = image.readBigUInt64LE(24);
  const instructionCount = image.readUInt32LE(32);
  const dataCount = image.readUInt32LE(36);
  const abiVersion = image.readUInt32LE(48);
  const syscallAbiVersion = image.readUInt32LE(52);
  const scalarWordTrits = image.readUInt32LE(56);
  const basePageWords = image.readUInt32LE(60);

  if (
    magic !== "TXE4" ||
    version !== 2 ||
    endianness !== 0x12345678 ||
    headerSize !== TXE4_HEADER_BYTES ||
    isaVersion !== 2 ||
    syscallAbiVersion !== 2 ||
    scalarWordTrits !== 40 ||
    basePageWords !== 729
  ) {
    throw new Error(
      `${filePath} is not a current TXE4/ISA v2 image (magic=${magic}, version=${version}, endian=${endianness}, header=${headerSize}, isa=${isaVersion}, syscallAbi=${syscallAbiVersion}, scalar=${scalarWordTrits}, basePage=${basePageWords})`,
    );
  }
  if ((requiredFeatures & 1n) === 0n || instructionCount === 0 || abiVersion !== 2) {
    throw new Error(
      `${filePath} has an invalid native driver header (features=${requiredFeatures}, instructions=${instructionCount}, abi=${abiVersion})`,
    );
  }

  return { bytes: image.length, instructionCount, dataCount, abiVersion };
}

function run() {
  const checkOnly = process.argv.includes("--check");
  if (checkOnly) {
    const details = validateImage(outputPath);
    console.log(
      `Native compiler driver is valid: TXE4/ISA v2, ${details.instructionCount} instruction words, ${details.dataCount} data words`,
    );
    return;
  }

  for (const sourceFile of sourceFiles) {
    if (!fs.existsSync(sourceFile)) {
      throw new Error(`Compiler driver source dependency is missing: ${sourceFile}`);
    }
  }

  const compiler = findCompiler();
  if (!compiler) {
    const existing = validateImage(outputPath);
    console.warn(
      `Current compiler executable was not found; keeping the validated native driver (${existing.bytes} bytes).`,
    );
    return;
  }

  fs.mkdirSync(temporaryRoot, { recursive: true });
  const temporaryPath = path.join(
    temporaryRoot,
    `compiler_driver-${process.pid}-${Date.now()}.txe`,
  );

  try {
    const result = spawnSync(
      compiler,
      [...sourceFiles, "-O2", "-o", temporaryPath, "--no-ansi"],
      {
        cwd: repositoryRoot,
        stdio: "inherit",
        windowsHide: true,
      },
    );

    if (result.error) {
      throw new Error(`Failed to run ${compiler}: ${result.error.message}`);
    }
    if (result.status !== 0) {
      throw new Error(`${compiler} exited with code ${result.status ?? "unknown"}`);
    }

    const details = validateImage(temporaryPath);
    fs.copyFileSync(temporaryPath, outputPath);
    console.log(
      `Updated ${outputPath}: TXE4/ISA v2, ${details.instructionCount} instruction words, ${details.dataCount} data words`,
    );
  } finally {
    fs.rmSync(temporaryPath, { force: true });
  }
}

try {
  run();
} catch (error) {
  console.error(error instanceof Error ? error.message : error);
  process.exitCode = 1;
}
