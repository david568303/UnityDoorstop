/*
 * Native IL2CPP discovery by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#ifdef DOORSTOP_NATIVE_API
#include "native_api_internal.h"
#include "native_api_schema.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    const IMAGE_EXPORT_DIRECTORY *directory;
    const DWORD *names;
    const WORD *ordinals;
    const DWORD *functions;
    IMAGE_DATA_DIRECTORY range;
    const NativeImage *image;
} NativeExports;

typedef struct {
    DWORD rva;
    BOOL indirect;
} NativeCallTarget;

static const char *image_string(const NativeImage *image, DWORD rva) {
    const IMAGE_SECTION_HEADER *section = native_section(image, rva, 1);
    if (!section) return NULL;
    DWORD size = max(section->Misc.VirtualSize, section->SizeOfRawData);
    size_t maximum = min(256, min(image->size - rva, size - (rva - section->VirtualAddress)));
    const char *string = native_range(image, rva, maximum);
    return string && *string && memchr(string, '\0', maximum) ? string : NULL;
}

static BOOL export_table(const NativeImage *image, NativeExports *exports) {
    exports->image = image;
    exports->range = image->nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (exports->range.Size < sizeof(*exports->directory)) return FALSE;
    exports->directory = native_range(image, exports->range.VirtualAddress, exports->range.Size);
    if (!exports->directory) return FALSE;
    const IMAGE_EXPORT_DIRECTORY *dir = exports->directory;
    if (dir->NumberOfNames < 20 || dir->NumberOfNames > dir->NumberOfFunctions) return FALSE;
    exports->names = native_range(image, dir->AddressOfNames,
                                  (size_t)dir->NumberOfNames * sizeof(*exports->names));
    exports->ordinals = native_range(image, dir->AddressOfNameOrdinals,
                                     (size_t)dir->NumberOfNames * sizeof(*exports->ordinals));
    exports->functions = native_range(image, dir->AddressOfFunctions,
                                      (size_t)dir->NumberOfFunctions * sizeof(*exports->functions));
    return exports->names && exports->ordinals && exports->functions;
}

static DWORD lookup_export(const NativeExports *exports, const char *name, const char **actual) {
    if (!name) return 0;
    DWORD first = 0, end = exports->directory->NumberOfNames;
    while (first < end) {
        DWORD index = first + (end - first) / 2;
        const char *candidate = image_string(exports->image, exports->names[index]);
        if (!candidate) return 0;
        int order = strcmp(name, candidate);
        if (order > 0) {
            first = index + 1;
        } else if (order < 0) {
            end = index;
        } else {
            WORD ordinal = exports->ordinals[index];
            if (ordinal >= exports->directory->NumberOfFunctions) return 0;
            DWORD rva = exports->functions[ordinal];
            if (rva >= exports->range.VirtualAddress &&
                rva - exports->range.VirtualAddress < exports->range.Size) return 0;
            const IMAGE_SECTION_HEADER *section = native_section(exports->image, rva, 1);
            if (!section || !(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) return 0;
            if (actual) *actual = candidate;
            return rva;
        }
    }
    return 0;
}

static BOOL valid_alias(const char *name) {
    for (; *name; ++name)
        if (!(*name >= 'a' && *name <= 'z') && !(*name >= 'A' && *name <= 'Z') &&
            !(*name >= '0' && *name <= '9') && *name != '_') return FALSE;
    return TRUE;
}

static void find_binder_candidates(const NativeImage *image, const NativeExports *exports,
                                   DWORD *references) {
    for (WORD index = 0; index < image->nt->FileHeader.NumberOfSections; ++index) {
        const IMAGE_SECTION_HEADER *section = &image->sections[index];
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        DWORD begin = section->VirtualAddress;
        DWORD size = min(section->Misc.VirtualSize, section->SizeOfRawData);
        const BYTE *code = native_range(image, begin, size);
        if (!code || size < 7) continue;
        for (DWORD offset = 0; offset <= size - 7; ++offset) {
            /* LEA RDX,[RIP+disp32] loads the second Win64 argument. These
               candidate byte matches are verified at instruction boundaries. */
            if (code[offset] != 0x48 || code[offset + 1] != 0x8d || code[offset + 2] != 0x15)
                continue;
            LONG displacement;
            memcpy(&displacement, code + offset + 3, sizeof(displacement));
            DWORD load = begin + offset;
            int64_t string = (int64_t)load + 7 + displacement;
            if (string <= 0 || string >= image->size ||
                !lookup_export(exports, image_string(image, (DWORD)string), NULL)) continue;
            DWORD function = native_function_index(image, load);
            if (function) ++references[image->roots[function - 1] - 1];
        }
    }
}

static BOOL register_is(const cs_x86_op *operand, x86_reg reg) {
    return operand->type == X86_OP_REG && operand->reg == reg;
}

static BOOL prove_resolver_import(NativeDecoder *decoder, const NativeImage *image,
                                 NativeCallTarget target) {
    if (target.indirect) return native_has_import(image, target.rva, "GetProcAddress");
    struct { DWORD rva; unsigned depth; } pending[512] = {{ target.rva, 0 }};
    size_t length = 1;
    for (size_t i = 0; i < length; ++i) {
        NativeBody body = {0};
        if (!native_disassemble(decoder, image, pending[i].rva, &body)) {
            native_body_close(&body);
            continue;
        }
        for (size_t j = 0; j < body.count; ++j) {
            const NativeInstruction *instruction = &body.instructions[j];
            if (instruction->id != X86_INS_CALL && instruction->id != X86_INS_JMP) continue;
            DWORD imported = native_rip_target(instruction, 0);
            if (imported && native_has_import(image, imported, "GetProcAddress")) {
                native_body_close(&body);
                return TRUE;
            }
            DWORD called = native_direct_target(instruction);
            if (!called || called >= image->size || length == ARRAYSIZE(pending) ||
                pending[i].depth >= 6) continue;
            DWORD index = native_function_index(image, called);
            if (index) called = image->unwind[image->roots[index - 1] - 1].BeginAddress;
            size_t k = 0;
            while (k < length && pending[k].rva != called) ++k;
            if (k < length) continue;
            pending[length].rva = called;
            pending[length++].depth = pending[i].depth + 1;
        }
        native_body_close(&body);
    }
    return FALSE;
}

static BOOL extract_bindings(NativeDecoder *decoder, const NativeImage *unity,
                             const NativeExports *exports, const NativeBody *body,
                             NativeApiMap *map) {
    NativeCallTarget shared = {0};
    DWORD slots[NATIVE_API_CAPACITY];
    for (size_t i = 0; i < body->count; ++i) {
        const NativeInstruction *lea = &body->instructions[i];
        if (lea->id != X86_INS_LEA || lea->operand_count != 2 ||
            !register_is(&lea->operands[0], X86_REG_RDX)) continue;
        DWORD target = native_rip_target(lea, 1);
        const char *alias = NULL;
        DWORD export_rva = lookup_export(exports, image_string(unity, target), &alias);
        if (!export_rva) continue;
        if (!valid_alias(alias) || map->count == ARRAYSIZE(map->entries)) return FALSE;

        size_t call_index = i + 1;
        for (; call_index < min(i + 9, body->count); ++call_index) {
            const NativeInstruction *candidate = &body->instructions[call_index];
            if (candidate->id == X86_INS_CALL) break;
            if (candidate->id == X86_INS_RET || candidate->jump || candidate->writes_rdx)
                return FALSE;
        }
        if (call_index >= i + 9 || call_index + 2 >= body->count) return FALSE;
        const NativeInstruction *call = &body->instructions[call_index];
        const NativeInstruction *store = &body->instructions[call_index + 1];
        const NativeInstruction *test = &body->instructions[call_index + 2];
        if (call->id != X86_INS_CALL || store->id != X86_INS_MOV ||
            store->operand_count != 2 || store->operands[0].size != sizeof(void *) ||
            !register_is(&store->operands[1], X86_REG_RAX) || test->id != X86_INS_TEST ||
            test->operand_count != 2 || !register_is(&test->operands[0], X86_REG_RAX) ||
            !register_is(&test->operands[1], X86_REG_RAX)) return FALSE;
        DWORD slot = native_rip_target(store, 0);
        if (!native_writable_slot(unity, slot)) return FALSE;
        NativeCallTarget resolver = {native_direct_target(call), FALSE};
        if (!resolver.rva) {
            resolver.rva = native_rip_target(call, 0);
            resolver.indirect = TRUE;
        }
        if (!resolver.rva) return FALSE;
        if (!map->count) shared = resolver;
        if (resolver.rva != shared.rva || resolver.indirect != shared.indirect) return FALSE;
        for (size_t previous = 0; previous < map->count; ++previous)
            if (slots[previous] == slot || map->entries[previous].actual == alias) return FALSE;
        FARPROC address = GetProcAddress((HMODULE)exports->image->base, alias);
        if (address != (FARPROC)(exports->image->base + export_rva)) return FALSE;
        slots[map->count] = slot;
        map->entries[map->count++] = (NativeApiBinding){NULL, alias, export_rva, address};
    }
    return map->count >= 20 && prove_resolver_import(decoder, unity, shared);
}

static BOOL select_declaration_profile(NativeApiMap *map) {
    size_t without_profiler = 0;
    for (size_t i = 0; i < ARRAYSIZE(native_api_schema); ++i)
        if (!native_api_schema[i].profiler_only) ++without_profiler;
    BOOL with_profiler = map->count == ARRAYSIZE(native_api_schema);
    if (!with_profiler && map->count != without_profiler) return FALSE;
    size_t index = 0;
    for (size_t i = 0; i < ARRAYSIZE(native_api_schema); ++i)
        if (with_profiler || !native_api_schema[i].profiler_only)
            map->entries[index++].canonical = native_api_schema[i].canonical;
    return TRUE;
}

BOOL doorstop_discover_native_apis(HMODULE unity_module, HMODULE game_module,
                                  LPCWSTR decoder_path, NativeApiMap *result,
                                  const char **failure) {
    NativeImage unity = {0}, game = {0};
    NativeDecoder decoder = {0};
    NativeExports exports = {0};
    DWORD *references = NULL;
    BOOL success = FALSE, found = FALSE;
    result->count = 0;
    *failure = "unrecognized loaded x64 PE image or unwind directory";
    if (!native_image_open(&unity, unity_module) || !native_image_open(&game, game_module)) goto done;
    *failure = "could not load the packaged x64 Capstone 5 decoder";
    if (!native_decoder_open(&decoder, decoder_path)) goto done;
    *failure = "could not read the native runtime export table";
    if (!export_table(&game, &exports)) goto done;
    *failure = "could not allocate binder scan state";
    references = calloc(unity.unwind_count, sizeof(*references));
    if (!references) goto done;
    find_binder_candidates(&unity, &exports, references);
    *failure = "could not establish a unique native IL2CPP export binder";
    for (size_t i = 0; i < unity.unwind_count; ++i) {
        if (references[i] < 20) continue;
        if (found) goto done;
        NativeBody body = {0};
        DWORD root = unity.unwind[i].BeginAddress;
        found = native_disassemble(&decoder, &unity, root, &body) &&
                extract_bindings(&decoder, &unity, &exports, &body, result);
        native_body_close(&body);
        if (!found) goto done;
    }
    if (!found) goto done;
    *failure = "native binder does not match a supported IL2CPP API declaration profile";
    if (!select_declaration_profile(result)) goto done;
    if (!native_validate_semantics(&decoder, &game, result, failure)) goto done;
    result->timestamp = game.nt->FileHeader.TimeDateStamp;
    result->image_size = game.size;
    *failure = NULL;
    success = TRUE;
done:
    if (!success) result->count = 0;
    free(references);
    native_decoder_close(&decoder);
    native_image_close(&game);
    native_image_close(&unity);
    return success;
}
#endif
