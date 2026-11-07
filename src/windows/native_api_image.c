/*
 * Native IL2CPP discovery by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#ifdef DOORSTOP_NATIVE_API
#include "native_api_internal.h"

#include <stdlib.h>
#include <string.h>

const IMAGE_SECTION_HEADER *native_section(const NativeImage *image, DWORD rva,
                                           size_t length) {
    if (rva >= image->size || length > image->size - rva) return NULL;
    for (WORD i = 0; i < image->nt->FileHeader.NumberOfSections; ++i) {
        const IMAGE_SECTION_HEADER *section = &image->sections[i];
        DWORD size = max(section->Misc.VirtualSize, section->SizeOfRawData);
        if (rva >= section->VirtualAddress) {
            DWORD offset = rva - section->VirtualAddress;
            if (offset < size && length <= size - offset) return section;
        }
    }
    return NULL;
}

const void *native_range(const NativeImage *image, DWORD rva, size_t length) {
    const IMAGE_SECTION_HEADER *section = native_section(image, rva, length);
    if (!section || !(section->Characteristics & IMAGE_SCN_MEM_READ) ||
        (section->Characteristics & IMAGE_SCN_MEM_DISCARDABLE)) return NULL;
    return image->base + rva;
}

BOOL native_writable_slot(const NativeImage *image, DWORD rva) {
    const IMAGE_SECTION_HEADER *section = native_section(image, rva, sizeof(void *));
    return rva && rva % sizeof(void *) == 0 && section &&
           (section->Characteristics & IMAGE_SCN_MEM_WRITE);
}

/* Look up the .pdata interval, returning its index plus one. */
DWORD native_function_index(const NativeImage *image, DWORD rva) {
    size_t left = 0, right = image->unwind_count;
    while (left < right) {
        size_t middle = left + (right - left) / 2;
        if (image->unwind[middle].BeginAddress <= rva) left = middle + 1;
        else right = middle;
    }
    return left && rva < image->unwind[left - 1].EndAddress ? (DWORD)left : 0;
}

static DWORD function_root(NativeImage *image, DWORD index) {
    for (size_t depth = 0; depth < 32; ++depth) {
        DWORD cached = image->roots[index - 1];
        if (cached) return cached;
        const RUNTIME_FUNCTION *record = &image->unwind[index - 1];
        const BYTE *info = native_range(image, record->UnwindData, 4);
        if (!info) return 0;
        if (!(info[0] & (UNW_FLAG_CHAININFO << 3))) return index;
        DWORD tail = record->UnwindData + 4 + ((info[2] + 1) / 2) * 4;
        const RUNTIME_FUNCTION *parent = native_range(image, tail, sizeof(*parent));
        if (!parent) return 0;
        index = native_function_index(image, parent->BeginAddress);
        if (!index || image->unwind[index - 1].BeginAddress != parent->BeginAddress ||
            image->unwind[index - 1].EndAddress != parent->EndAddress) return 0;
    }
    return 0;
}

BOOL native_image_open(NativeImage *image, HMODULE module) {
    MEMORY_BASIC_INFORMATION region;
    if (!module || VirtualQuery(module, &region, sizeof(region)) != sizeof(region) ||
        region.AllocationBase != module || region.Type != MEM_IMAGE ||
        region.State != MEM_COMMIT || region.RegionSize < sizeof(IMAGE_DOS_HEADER))
        return FALSE;
    image->base = (BYTE *)module;
    const IMAGE_DOS_HEADER *dos = (const void *)image->base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        (size_t)dos->e_lfanew > region.RegionSize ||
        sizeof(*image->nt) > region.RegionSize - dos->e_lfanew) return FALSE;
    image->nt = (const void *)(image->base + dos->e_lfanew);
    const IMAGE_NT_HEADERS64 *nt = image->nt;
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt->FileHeader.SizeOfOptionalHeader != sizeof(nt->OptionalHeader) ||
        nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        nt->OptionalHeader.NumberOfRvaAndSizes < IMAGE_NUMBEROF_DIRECTORY_ENTRIES ||
        !nt->OptionalHeader.SizeOfImage || !nt->FileHeader.NumberOfSections ||
        nt->FileHeader.NumberOfSections > 96) return FALSE;
    image->size = nt->OptionalHeader.SizeOfImage;
    image->sections = IMAGE_FIRST_SECTION(nt);
    size_t section_offset = (const BYTE *)image->sections - image->base;
    if (nt->FileHeader.NumberOfSections * sizeof(*image->sections) >
        region.RegionSize - section_offset) return FALSE;
    IMAGE_DATA_DIRECTORY directory =
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!directory.VirtualAddress || directory.Size % sizeof(*image->unwind)) return FALSE;
    image->unwind = native_range(image, directory.VirtualAddress, directory.Size);
    image->unwind_count = directory.Size / sizeof(*image->unwind);
    if (!image->unwind || !image->unwind_count) return FALSE;

    for (size_t i = 0; i < image->unwind_count; ++i) {
        const RUNTIME_FUNCTION *record = &image->unwind[i];
        if (!record->BeginAddress || record->EndAddress <= record->BeginAddress ||
            record->EndAddress > image->size ||
            (i && record->BeginAddress < image->unwind[i - 1].EndAddress)) return FALSE;
    }
    image->roots = calloc(image->unwind_count, sizeof(*image->roots));
    image->first = calloc(image->unwind_count, sizeof(*image->first));
    image->next = calloc(image->unwind_count, sizeof(*image->next));
    DWORD *last = calloc(image->unwind_count, sizeof(*last));
    if (!image->roots || !image->first || !image->next || !last) {
        free(last);
        return FALSE;
    }
    for (DWORD i = 1; i <= image->unwind_count; ++i) {
        DWORD root = function_root(image, i);
        if (!root) {
            free(last);
            return FALSE;
        }
        image->roots[i - 1] = root;
        if (last[root - 1]) image->next[last[root - 1] - 1] = i;
        else image->first[root - 1] = i;
        last[root - 1] = i;
    }
    free(last);
    return TRUE;
}

void native_image_close(NativeImage *image) {
    free(image->roots);
    free(image->first);
    free(image->next);
}

BOOL native_has_import(const NativeImage *image, DWORD iat_rva, const char *name) {
    IMAGE_DATA_DIRECTORY dir = image->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const IMAGE_IMPORT_DESCRIPTOR *entries = native_range(image, dir.VirtualAddress, dir.Size);
    if (!entries) return FALSE;
    for (size_t i = 0; i < dir.Size / sizeof(*entries) && entries[i].Name; ++i) {
        const IMAGE_IMPORT_DESCRIPTOR *entry = &entries[i];
        if (!entry->OriginalFirstThunk || entry->OriginalFirstThunk >= image->size ||
            !entry->FirstThunk || iat_rva < entry->FirstThunk) continue;
        DWORD offset = iat_rva - entry->FirstThunk;
        if (offset % sizeof(IMAGE_THUNK_DATA64) || offset >= image->size - entry->OriginalFirstThunk)
            continue;
        const IMAGE_THUNK_DATA64 *names = native_range(image, entry->OriginalFirstThunk,
                                                       offset + sizeof(*names));
        if (!names) continue;
        size_t index = 0;
        while (index < offset / sizeof(*names) && names[index].u1.AddressOfData) ++index;
        ULONGLONG value = names[index].u1.AddressOfData;
        if (index != offset / sizeof(*names) || !value || IMAGE_SNAP_BY_ORDINAL64(value) ||
            value > MAXDWORD - sizeof(WORD)) continue;
        size_t length = strlen(name) + 1;
        const char *import_name = native_range(image, (DWORD)value + sizeof(WORD), length);
        if (import_name && !memcmp(import_name, name, length)) return TRUE;
    }
    return FALSE;
}

BOOL native_decoder_open(NativeDecoder *decoder, LPCWSTR path) {
    decoder->library = LoadLibraryExW(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR |
                                                  LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!decoder->library) return FALSE;
#define LOAD_DECODER(member, symbol) \
    do { \
        FARPROC address = GetProcAddress(decoder->library, symbol); \
        if (!address) return FALSE; \
        memcpy(&decoder->member, &address, sizeof(address)); \
    } while (0)
    LOAD_DECODER(open, "cs_open");
    LOAD_DECODER(close, "cs_close");
    LOAD_DECODER(option, "cs_option");
    LOAD_DECODER(alloc, "cs_malloc");
    LOAD_DECODER(free, "cs_free");
    LOAD_DECODER(disasm_iter, "cs_disasm_iter");
    LOAD_DECODER(version, "cs_version");
#undef LOAD_DECODER
    int major = 0, minor = 0;
    decoder->version(&major, &minor);
    if (major != CS_API_MAJOR || minor != CS_API_MINOR ||
        decoder->open(CS_ARCH_X86, CS_MODE_64, &decoder->handle) != CS_ERR_OK ||
        decoder->option(decoder->handle, CS_OPT_DETAIL, CS_OPT_ON) != CS_ERR_OK) return FALSE;
    decoder->scratch = decoder->alloc(decoder->handle);
    return decoder->scratch != NULL;
}

void native_decoder_close(NativeDecoder *decoder) {
    if (decoder->scratch) decoder->free(decoder->scratch, 1);
    if (decoder->handle) decoder->close(&decoder->handle);
    if (decoder->library) FreeLibrary(decoder->library);
}

void native_body_close(NativeBody *body) {
    free(body->instructions);
    *body = (NativeBody){0};
}

static BOOL append_range(NativeDecoder *decoder, const NativeImage *image,
                         NativeBody *body, DWORD begin, DWORD end, BOOL leaf) {
    size_t remaining = end - begin;
    const uint8_t *code = native_range(image, begin, remaining);
    if (!code) return FALSE;
    uint64_t address = begin;
    cs_insn *decoded = decoder->scratch;
    while (decoder->disasm_iter(decoder->handle, &code, &remaining, &address, decoded)) {
        if (decoded->id == X86_INS_NOP || decoded->id == X86_INS_ENDBR64 ||
            decoded->id == X86_INS_INT3) {
            if (leaf && decoded->id == X86_INS_INT3) break;
            continue;
        }
        if (body->count == body->capacity) {
            size_t capacity = body->capacity ? body->capacity * 2 : 32;
            /* Reject implausibly large candidate functions without using unbounded memory. */
            if (capacity > 65536) return FALSE;
            NativeInstruction *instructions = realloc(body->instructions,
                                                       capacity * sizeof(*instructions));
            if (!instructions) return FALSE;
            body->instructions = instructions;
            body->capacity = capacity;
        }
        NativeInstruction *ins = &body->instructions[body->count++];
        *ins = (NativeInstruction){0};
        ins->id = decoded->id;
        ins->rva = (DWORD)decoded->address;
        ins->size = (BYTE)decoded->size;
        ins->operand_count = min(decoded->detail->x86.op_count, ARRAYSIZE(ins->operands));
        memcpy(ins->operands, decoded->detail->x86.operands,
               ins->operand_count * sizeof(*ins->operands));
        for (uint8_t i = 0; i < decoded->detail->groups_count; ++i)
            ins->jump |= decoded->detail->groups[i] == CS_GRP_JUMP;
        /* RDX writes can be explicit operands or implicit side effects. */
        for (uint8_t i = 0; i < decoded->detail->regs_write_count; ++i)
            ins->writes_rdx |= decoded->detail->regs_write[i] == X86_REG_RDX ||
                               decoded->detail->regs_write[i] == X86_REG_EDX;
        for (uint8_t i = 0; i < decoded->detail->x86.op_count; ++i) {
            const cs_x86_op *op = &decoded->detail->x86.operands[i];
            if (op->type == X86_OP_REG && (op->access & CS_AC_WRITE))
                ins->writes_rdx |= op->reg == X86_REG_RDX || op->reg == X86_REG_EDX ||
                                   op->reg == X86_REG_DX || op->reg == X86_REG_DL ||
                                   op->reg == X86_REG_DH;
        }
        if (leaf && (ins->id == X86_INS_RET || ins->id == X86_INS_JMP)) break;
    }
    return TRUE;
}

BOOL native_disassemble(NativeDecoder *decoder, const NativeImage *image, DWORD rva,
                        NativeBody *body) {
    DWORD index = native_function_index(image, rva);
    if (index) {
        index = image->roots[index - 1];
        body->rva = image->unwind[index - 1].BeginAddress;
        index = image->first[index - 1];
        for (; index; index = image->next[index - 1]) {
            const RUNTIME_FUNCTION *entry = &image->unwind[index - 1];
            if (!append_range(decoder, image, body, entry->BeginAddress, entry->EndAddress, FALSE))
                return FALSE;
        }
        return body->count > 0;
    }
    const IMAGE_SECTION_HEADER *section = native_section(image, rva, 1);
    if (!section || !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) return FALSE;
    DWORD section_size = max(section->Misc.VirtualSize, section->SizeOfRawData);
    DWORD remaining = section_size - (rva - section->VirtualAddress);
    body->rva = rva;
    return append_range(decoder, image, body, rva, rva + min(remaining, 256), TRUE) &&
           body->count > 0;
}

DWORD native_rip_target(const NativeInstruction *instruction, size_t operand) {
    if (operand >= instruction->operand_count) return 0;
    const cs_x86_op *op = &instruction->operands[operand];
    if (op->type != X86_OP_MEM || op->mem.base != X86_REG_RIP || op->mem.index) return 0;
    int64_t target = (int64_t)instruction->rva + instruction->size + op->mem.disp;
    return target > 0 && target <= MAXDWORD ? (DWORD)target : 0;
}

DWORD native_direct_target(const NativeInstruction *instruction) {
    if ((instruction->id != X86_INS_CALL && instruction->id != X86_INS_JMP) ||
        !instruction->operand_count || instruction->operands[0].type != X86_OP_IMM) return 0;
    int64_t target = instruction->operands[0].imm;
    return target > 0 && target <= MAXDWORD ? (DWORD)target : 0;
}
#endif
