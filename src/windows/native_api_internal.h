/*
 * Native IL2CPP discovery by David Salas Kalendar, 2026-11-07.
 * Distributed under the GNU LGPL 2.1; see LICENSE.
 */
#ifndef DOORSTOP_NATIVE_API_INTERNAL_H
#define DOORSTOP_NATIVE_API_INTERNAL_H

#include "native_api.h"
#include <capstone/capstone.h>

typedef struct {
    BYTE *base;
    DWORD size;
    const IMAGE_NT_HEADERS64 *nt;
    const IMAGE_SECTION_HEADER *sections;
    const RUNTIME_FUNCTION *unwind;
    size_t unwind_count;
    /* Indices plus one, reserving zero for no root or successor. */
    DWORD *roots;
    DWORD *first;
    DWORD *next;
} NativeImage;

typedef struct {
    HMODULE library;
    csh handle;
    cs_insn *scratch;
    cs_err (CAPSTONE_API *open)(cs_arch, cs_mode, csh *);
    cs_err (CAPSTONE_API *close)(csh *);
    cs_err (CAPSTONE_API *option)(csh, cs_opt_type, size_t);
    cs_insn *(CAPSTONE_API *alloc)(csh);
    void (CAPSTONE_API *free)(cs_insn *, size_t);
    bool (CAPSTONE_API *disasm_iter)(csh, const uint8_t **, size_t *, uint64_t *, cs_insn *);
    unsigned int (CAPSTONE_API *version)(int *, int *);
} NativeDecoder;

typedef struct {
    unsigned int id;
    DWORD rva;
    BYTE size, operand_count;
    BOOL jump, writes_rdx;
    cs_x86_op operands[3];
} NativeInstruction;

typedef struct {
    NativeInstruction *instructions;
    size_t count, capacity;
    DWORD rva;
} NativeBody;

BOOL native_image_open(NativeImage *image, HMODULE module);
void native_image_close(NativeImage *image);
const void *native_range(const NativeImage *image, DWORD rva, size_t length);
const IMAGE_SECTION_HEADER *native_section(const NativeImage *image, DWORD rva,
                                           size_t length);
BOOL native_writable_slot(const NativeImage *image, DWORD rva);
DWORD native_function_index(const NativeImage *image, DWORD rva);
BOOL native_has_import(const NativeImage *image, DWORD iat_rva, const char *name);
BOOL native_decoder_open(NativeDecoder *decoder, LPCWSTR path);
void native_decoder_close(NativeDecoder *decoder);
BOOL native_disassemble(NativeDecoder *decoder, const NativeImage *image, DWORD rva,
                         NativeBody *body);
void native_body_close(NativeBody *body);
DWORD native_rip_target(const NativeInstruction *instruction, size_t operand);
DWORD native_direct_target(const NativeInstruction *instruction);
BOOL native_validate_semantics(NativeDecoder *decoder, const NativeImage *image,
                               const NativeApiMap *map, const char **failure);

#endif
