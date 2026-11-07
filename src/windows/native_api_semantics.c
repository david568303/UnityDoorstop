/*
 * Native IL2CPP discovery by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#ifdef DOORSTOP_NATIVE_API
#include "native_api_internal.h"

#include <string.h>

#define POINTER_WIDTH 8
#define INFER_OFFSET (-1)

typedef struct {
    NativeDecoder *decoder;
    const NativeImage *image;
    const NativeApiMap *map;
    BOOL covered[NATIVE_API_CAPACITY];
    const char *failure;
} SemanticChecks;

static x86_reg reg(const cs_x86_op *op) {
    return op->type == X86_OP_REG ? op->reg : X86_REG_INVALID;
}

static BOOL immediate(const cs_x86_op *op, int64_t value) {
    return op->type == X86_OP_IMM && op->imm == value;
}

static BOOL memory(const cs_x86_op *op, x86_reg base, unsigned int width) {
    return op->type == X86_OP_MEM && op->mem.base == base && !op->mem.index &&
           (!width || op->size == width);
}

static BOOL body_follow(SemanticChecks *checks, DWORD rva, NativeBody *body, BOOL first_jump) {
    native_body_close(body);
    for (unsigned int depth = 0; depth < 12; ++depth) {
        if (!rva || rva >= checks->image->size ||
            !native_disassemble(checks->decoder, checks->image, rva, body)) break;
        const NativeInstruction *first = body->instructions;
        if (first->id != X86_INS_JMP || (!first_jump && body->count != 1) ||
            !native_direct_target(first)) return TRUE;
        rva = native_direct_target(first);
        native_body_close(body);
    }
    return FALSE;
}

static BOOL api_body(SemanticChecks *checks, const char *name, NativeBody *body, BOOL first_jump) {
    checks->failure = name;
    for (size_t i = 0; i < checks->map->count; ++i) {
        if (strcmp(checks->map->entries[i].canonical, name)) continue;
        if (!body_follow(checks, checks->map->entries[i].rva, body, first_jump)) break;
        checks->covered[i] = TRUE;
        return TRUE;
    }
    return FALSE;
}

static BOOL ends_ret(const NativeBody *body, size_t count) {
    return body->count == count && body->instructions[count - 1].id == X86_INS_RET;
}

static int member_offset(const NativeBody *body, unsigned int width, BOOL address) {
    if (!ends_ret(body, 2)) return INFER_OFFSET;
    const NativeInstruction *ins = body->instructions;
    if (address ? ins->id != X86_INS_LEA :
                  (ins->id != X86_INS_MOV && ins->id != X86_INS_MOVZX &&
                   ins->id != X86_INS_MOVSX && ins->id != X86_INS_MOVSXD)) return INFER_OFFSET;
    x86_reg result = reg(&ins->operands[0]);
    const cs_x86_op *source = &ins->operands[1];
    if (ins->operand_count != 2 || (result != X86_REG_RAX && result != X86_REG_EAX) ||
        !memory(source, X86_REG_RCX, address ? 0 : width) ||
        source->mem.disp < 0 || source->mem.disp > 4096) return INFER_OFFSET;
    return (int)source->mem.disp;
}

static BOOL check_member(SemanticChecks *checks, const char *api, unsigned int width,
                         int expected, BOOL address, int *offset) {
    NativeBody body = {0};
    int found = api_body(checks, api, &body, FALSE) ? member_offset(&body, width, address) : INFER_OFFSET;
    native_body_close(&body);
    if (offset) *offset = found;
    return found != INFER_OFFSET && (expected == INFER_OFFSET || found == expected);
}

static BOOL check_bit(SemanticChecks *checks, const char *api, unsigned int width,
                      int expected, unsigned int shift, int *offset) {
    NativeBody body = {0};
    BOOL valid = FALSE;
    if (api_body(checks, api, &body, FALSE) && ends_ret(&body, 4)) {
        const NativeInstruction *load = &body.instructions[0];
        const NativeInstruction *move = &body.instructions[1];
        const NativeInstruction *mask = &body.instructions[2];
        const cs_x86_op *source = &load->operands[1];
        if ((load->id == X86_INS_MOV || load->id == X86_INS_MOVZX) &&
            load->operand_count == 2 && memory(source, X86_REG_RCX, width) &&
            source->mem.disp >= 0 && source->mem.disp <= 4096 &&
            (expected == INFER_OFFSET || source->mem.disp == expected) &&
            (move->id == X86_INS_SHR || move->id == X86_INS_SAR) &&
            immediate(&move->operands[1], shift) && mask->id == X86_INS_AND &&
            immediate(&mask->operands[1], 1)) {
            if (offset) *offset = (int)source->mem.disp;
            valid = TRUE;
        }
    }
    native_body_close(&body);
    return valid;
}

static DWORD global_cell(SemanticChecks *checks, const char *api, BOOL write) {
    NativeBody body = {0};
    DWORD cell = 0;
    if (api_body(checks, api, &body, FALSE) && ends_ret(&body, 2)) {
        const NativeInstruction *ins = body.instructions;
        size_t memory_operand = write ? 0 : 1;
        if (ins->id == X86_INS_MOV && ins->operand_count == 2 &&
            ins->operands[memory_operand].size == POINTER_WIDTH &&
            reg(&ins->operands[1 - memory_operand]) == (write ? X86_REG_RCX : X86_REG_RAX))
            cell = native_rip_target(ins, memory_operand);
    }
    native_body_close(&body);
    return native_writable_slot(checks->image, cell) ? cell : 0;
}

static BOOL reports_constant(const NativeBody *body, int64_t expected) {
    if (!ends_ret(body, 2)) return FALSE;
    const NativeInstruction *move = body->instructions;
    x86_reg output = reg(&move->operands[0]);
    return move->id == X86_INS_MOV && move->operand_count == 2 &&
           (output == X86_REG_RAX || output == X86_REG_EAX) &&
           immediate(&move->operands[1], expected);
}

static DWORD first_call(const NativeBody *body, size_t *count) {
    DWORD first = 0;
    if (count) *count = 0;
    for (size_t i = 0; i < body->count; ++i) {
        DWORD target = native_direct_target(&body->instructions[i]);
        if (!target) continue;
        if (!first) first = target;
        if (count) ++*count;
    }
    return first;
}

static BOOL method_flags_agree(SemanticChecks *checks, int token_offset) {
    NativeBody body = {0}, getter = {0};
    BOOL method_flags = FALSE, impl_flags = FALSE;
    if (api_body(checks, "il2cpp_method_get_flags", &body, FALSE)) {
        for (size_t i = 0; i < body.count; ++i) {
            DWORD target = native_direct_target(&body.instructions[i]);
            if (!target || !body_follow(checks, target, &getter, FALSE) ||
                getter.instructions[0].id != X86_INS_MOVZX) continue;
            int offset = member_offset(&getter, 2, FALSE);
            method_flags |= offset == token_offset + 4;
            impl_flags |= offset == token_offset + 6;
        }
    }
    native_body_close(&getter);
    native_body_close(&body);
    return method_flags && impl_flags;
}

static BOOL check_image_count(SemanticChecks *checks) {
    NativeBody body = {0}, getter = {0};
    BOOL valid = FALSE;
    if (api_body(checks, "il2cpp_image_get_class_count", &body, FALSE)) {
        if (body.count == 2) {
            valid = member_offset(&body, 4, FALSE) == 3 * POINTER_WIDTH;
        } else {
            size_t count;
            DWORD target = first_call(&body, &count);
            valid = count == 1 && body_follow(checks, target, &getter, FALSE) &&
                    getter.instructions[0].id == X86_INS_MOV &&
                    member_offset(&getter, 4, FALSE) == 3 * POINTER_WIDTH;
        }
    }
    native_body_close(&getter);
    native_body_close(&body);
    return valid;
}

static DWORD suppression_counter(SemanticChecks *checks, const char *name, unsigned int operation) {
    NativeBody body = {0};
    DWORD cell = 0;
    if (api_body(checks, name, &body, FALSE)) {
        for (size_t i = 0; i < body.count; ++i) {
            const NativeInstruction *ins = &body.instructions[i];
            if (ins->id != operation) continue;
            DWORD target = native_rip_target(ins, 0);
            if (!target) continue;
            if (cell && cell != target) {
                cell = 0;
                break;
            }
            cell = target;
        }
    }
    native_body_close(&body);
    const IMAGE_SECTION_HEADER *section = native_section(checks->image, cell, sizeof(DWORD));
    return section && (section->Characteristics & IMAGE_SCN_MEM_WRITE) ? cell : 0;
}

static BOOL has_operation(const NativeBody *body, unsigned int operation) {
    for (size_t i = 0; i < body->count; ++i)
        if (body->instructions[i].id == operation) return TRUE;
    return FALSE;
}

static BOOL check_heap_size(SemanticChecks *checks) {
    NativeBody heap = {0}, used = {0}, getter = {0};
    BOOL valid = api_body(checks, "il2cpp_gc_get_heap_size", &heap, FALSE) &&
                 api_body(checks, "il2cpp_gc_get_used_size", &used, FALSE) &&
                 body_follow(checks, first_call(&used, NULL), &getter, FALSE) &&
                 getter.rva == heap.rva && has_operation(&heap, X86_INS_SUB) &&
                 has_operation(&used, X86_INS_SUB);
    native_body_close(&getter);
    native_body_close(&used);
    native_body_close(&heap);
    return valid;
}

static BOOL check_import(SemanticChecks *checks, const char *api, const char *import) {
    NativeBody body = {0};
    BOOL found = FALSE;
    if (api_body(checks, api, &body, FALSE)) {
        for (size_t i = 0; i < body.count; ++i) {
            const NativeInstruction *ins = &body.instructions[i];
            if (ins->id != X86_INS_CALL && ins->id != X86_INS_JMP) continue;
            DWORD target = native_rip_target(ins, 0);
            if (target && native_has_import(checks->image, target, import)) {
                found = TRUE;
                break;
            }
        }
    }
    native_body_close(&body);
    return found;
}

static BOOL validate_getters(SemanticChecks *checks) {
    static const struct {
        const char *api;
        int offset;
        unsigned int width;
        BOOL address;
    } getters[] = {
        {"il2cpp_object_get_class", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_assembly_get_image", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_object_unbox", 2 * POINTER_WIDTH, POINTER_WIDTH, TRUE},
        {"il2cpp_array_length", 3 * POINTER_WIDTH, 4, FALSE},
        {"il2cpp_string_length", 2 * POINTER_WIDTH, 4, FALSE},
        {"il2cpp_string_chars", 2 * POINTER_WIDTH + 4, POINTER_WIDTH, TRUE},
        {"il2cpp_field_get_name", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_field_get_type", POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_field_get_parent", 2 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_field_get_offset", 3 * POINTER_WIDTH, 4, FALSE},
        {"il2cpp_property_get_parent", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_property_get_name", POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_property_get_get_method", 2 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_property_get_set_method", 3 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_class_get_name", 2 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_class_get_namespace", 3 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
        {"il2cpp_image_get_name", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_image_get_filename", 0, POINTER_WIDTH, FALSE},
        {"il2cpp_image_get_assembly", 2 * POINTER_WIDTH, POINTER_WIDTH, FALSE},
    };
    for (size_t i = 0; i < ARRAYSIZE(getters); ++i) {
        if (!check_member(checks, getters[i].api, getters[i].width, getters[i].offset,
                          getters[i].address, NULL)) return FALSE;
    }

    int name_offset, token_offset, params, attrs, kind, class_flags, generic_flags;
    if (!check_member(checks, "il2cpp_method_get_name", POINTER_WIDTH, INFER_OFFSET, FALSE, &name_offset) ||
        name_offset % POINTER_WIDTH || name_offset < 2 * POINTER_WIDTH || name_offset > 8 * POINTER_WIDTH ||
        !check_member(checks, "il2cpp_method_get_declaring_type", POINTER_WIDTH, name_offset + POINTER_WIDTH, FALSE, NULL) ||
        !check_member(checks, "il2cpp_method_get_class", POINTER_WIDTH, name_offset + POINTER_WIDTH, FALSE, NULL) ||
        !check_member(checks, "il2cpp_method_get_return_type", POINTER_WIDTH, name_offset + 2 * POINTER_WIDTH, FALSE, NULL) ||
        !check_member(checks, "il2cpp_method_get_token", 4, INFER_OFFSET, FALSE, &token_offset) ||
        !check_member(checks, "il2cpp_method_get_param_count", 1, INFER_OFFSET, FALSE, &params) ||
        params != token_offset + 4 + 2 + 2 + 2 || !method_flags_agree(checks, token_offset)) return FALSE;

    if (!check_member(checks, "il2cpp_type_get_attrs", 2, INFER_OFFSET, FALSE, &attrs) ||
        !check_member(checks, "il2cpp_type_get_type", 1, attrs + 2, FALSE, &kind) ||
        !check_bit(checks, "il2cpp_type_is_byref", 4, attrs, 29, NULL) ||
        !check_bit(checks, "il2cpp_type_is_static", 4, attrs, 4, NULL)) return FALSE;

    NativeBody body = {0};
    BOOL success = FALSE;
    if (!api_body(checks, "il2cpp_type_is_pointer_type", &body, FALSE) ||
        !ends_ret(&body, 3) || body.instructions[0].id != X86_INS_CMP ||
        body.instructions[1].id != X86_INS_SETE ||
        !memory(&body.instructions[0].operands[0], X86_REG_RCX, 1) ||
        body.instructions[0].operands[0].mem.disp != kind ||
        !immediate(&body.instructions[0].operands[1], 0x0f)) goto done;

    if (!check_member(checks, "il2cpp_class_get_flags", 4, INFER_OFFSET, FALSE, &class_flags) ||
        !check_bit(checks, "il2cpp_class_is_abstract", 4, class_flags, 7, NULL) ||
        !check_bit(checks, "il2cpp_class_is_generic", 1, INFER_OFFSET, 4, &generic_flags) ||
        !check_bit(checks, "il2cpp_class_is_enum", 1, generic_flags, 2, NULL) ||
        !check_member(checks, "il2cpp_class_get_rank", 1, INFER_OFFSET, FALSE, NULL) ||
        !check_member(checks, "il2cpp_class_get_static_field_data", POINTER_WIDTH, INFER_OFFSET, FALSE, NULL) ||
        !check_image_count(checks)) goto done;

    DWORD gc_slice = global_cell(checks, "il2cpp_gc_get_max_time_slice_ns", FALSE);
    if (!gc_slice || global_cell(checks, "il2cpp_gc_set_max_time_slice_ns", TRUE) != gc_slice) goto done;
    DWORD counter = suppression_counter(checks, "il2cpp_gc_disable", X86_INS_INC);
    if (!counter || suppression_counter(checks, "il2cpp_gc_enable", X86_INS_DEC) != counter) goto done;

    static const struct { const char *api; int value; } sizes[] = {
        {"il2cpp_object_header_size", 2 * POINTER_WIDTH},
        {"il2cpp_array_object_header_size", 4 * POINTER_WIDTH},
        {"il2cpp_offset_of_array_length_in_array_object_header", 3 * POINTER_WIDTH},
        {"il2cpp_offset_of_array_bounds_in_array_object_header", 2 * POINTER_WIDTH},
    };
    for (size_t i = 0; i < ARRAYSIZE(sizes); ++i)
        if (!api_body(checks, sizes[i].api, &body, FALSE) ||
            !reports_constant(&body, sizes[i].value)) goto done;

    if (!check_heap_size(checks) ||
        !check_import(checks, "il2cpp_thread_current", "TlsGetValue") ||
        !check_import(checks, "il2cpp_is_debugger_attached", "IsDebuggerPresent")) goto done;

    static const char *const attrs_apis[] = {
        "il2cpp_custom_attrs_get_attr", "il2cpp_custom_attrs_has_attr", "il2cpp_custom_attrs_construct"
    };
    DWORD initializer = 0;
    for (size_t i = 0; i < ARRAYSIZE(attrs_apis); ++i) {
        if (!api_body(checks, attrs_apis[i], &body, FALSE)) goto done;
        DWORD target = first_call(&body, NULL);
        if (!target || (initializer && target != initializer)) goto done;
        initializer = target;
    }

    if (!api_body(checks, "il2cpp_class_get_userdata_offset", &body, FALSE) ||
        !ends_ret(&body, 2) || body.instructions[0].operands[1].type != X86_OP_IMM) goto done;
    int64_t user_offset = body.instructions[0].operands[1].imm;
    if (user_offset < POINTER_WIDTH || user_offset > 4096 || !reports_constant(&body, user_offset) ||
        !api_body(checks, "il2cpp_class_set_userdata", &body, FALSE) || !ends_ret(&body, 2)) goto done;
    const NativeInstruction *setter = &body.instructions[0];
    success = setter->id == X86_INS_MOV && setter->operand_count == 2 &&
              memory(&setter->operands[0], X86_REG_RCX, POINTER_WIDTH) &&
              setter->operands[0].mem.disp == user_offset &&
              reg(&setter->operands[1]) == X86_REG_RDX;
done:
    native_body_close(&body);
    return success;
}

/* Critical wrappers add independent initialization, state, and calling
   convention checks to the distributed native struct accessors above. */

typedef struct {
    DWORD locale_target, runtime_target;
    size_t locale_index, runtime_index;
} InitCalls;

static unsigned int register_family(x86_reg reg_id) {
#define REG_FAMILY(q, d, w, b, index) \
    case X86_REG_##q: case X86_REG_##d: case X86_REG_##w: case X86_REG_##b: return 1u << (index)
    switch (reg_id) {
        REG_FAMILY(RAX, EAX, AX, AL, 0);
        REG_FAMILY(RCX, ECX, CX, CL, 1);
        REG_FAMILY(RDX, EDX, DX, DL, 2);
        REG_FAMILY(R8, R8D, R8W, R8B, 3);
        REG_FAMILY(R9, R9D, R9W, R9B, 4);
        REG_FAMILY(R10, R10D, R10W, R10B, 5);
        REG_FAMILY(R11, R11D, R11W, R11B, 6);
        REG_FAMILY(RBX, EBX, BX, BL, 7);
        REG_FAMILY(RBP, EBP, BP, BPL, 8);
        REG_FAMILY(RSI, ESI, SI, SIL, 9);
        REG_FAMILY(RDI, EDI, DI, DIL, 10);
        REG_FAMILY(R12, R12D, R12W, R12B, 11);
        REG_FAMILY(R13, R13D, R13W, R13B, 12);
        REG_FAMILY(R14, R14D, R14W, R14B, 13);
        REG_FAMILY(R15, R15D, R15W, R15B, 14);
        default: return 0;
    }
#undef REG_FAMILY
}

static DWORD direct_call(const NativeInstruction *ins) {
    return ins->id == X86_INS_CALL ? native_direct_target(ins) : 0;
}

static BOOL finds_locale_and_runtime(const NativeImage *image, const NativeBody *body,
                                    InitCalls *calls) {
    unsigned int rcx = register_family(X86_REG_RCX), rdx = register_family(X86_REG_RDX);
    BOOL found = FALSE;
    for (size_t i = 0; i < body->count; ++i) {
        DWORD locale = direct_call(&body->instructions[i]);
        if (!locale) continue;
        BOOL category_zero = FALSE;
        size_t empty_locales = 0;
        for (size_t j = i > 5 ? i - 5 : 0; j < i; ++j) {
            const NativeInstruction *ins = &body->instructions[j];
            if (ins->operand_count != 2) continue;
            unsigned int dst = register_family(reg(&ins->operands[0]));
            unsigned int src = register_family(reg(&ins->operands[1]));
            category_zero |= dst == rcx && ((ins->id == X86_INS_XOR && src == rcx) ||
                                            (ins->id == X86_INS_MOV && immediate(&ins->operands[1], 0)));
            if (ins->id != X86_INS_LEA || dst != rdx) continue;
            DWORD rva = native_rip_target(ins, 1);
            const BYTE *string = native_range(image, rva, 1);
            const IMAGE_SECTION_HEADER *section = native_section(image, rva, 1);
            if (string && !*string && section &&
                !(section->Characteristics & IMAGE_SCN_MEM_WRITE)) ++empty_locales;
        }
        if (!category_zero || empty_locales != 1) continue;
        for (size_t j = i + 1; j < min(i + 9, body->count); ++j) {
            DWORD runtime = direct_call(&body->instructions[j]);
            if (!runtime) continue;
            for (size_t k = j + 1; k < min(j + 5, body->count); ++k) {
                const NativeInstruction *ins = &body->instructions[k];
                if (ins->id != X86_INS_MOVZX || ins->operand_count != 2 ||
                    reg(&ins->operands[1]) != X86_REG_AL) continue;
                if (found) return FALSE;
                *calls = (InitCalls){locale, runtime, i, j};
                found = TRUE;
                break;
            }
            break;
        }
    }
    return found;
}

static BOOL preserves_ascii_domain(const NativeBody *body, size_t runtime_index) {
    const unsigned int rcx = register_family(X86_REG_RCX);
    const unsigned int volatile_registers = register_family(X86_REG_RAX) | rcx |
        register_family(X86_REG_RDX) | register_family(X86_REG_R8) | register_family(X86_REG_R9) |
        register_family(X86_REG_R10) | register_family(X86_REG_R11);
    unsigned int domain = rcx;
    for (size_t i = 0; i < runtime_index; ++i) {
        const NativeInstruction *ins = &body->instructions[i];
        unsigned int dst = register_family(reg(&ins->operands[0]));
        unsigned int src = register_family(reg(&ins->operands[1]));
        if (ins->id == X86_INS_MOV && ins->operand_count == 2) {
            BOOL copies_domain = (domain & src) != 0;
            domain &= ~dst;
            if (copies_domain) domain |= dst;
        } else if (ins->id == X86_INS_XOR || ins->id == X86_INS_LEA) {
            domain &= ~dst;
        } else if (ins->id == X86_INS_CALL) {
            domain &= ~volatile_registers;
        }
    }
    return (domain & rcx) != 0;
}

static BOOL check_init_wrappers(SemanticChecks *checks, DWORD *runtime) {
    NativeBody body = {0};
    InitCalls ascii = {0}, utf16 = {0};
    BOOL success = FALSE;
    if (api_body(checks, "il2cpp_init", &body, TRUE) &&
        finds_locale_and_runtime(checks->image, &body, &ascii) &&
        preserves_ascii_domain(&body, ascii.runtime_index) &&
        api_body(checks, "il2cpp_init_utf16", &body, TRUE) &&
        finds_locale_and_runtime(checks->image, &body, &utf16) &&
        ascii.locale_target == utf16.locale_target && ascii.runtime_target == utf16.runtime_target) {
        for (size_t i = 0; i < utf16.locale_index; ++i) {
            if (direct_call(&body.instructions[i])) {
                *runtime = ascii.runtime_target;
                success = TRUE;
                break;
            }
        }
    }
    native_body_close(&body);
    return success;
}

static BOOL writes_cell(const NativeBody *body, DWORD cell) {
    for (size_t i = 0; i < body->count; ++i) {
        const NativeInstruction *ins = &body->instructions[i];
        if (ins->id == X86_INS_MOV && ins->operand_count == 2 &&
            native_rip_target(ins, 0) == cell) return TRUE;
    }
    return FALSE;
}

static BOOL tests_register(const NativeInstruction *ins, x86_reg value) {
    return ins->id == X86_INS_TEST && ins->operand_count == 2 &&
           reg(&ins->operands[0]) == value && reg(&ins->operands[1]) == value;
}

static DWORD find_domain_cell(const NativeBody *body) {
    if (ends_ret(body, 2) && body->instructions[0].id == X86_INS_LEA &&
        reg(&body->instructions[0].operands[0]) == X86_REG_RAX)
        return native_rip_target(&body->instructions[0], 1);

    DWORD last_load = 0, shared = 0;
    BOOL guarded = FALSE;
    for (size_t i = 0; i < body->count; ++i) {
        const NativeInstruction *load = &body->instructions[i];
        guarded |= tests_register(load, X86_REG_RAX);
        if (load->id != X86_INS_MOV || reg(&load->operands[0]) != X86_REG_RAX) continue;
        DWORD cell = native_rip_target(load, 1);
        if (!cell) continue;
        last_load = cell;
        for (size_t j = 0; j < body->count; ++j) {
            const NativeInstruction *store = &body->instructions[j];
            if (store->id != X86_INS_MOV || reg(&store->operands[1]) != X86_REG_RAX ||
                native_rip_target(store, 0) != cell) continue;
            if (shared && shared != cell) return 0;
            shared = cell;
        }
    }
    return guarded && last_load == shared ? shared : 0;
}

static BOOL check_singletons(SemanticChecks *checks, DWORD runtime) {
    NativeBody body = {0};
    BOOL success = FALSE;
    DWORD pool = global_cell(checks, "il2cpp_memory_pool_get_region_size", FALSE);
    if (!pool || global_cell(checks, "il2cpp_memory_pool_set_region_size", TRUE) != pool) goto done;
    DWORD corlib = global_cell(checks, "il2cpp_get_corlib", FALSE);
    if (!corlib || corlib == pool || !body_follow(checks, runtime, &body, TRUE) ||
        !writes_cell(&body, corlib)) goto done;
    if (!api_body(checks, "il2cpp_domain_get", &body, TRUE)) goto done;
    DWORD domain = find_domain_cell(&body);
    success = domain != pool && domain != corlib && native_writable_slot(checks->image, domain);
done:
    native_body_close(&body);
    return success;
}

static BOOL clears_exception(const NativeInstruction *ins) {
    const cs_x86_op *destination = &ins->operands[0];
    return ins->id == X86_INS_MOV && ins->operand_count == 2 &&
           memory(destination, X86_REG_R9, POINTER_WIDTH) && !destination->mem.disp &&
           immediate(&ins->operands[1], 0);
}

static BOOL check_runtime_invoke(SemanticChecks *checks) {
    NativeBody body = {0};
    BOOL success = FALSE;
    if (!api_body(checks, "il2cpp_runtime_invoke", &body, TRUE)) goto done;
    size_t clear = 0;
    while (clear < body.count && !clears_exception(&body.instructions[clear])) ++clear;
    if (clear == body.count) goto done;
    BOOL guarded = FALSE;
    for (size_t i = clear > 5 ? clear - 5 : 0; i < clear; ++i)
        guarded |= tests_register(&body.instructions[i], X86_REG_R9);
    if (!guarded) goto done;

    const unsigned int required = register_family(X86_REG_RCX) |
                                  register_family(X86_REG_RDX) | register_family(X86_REG_R8);
    for (size_t i = clear + 1; i < body.count; ++i) {
        if (!direct_call(&body.instructions[i])) continue;
        unsigned int setup = 0;
        for (size_t j = i > clear + 4 ? i - 4 : clear + 1; j < i; ++j) {
            const NativeInstruction *move = &body.instructions[j];
            if (move->id == X86_INS_MOV && move->operand_count == 2 && reg(&move->operands[1]))
                setup |= register_family(reg(&move->operands[0]));
        }
        if ((setup & required) == required) {
            success = TRUE;
            break;
        }
    }
done:
    native_body_close(&body);
    return success;
}

BOOL native_validate_semantics(NativeDecoder *decoder, const NativeImage *image,
                               const NativeApiMap *map, const char **failure) {
    SemanticChecks checks = {decoder, image, map, {0}, NULL};
    DWORD runtime = 0;
    if (!validate_getters(&checks) || !check_init_wrappers(&checks, &runtime) ||
        !check_singletons(&checks, runtime) || !check_runtime_invoke(&checks)) {
        *failure = checks.failure;
        return FALSE;
    }
    size_t unanchored = 0;
    for (size_t i = 0; i < map->count; ++i) {
        unanchored = checks.covered[i] ? 0 : unanchored + 1;
        if (unanchored > 20) {
            *failure = "native semantic anchors are too sparse for this API profile";
            return FALSE;
        }
    }
    return TRUE;
}
#endif
