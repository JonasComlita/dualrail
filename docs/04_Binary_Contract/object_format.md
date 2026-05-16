# Trit-Object (TOB) File Format

| Status | Last Updated | Related Code |
| :--- | :--- | :--- |
| 🛠️ **Draft** | 2026-05-15 | `ternary_asm.h` |

---

## 🏗️ File Structure
A TOB file is a structured binary container used for separate compilation and linking. It consists of four primary segments.

| Segment | Purpose |
| :--- | :--- |
| **Header** | Magic number, version, and segment offsets. |
| **Section Data** | The raw `.text` (IMEM) and `.data` (DMEM) blobs. |
| **Symbol Table** | Exported labels and their internal offsets. |
| **Relocation Table** | List of addresses requiring linker patching. |

---

## 📜 The Header
The header is exactly 27 trits long (one word).

| Trit Range | Field | Description |
| :--- | :--- | :--- |
| **[3:0]** | **Magic** | Fixed pattern (e.g., `TRIT` in ternary). |
| **[7:4]** | **Version** | ABI Versioning. |
| **[16:8]** | **TextSize** | Number of instructions in the `.text` segment. |
| **[26:17]** | **DataSize** | Number of words in the `.data` segment. |

---

## 🏷️ Symbol Table (SYMTAB)
Each entry in the symbol table allows the linker to resolve external references.

*   **Symbol Name**: UTF-8 string (terminated by a null-trit).
*   **Section**: `0` for Text, `1` for Data.
*   **Offset**: The section-relative address (50-trit value).
*   **Visibility**: `Global` (exported) or `Local` (internal).

---

## 🔗 Relocation Table (RELOC)
When a compiler emits a `CALL` to an external function, it does not know the final address. The Relocation Table marks these instructions.

*   **Reloc Offset**: The address within the `.text` segment to be patched.
*   **Symbol Index**: The index into the Symbol Table for the target name.
*   **Type**:
    *   **TYPE_ABS**: Patch with the 50-trit absolute address.
    *   **TYPE_PCREL**: Patch with the 19-trit relative offset.

---

## 🚀 The Linking Process
1.  **Concatenation**: The linker merges all `.text` and `.data` segments into a single contiguous pool.
2.  **Resolution**: The linker resolves symbol offsets based on the new merged addresses.
3.  **Patching**: The linker iterates through the Relocation Tables and writes the final addresses into the instruction words.
4.  **Emission**: The final, fully-resolved image is emitted as a bootable `.t3` file.
