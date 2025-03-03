// Copyright 2019 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/vm/bytecode_module.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "iree/base/api.h"
#include "iree/base/tracing.h"
#include "iree/vm/api.h"
#include "iree/vm/bytecode_module_impl.h"

// Alignment applied to each segment of the archive.
// All embedded file contents (FlatBuffers, rodata, etc) are aligned to this
// boundary.
#define IREE_VM_ARCHIVE_SEGMENT_ALIGNMENT 64

// ZIP local file header (comes immediately before each file in the archive).
// In order to find the starting offset of the FlatBuffer in a polyglot archive
// we need to parse this given the variable-length nature of it (we want to
// be robust to file name and alignment changes).
//
// NOTE: all fields are little-endian.
// NOTE: we don't care about the actual module size here; since we support
//       streaming archives trying to recover it would require much more
//       involved processing (we'd need to reference the central directory).
//       If we wanted to support users repacking ZIPs we'd probably want to
//       rewrite everything as we store offsets in the FlatBuffer that are
//       difficult to update after the archive has been produced.
#define ZIP_LOCAL_FILE_HEADER_SIGNATURE 0x04034B50u
#if defined(IREE_COMPILER_MSVC)
#pragma pack(push, 1)
#endif  // IREE_COMPILER_MSVC
typedef struct {
  uint32_t signature;  // ZIP_LOCAL_FILE_HEADER_SIGNATURE
  uint16_t version;
  uint16_t general_purpose_flag;
  uint16_t compression_method;
  uint16_t last_modified_time;
  uint16_t last_modified_date;
  uint32_t crc32;              // 0 for us
  uint32_t compressed_size;    // 0 for us
  uint32_t uncompressed_size;  // 0 for us
  uint16_t file_name_length;
  uint16_t extra_field_length;
  // file name (variable size)
  // extra field (variable size)
} IREE_ATTRIBUTE_PACKED zip_local_file_header_t;
#if defined(IREE_COMPILER_MSVC)
#pragma pack(pop)
#endif  // IREE_COMPILER_MSVC
static_assert(sizeof(zip_local_file_header_t) == 30, "bad packing");
#if !defined(IREE_ENDIANNESS_LITTLE) || !IREE_ENDIANNESS_LITTLE
#error "little endian required for zip header parsing"
#endif  // IREE_ENDIANNESS_LITTLE

// Strips any ZIP local file header from |contents| and stores the remaining
// range in |out_stripped|.
static iree_status_t iree_vm_bytecode_module_strip_zip_header(
    iree_const_byte_span_t contents, iree_const_byte_span_t* out_stripped) {
  // Ensure there's at least some bytes we can check for the header.
  // Since we're only looking to strip zip stuff here we can check on that.
  if (!contents.data ||
      contents.data_length < sizeof(zip_local_file_header_t)) {
    memmove(out_stripped, &contents, sizeof(contents));
    return iree_ok_status();
  }

  // Check to see if there's a zip local header signature.
  // For a compliant zip file this is expected to start at offset 0.
  const zip_local_file_header_t* header =
      (const zip_local_file_header_t*)contents.data;
  if (header->signature != ZIP_LOCAL_FILE_HEADER_SIGNATURE) {
    // No signature found, probably not a ZIP.
    memmove(out_stripped, &contents, sizeof(contents));
    return iree_ok_status();
  }

  // Compute the starting offset of the file.
  // Note that we still don't know (or care) if it's the file we want; actual
  // FlatBuffer verification happens later on.
  uint32_t offset =
      sizeof(*header) + header->file_name_length + header->extra_field_length;
  if (offset > contents.data_length) {
    // Is a ZIP but doesn't have enough data; error out with something more
    // useful than the FlatBuffer verification failing later on given that here
    // we know this isn't a FlatBuffer.
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "archive self-reports as a zip but does not have "
                            "enough data to contain a module");
  }

  *out_stripped = iree_make_const_byte_span(contents.data + offset,
                                            contents.data_length - offset);
  return iree_ok_status();
}

IREE_API_EXPORT iree_status_t iree_vm_bytecode_module_parse_header(
    iree_const_byte_span_t archive_contents,
    iree_const_byte_span_t* out_flatbuffer_contents,
    iree_host_size_t* out_rodata_offset) {
  // Slice off any polyglot zip header we have prior to the base of the module.
  iree_const_byte_span_t module_contents = iree_const_byte_span_empty();
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_module_strip_zip_header(
      archive_contents, &module_contents));

  // Verify there's enough data to safely check the FlatBuffer header.
  if (!module_contents.data || module_contents.data_length < 16) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "FlatBuffer data is not present or less than 16 bytes (%zu total)",
        module_contents.data_length);
  }

  // Read the size prefix from the head of the module contents; this should be
  // a 4 byte value indicating the total size of the FlatBuffer data.
  size_t length_prefix = 0;
  flatbuffers_read_size_prefix((void*)module_contents.data, &length_prefix);

  // Verify the length prefix is within bounds (always <= the remaining module
  // bytes).
  size_t length_remaining =
      module_contents.data_length - sizeof(flatbuffers_uoffset_t);
  if (length_prefix > length_remaining) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "FlatBuffer length prefix out of bounds (prefix is "
                            "%zu but only %zu available)",
                            length_prefix, length_remaining);
  }

  // Form the range of bytes containing just the FlatBuffer data.
  iree_const_byte_span_t flatbuffer_contents = iree_make_const_byte_span(
      module_contents.data + sizeof(flatbuffers_uoffset_t), length_prefix);

  if (out_flatbuffer_contents) {
    *out_flatbuffer_contents = flatbuffer_contents;
  }
  if (out_rodata_offset) {
    // rodata begins immediately following the FlatBuffer in memory.
    iree_host_size_t rodata_offset = iree_host_align(
        (iree_host_size_t)(flatbuffer_contents.data - archive_contents.data) +
            length_prefix,
        IREE_VM_ARCHIVE_SEGMENT_ALIGNMENT);
    *out_rodata_offset = rodata_offset;
  }
  return iree_ok_status();
}

// Perform an strcmp between a FlatBuffers string and an IREE string view.
static bool iree_vm_flatbuffer_strcmp(flatbuffers_string_t lhs,
                                      iree_string_view_t rhs) {
  size_t lhs_size = flatbuffers_string_len(lhs);
  int x = strncmp(lhs, rhs.data, lhs_size < rhs.size ? lhs_size : rhs.size);
  return x != 0 ? x : lhs_size < rhs.size ? -1 : lhs_size > rhs.size;
}

// Resolves a type through either builtin rules or the ref registered types.
static bool iree_vm_bytecode_module_resolve_type(
    iree_vm_TypeDef_table_t type_def, iree_vm_type_def_t* out_type) {
  memset(out_type, 0, sizeof(*out_type));
  flatbuffers_string_t full_name = iree_vm_TypeDef_full_name(type_def);
  if (!flatbuffers_string_len(full_name)) {
    return false;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("i8")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_I8;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("i16")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_I16;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("i32")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_I32;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("i64")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_I64;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("f32")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_F32;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(full_name,
                                       iree_make_cstring_view("f64")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_F64;
    return true;
  } else if (iree_vm_flatbuffer_strcmp(
                 full_name, iree_make_cstring_view("!vm.opaque")) == 0) {
    out_type->value_type = IREE_VM_VALUE_TYPE_NONE;
    out_type->ref_type = IREE_VM_REF_TYPE_NULL;
    return true;
  } else if (full_name[0] == '!') {
    // Note that we drop the ! prefix:
    iree_string_view_t type_name = {full_name + 1,
                                    flatbuffers_string_len(full_name) - 1};
    if (iree_string_view_starts_with(type_name,
                                     iree_make_cstring_view("vm.list"))) {
      // This is a !vm.list<...> type. We don't actually care about the type as
      // we allow list types to be widened. Rewrite to just vm.list as that's
      // all we have registered.
      type_name = iree_make_cstring_view("vm.list");
    }
    const iree_vm_ref_type_descriptor_t* type_descriptor =
        iree_vm_ref_lookup_registered_type(type_name);
    if (type_descriptor) {
      out_type->ref_type = type_descriptor->type;
    }
    return true;
  }
  return false;
}

// Resolves all types through either builtin rules or the ref registered types.
// |type_table| can be omitted to just perform verification that all types are
// registered.
static iree_status_t iree_vm_bytecode_module_resolve_types(
    iree_vm_TypeDef_vec_t type_defs, iree_vm_type_def_t* type_table) {
  IREE_TRACE_ZONE_BEGIN(z0);
  iree_status_t status = iree_ok_status();
  for (size_t i = 0; i < iree_vm_TypeDef_vec_len(type_defs); ++i) {
    iree_vm_TypeDef_table_t type_def = iree_vm_TypeDef_vec_at(type_defs, i);
    if (!iree_vm_bytecode_module_resolve_type(type_def, &type_table[i])) {
      status = iree_make_status(IREE_STATUS_NOT_FOUND,
                                "no type registered with name '%s'",
                                iree_vm_TypeDef_full_name(type_def));
      break;
    }
  }
  IREE_TRACE_ZONE_END(z0);
  return status;
}

// Verifies the structure of the FlatBuffer so that we can avoid doing so during
// runtime. There are still some conditions we must be aware of (such as omitted
// names on functions with internal linkage), however we shouldn't need to
// bounds check anything within the FlatBuffer after this succeeds.
static iree_status_t iree_vm_bytecode_module_flatbuffer_verify(
    iree_const_byte_span_t archive_contents,
    iree_const_byte_span_t flatbuffer_contents,
    iree_host_size_t archive_rodata_offset) {
  // Run flatcc generated verification. This ensures all pointers are in-bounds
  // and that we can safely walk the file, but not that the actual contents of
  // the FlatBuffer meet our expectations.
  int verify_ret = iree_vm_BytecodeModuleDef_verify_as_root(
      flatbuffer_contents.data, flatbuffer_contents.data_length);
  if (verify_ret != flatcc_verify_ok) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "FlatBuffer verification failed: %s",
                            flatcc_verify_error_string(verify_ret));
  }

  iree_vm_BytecodeModuleDef_table_t module_def =
      iree_vm_BytecodeModuleDef_as_root(flatbuffer_contents.data);

  flatbuffers_string_t name = iree_vm_BytecodeModuleDef_name(module_def);
  if (!flatbuffers_string_len(name)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "module missing name field");
  }

  iree_vm_TypeDef_vec_t types = iree_vm_BytecodeModuleDef_types(module_def);
  for (size_t i = 0; i < iree_vm_TypeDef_vec_len(types); ++i) {
    iree_vm_TypeDef_table_t type_def = iree_vm_TypeDef_vec_at(types, i);
    if (!type_def) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "types[%zu] missing body", i);
    }
    flatbuffers_string_t full_name = iree_vm_TypeDef_full_name(type_def);
    if (flatbuffers_string_len(full_name) <= 0) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "types[%zu] missing name", i);
    }
  }

  iree_vm_RodataSegmentDef_vec_t rodata_segments =
      iree_vm_BytecodeModuleDef_rodata_segments(module_def);
  for (size_t i = 0; i < iree_vm_RodataSegmentDef_vec_len(rodata_segments);
       ++i) {
    iree_vm_RodataSegmentDef_table_t segment =
        iree_vm_RodataSegmentDef_vec_at(rodata_segments, i);
    if (iree_vm_RodataSegmentDef_embedded_data_is_present(segment)) {
      continue;  // embedded data is verified by FlatBuffers
    }
    uint64_t segment_offset =
        iree_vm_RodataSegmentDef_external_data_offset(segment);
    uint64_t segment_length =
        iree_vm_RodataSegmentDef_external_data_length(segment);
    uint64_t segment_end =
        archive_rodata_offset + segment_offset + segment_length;
    if (segment_end > archive_contents.data_length) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "rodata[%zu] external reference out of range", i);
    }
  }

  iree_vm_ImportFunctionDef_vec_t imported_functions =
      iree_vm_BytecodeModuleDef_imported_functions(module_def);
  iree_vm_ExportFunctionDef_vec_t exported_functions =
      iree_vm_BytecodeModuleDef_exported_functions(module_def);
  iree_vm_FunctionDescriptor_vec_t function_descriptors =
      iree_vm_BytecodeModuleDef_function_descriptors(module_def);

  for (size_t i = 0; i < iree_vm_ImportFunctionDef_vec_len(imported_functions);
       ++i) {
    iree_vm_ImportFunctionDef_table_t import_def =
        iree_vm_ImportFunctionDef_vec_at(imported_functions, i);
    if (!import_def) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "imports[%zu] missing body", i);
    }
    flatbuffers_string_t full_name =
        iree_vm_ImportFunctionDef_full_name(import_def);
    if (!flatbuffers_string_len(full_name)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "imports[%zu] missing full_name", i);
    }
  }

  for (size_t i = 0; i < iree_vm_ExportFunctionDef_vec_len(exported_functions);
       ++i) {
    iree_vm_ExportFunctionDef_table_t export_def =
        iree_vm_ExportFunctionDef_vec_at(exported_functions, i);
    if (!export_def) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "exports[%zu] missing body", i);
    }
    flatbuffers_string_t local_name =
        iree_vm_ExportFunctionDef_local_name(export_def);
    if (!flatbuffers_string_len(local_name)) {
      return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                              "exports[%zu] missing local_name", i);
    }
    iree_host_size_t internal_ordinal =
        iree_vm_ExportFunctionDef_internal_ordinal(export_def);
    if (internal_ordinal >=
        iree_vm_FunctionDescriptor_vec_len(function_descriptors)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "exports[%zu] internal_ordinal out of bounds (0 < %zu < %zu)", i,
          internal_ordinal,
          iree_vm_FunctionDescriptor_vec_len(function_descriptors));
    }
  }

  // Verify that we can properly handle the bytecode embedded in the module.
  // We require that major versions match and allow loading of older minor
  // versions (we keep changes backwards-compatible).
  const uint32_t bytecode_version =
      iree_vm_BytecodeModuleDef_bytecode_version(module_def);
  const uint32_t bytecode_version_major = bytecode_version >> 16;
  const uint32_t bytecode_version_minor = bytecode_version & 0xFFFF;
  if ((bytecode_version_major != IREE_VM_BYTECODE_VERSION_MAJOR) ||
      (bytecode_version_minor > IREE_VM_BYTECODE_VERSION_MINOR)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "bytecode version mismatch; runtime supports %d.%d, module has %d.%d",
        IREE_VM_BYTECODE_VERSION_MAJOR, IREE_VM_BYTECODE_VERSION_MINOR,
        bytecode_version_major, bytecode_version_minor);
  }

  flatbuffers_uint8_vec_t bytecode_data =
      iree_vm_BytecodeModuleDef_bytecode_data(module_def);
  for (size_t i = 0;
       i < iree_vm_FunctionDescriptor_vec_len(function_descriptors); ++i) {
    iree_vm_FunctionDescriptor_struct_t function_descriptor =
        iree_vm_FunctionDescriptor_vec_at(function_descriptors, i);
    if (function_descriptor->bytecode_offset < 0 ||
        function_descriptor->bytecode_offset +
                function_descriptor->bytecode_length >
            flatbuffers_uint8_vec_len(bytecode_data)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "functions[%zu] descriptor bytecode span out of range (0 < %d < %zu)",
          i, function_descriptor->bytecode_offset,
          flatbuffers_uint8_vec_len(bytecode_data));
    }
    if (function_descriptor->i32_register_count > IREE_I32_REGISTER_COUNT ||
        function_descriptor->ref_register_count > IREE_REF_REGISTER_COUNT) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "functions[%zu] descriptor register count out of range", i);
    }

    // TODO(benvanik): run bytecode verifier on contents.
  }

  return iree_ok_status();
}

static iree_status_t iree_vm_bytecode_map_internal_ordinal(
    iree_vm_bytecode_module_t* module, iree_vm_function_t function,
    uint16_t* out_ordinal,
    iree_vm_FunctionSignatureDef_table_t* out_signature_def) {
  *out_ordinal = 0;
  if (out_signature_def) *out_signature_def = NULL;

  uint16_t ordinal = function.ordinal;
  iree_vm_FunctionSignatureDef_table_t signature_def = NULL;
  if (function.linkage == IREE_VM_FUNCTION_LINKAGE_EXPORT) {
    // Look up the internal ordinal index of this export in the function table.
    iree_vm_ExportFunctionDef_vec_t exported_functions =
        iree_vm_BytecodeModuleDef_exported_functions(module->def);
    IREE_ASSERT_LT(ordinal,
                   iree_vm_ExportFunctionDef_vec_len(exported_functions),
                   "export ordinal out of range (0 < %zu < %zu)", ordinal,
                   iree_vm_ExportFunctionDef_vec_len(exported_functions));
    iree_vm_ExportFunctionDef_table_t function_def =
        iree_vm_ExportFunctionDef_vec_at(exported_functions, function.ordinal);
    ordinal = iree_vm_ExportFunctionDef_internal_ordinal(function_def);
    signature_def = iree_vm_ExportFunctionDef_signature(function_def);
  } else {
    // TODO(benvanik): support querying the internal functions, which could be
    // useful for debugging. Or maybe we just drop them forever?
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "cannot map imported/internal functions; no entry "
                            "in the function table");
  }

  if (ordinal >= module->function_descriptor_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "function ordinal out of range (0 < %u < %zu)",
                            function.ordinal,
                            module->function_descriptor_count);
  }

  *out_ordinal = ordinal;
  if (out_signature_def) *out_signature_def = signature_def;
  return iree_ok_status();
}

static void iree_vm_bytecode_module_destroy(void* self) {
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  IREE_TRACE_ZONE_BEGIN(z0);

  module->def = NULL;
  iree_allocator_free(module->archive_allocator,
                      (void*)module->archive_contents.data);
  module->archive_contents = iree_const_byte_span_empty();
  module->archive_allocator = iree_allocator_null();

  iree_allocator_free(module->allocator, module);

  IREE_TRACE_ZONE_END(z0);
}

static iree_string_view_t iree_vm_bytecode_module_name(void* self) {
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  flatbuffers_string_t name = iree_vm_BytecodeModuleDef_name(module->def);
  return iree_make_string_view(name, flatbuffers_string_len(name));
}

static iree_vm_module_signature_t iree_vm_bytecode_module_signature(
    void* self) {
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  iree_vm_module_signature_t signature;
  memset(&signature, 0, sizeof(signature));
  signature.import_function_count = iree_vm_ImportFunctionDef_vec_len(
      iree_vm_BytecodeModuleDef_imported_functions(module->def));
  signature.export_function_count = iree_vm_ExportFunctionDef_vec_len(
      iree_vm_BytecodeModuleDef_exported_functions(module->def));
  signature.internal_function_count = module->function_descriptor_count;
  return signature;
}

// Tries to return the original function name for internal function |ordinal|.
// Empty if the debug database has been stripped from the flatbuffer.
static flatbuffers_string_t
iree_vm_bytecode_module_lookup_internal_function_name(
    iree_vm_BytecodeModuleDef_table_t module_def, iree_host_size_t ordinal) {
  iree_vm_DebugDatabaseDef_table_t debug_database_def =
      iree_vm_BytecodeModuleDef_debug_database(module_def);
  if (!debug_database_def) return NULL;
  iree_vm_FunctionSourceMapDef_vec_t source_maps_vec =
      iree_vm_DebugDatabaseDef_functions(debug_database_def);
  iree_vm_FunctionSourceMapDef_table_t source_map_def =
      ordinal < iree_vm_FunctionSourceMapDef_vec_len(source_maps_vec)
          ? iree_vm_FunctionSourceMapDef_vec_at(source_maps_vec, ordinal)
          : NULL;
  if (!source_map_def) return NULL;
  return iree_vm_FunctionSourceMapDef_local_name(source_map_def);
}

static iree_status_t iree_vm_bytecode_module_get_function(
    void* self, iree_vm_function_linkage_t linkage, iree_host_size_t ordinal,
    iree_vm_function_t* out_function, iree_string_view_t* out_name,
    iree_vm_function_signature_t* out_signature) {
  if (out_function) {
    memset(out_function, 0, sizeof(*out_function));
  }
  if (out_name) {
    memset(out_name, 0, sizeof(*out_name));
  }
  if (out_signature) {
    memset(out_signature, 0, sizeof(*out_signature));
  }

  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  flatbuffers_string_t name = NULL;
  iree_vm_FunctionSignatureDef_table_t signature = NULL;
  if (linkage == IREE_VM_FUNCTION_LINKAGE_IMPORT ||
      linkage == IREE_VM_FUNCTION_LINKAGE_IMPORT_OPTIONAL) {
    iree_vm_ImportFunctionDef_vec_t imported_functions =
        iree_vm_BytecodeModuleDef_imported_functions(module->def);
    if (ordinal >= iree_vm_ImportFunctionDef_vec_len(imported_functions)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "import ordinal out of range (0 < %zu < %zu)", ordinal,
          iree_vm_ImportFunctionDef_vec_len(imported_functions));
    }
    iree_vm_ImportFunctionDef_table_t import_def =
        iree_vm_ImportFunctionDef_vec_at(imported_functions, ordinal);
    name = iree_vm_ImportFunctionDef_full_name(import_def);
    signature = iree_vm_ImportFunctionDef_signature(import_def);
    if (iree_all_bits_set(iree_vm_ImportFunctionDef_flags(import_def),
                          iree_vm_ImportFlagBits_OPTIONAL)) {
      linkage = IREE_VM_FUNCTION_LINKAGE_IMPORT_OPTIONAL;
    }
  } else if (linkage == IREE_VM_FUNCTION_LINKAGE_EXPORT) {
    iree_vm_ExportFunctionDef_vec_t exported_functions =
        iree_vm_BytecodeModuleDef_exported_functions(module->def);
    if (ordinal >= iree_vm_ExportFunctionDef_vec_len(exported_functions)) {
      return iree_make_status(
          IREE_STATUS_INVALID_ARGUMENT,
          "export ordinal out of range (0 < %zu < %zu)", ordinal,
          iree_vm_ExportFunctionDef_vec_len(exported_functions));
    }
    iree_vm_ExportFunctionDef_table_t export_def =
        iree_vm_ExportFunctionDef_vec_at(exported_functions, ordinal);
    name = iree_vm_ExportFunctionDef_local_name(export_def);
    signature = iree_vm_ExportFunctionDef_signature(export_def);
  } else if (linkage == IREE_VM_FUNCTION_LINKAGE_INTERNAL) {
#if IREE_VM_BACKTRACE_ENABLE
    name = iree_vm_bytecode_module_lookup_internal_function_name(module->def,
                                                                 ordinal);
#endif  // IREE_VM_BACKTRACE_ENABLE
  }

  if (out_function) {
    out_function->module = &module->interface;
    out_function->linkage = linkage;
    out_function->ordinal = (uint16_t)ordinal;
  }
  if (out_name && name) {
    out_name->data = name;
    out_name->size = flatbuffers_string_len(name);
  }
  if (out_signature && signature) {
    flatbuffers_string_t calling_convention =
        iree_vm_FunctionSignatureDef_calling_convention(signature);
    out_signature->calling_convention.data = calling_convention;
    out_signature->calling_convention.size =
        flatbuffers_string_len(calling_convention);
  }

  return iree_ok_status();
}

static iree_status_t iree_vm_bytecode_module_get_function_reflection_attr(
    void* self, iree_vm_function_linkage_t linkage, iree_host_size_t ordinal,
    iree_host_size_t index, iree_string_view_t* key,
    iree_string_view_t* value) {
  if (linkage != IREE_VM_FUNCTION_LINKAGE_EXPORT) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "only exported functions can be queried");
  }

  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  iree_vm_ExportFunctionDef_vec_t exported_functions =
      iree_vm_BytecodeModuleDef_exported_functions(module->def);

  if (ordinal >= iree_vm_ExportFunctionDef_vec_len(exported_functions)) {
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "function ordinal out of range (0 < %zu < %zu)", ordinal,
        iree_vm_ExportFunctionDef_vec_len(exported_functions));
  }

  iree_vm_ExportFunctionDef_table_t function_def =
      iree_vm_ExportFunctionDef_vec_at(exported_functions, ordinal);
  iree_vm_FunctionSignatureDef_table_t signature_def =
      iree_vm_ExportFunctionDef_signature(function_def);
  if (!signature_def) {
    return iree_make_status(
        IREE_STATUS_NOT_FOUND,
        "reflection attribute at index %zu not found; no signature", index);
  }
  iree_vm_ReflectionAttrDef_vec_t reflection_attrs =
      iree_vm_FunctionSignatureDef_reflection_attrs(signature_def);
  if (!reflection_attrs ||
      index >= iree_vm_ReflectionAttrDef_vec_len(reflection_attrs)) {
    return iree_make_status(IREE_STATUS_NOT_FOUND,
                            "reflection attribute at index %zu not found",
                            index);
  }
  iree_vm_ReflectionAttrDef_table_t attr =
      iree_vm_ReflectionAttrDef_vec_at(reflection_attrs, index);
  flatbuffers_string_t attr_key = iree_vm_ReflectionAttrDef_key(attr);
  flatbuffers_string_t attr_value = iree_vm_ReflectionAttrDef_value(attr);
  if (!flatbuffers_string_len(attr_key) ||
      !flatbuffers_string_len(attr_value)) {
    // Because reflection metadata should not impose any overhead for the
    // non reflection case, we do not eagerly validate it on load -- instead
    // verify it structurally as needed.
    return iree_make_status(IREE_STATUS_FAILED_PRECONDITION,
                            "reflection attribute missing fields");
  }

  key->data = attr_key;
  key->size = flatbuffers_string_len(attr_key);
  value->data = attr_value;
  value->size = flatbuffers_string_len(attr_value);

  return iree_ok_status();
}

static iree_status_t iree_vm_bytecode_module_lookup_function(
    void* self, iree_vm_function_linkage_t linkage, iree_string_view_t name,
    iree_vm_function_t* out_function) {
  IREE_ASSERT_ARGUMENT(out_function);
  memset(out_function, 0, sizeof(iree_vm_function_t));

  if (iree_string_view_is_empty(name)) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "function name required for query");
  }

  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  out_function->linkage = linkage;
  out_function->module = &module->interface;

  // NOTE: we could organize exports alphabetically so we could bsearch.
  if (linkage == IREE_VM_FUNCTION_LINKAGE_IMPORT ||
      linkage == IREE_VM_FUNCTION_LINKAGE_IMPORT_OPTIONAL) {
    iree_vm_ImportFunctionDef_vec_t imported_functions =
        iree_vm_BytecodeModuleDef_imported_functions(module->def);
    for (iree_host_size_t ordinal = 0;
         ordinal < iree_vm_ImportFunctionDef_vec_len(imported_functions);
         ++ordinal) {
      iree_vm_ImportFunctionDef_table_t import_def =
          iree_vm_ImportFunctionDef_vec_at(imported_functions, ordinal);
      if (iree_vm_flatbuffer_strcmp(
              iree_vm_ImportFunctionDef_full_name(import_def), name) == 0) {
        out_function->ordinal = ordinal;
        if (iree_all_bits_set(iree_vm_ImportFunctionDef_flags(import_def),
                              iree_vm_ImportFlagBits_OPTIONAL)) {
          out_function->linkage = IREE_VM_FUNCTION_LINKAGE_IMPORT_OPTIONAL;
        }
        return iree_ok_status();
      }
    }
  } else if (linkage == IREE_VM_FUNCTION_LINKAGE_EXPORT) {
    iree_vm_ExportFunctionDef_vec_t exported_functions =
        iree_vm_BytecodeModuleDef_exported_functions(module->def);
    for (iree_host_size_t ordinal = 0;
         ordinal < iree_vm_ExportFunctionDef_vec_len(exported_functions);
         ++ordinal) {
      iree_vm_ExportFunctionDef_table_t export_def =
          iree_vm_ExportFunctionDef_vec_at(exported_functions, ordinal);
      if (iree_vm_flatbuffer_strcmp(
              iree_vm_ExportFunctionDef_local_name(export_def), name) == 0) {
        out_function->ordinal = ordinal;
        return iree_ok_status();
      }
    }
  }

  return iree_make_status(IREE_STATUS_NOT_FOUND,
                          "function with the given name not found");
}

static iree_status_t iree_vm_bytecode_location_format(
    int32_t location_ordinal,
    iree_vm_LocationTypeDef_union_vec_t location_table,
    iree_vm_source_location_format_flags_t flags,
    iree_string_builder_t* builder) {
  iree_vm_LocationTypeDef_union_t location =
      iree_vm_LocationTypeDef_union_vec_at(location_table, location_ordinal);
  switch (location.type) {
    default:
    case iree_vm_LocationTypeDef_NONE: {
      return iree_string_builder_append_cstring(builder, "[unknown]");
    }
    case iree_vm_LocationTypeDef_CallSiteLocDef: {
      // NOTE: MLIR prints caller->callee, but in a stack trace we want the
      // upside-down callee->caller.
      iree_vm_CallSiteLocDef_table_t loc =
          (iree_vm_CallSiteLocDef_table_t)location.value;
      IREE_RETURN_IF_ERROR(iree_vm_bytecode_location_format(
          iree_vm_CallSiteLocDef_callee(loc), location_table, flags, builder));
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(builder, "\n      at "));
      return iree_vm_bytecode_location_format(
          iree_vm_CallSiteLocDef_caller(loc), location_table, flags, builder);
    }
    case iree_vm_LocationTypeDef_FileLineColLocDef: {
      iree_vm_FileLineColLocDef_table_t loc =
          (iree_vm_FileLineColLocDef_table_t)location.value;
      flatbuffers_string_t filename = iree_vm_FileLineColLocDef_filename(loc);
      return iree_string_builder_append_format(
          builder, "%.*s:%d:%d", (int)flatbuffers_string_len(filename),
          filename, iree_vm_FileLineColLocDef_line(loc),
          iree_vm_FileLineColLocDef_column(loc));
    }
    case iree_vm_LocationTypeDef_FusedLocDef: {
      iree_vm_FusedLocDef_table_t loc =
          (iree_vm_FusedLocDef_table_t)location.value;
      if (iree_vm_FusedLocDef_metadata_is_present(loc)) {
        flatbuffers_string_t metadata = iree_vm_FusedLocDef_metadata(loc);
        IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
            builder, "<%.*s>", (int)flatbuffers_string_len(metadata),
            metadata));
      }
      IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "[\n"));
      flatbuffers_int32_vec_t child_locs = iree_vm_FusedLocDef_locations(loc);
      for (size_t i = 0; i < flatbuffers_int32_vec_len(child_locs); ++i) {
        if (i == 0) {
          IREE_RETURN_IF_ERROR(
              iree_string_builder_append_cstring(builder, "    "));
        } else {
          IREE_RETURN_IF_ERROR(
              iree_string_builder_append_cstring(builder, ",\n    "));
        }
        IREE_RETURN_IF_ERROR(iree_vm_bytecode_location_format(
            flatbuffers_int32_vec_at(child_locs, i), location_table, flags,
            builder));
      }
      IREE_RETURN_IF_ERROR(
          iree_string_builder_append_cstring(builder, "\n  ]"));
      return iree_ok_status();
    }
    case iree_vm_LocationTypeDef_NameLocDef: {
      iree_vm_NameLocDef_table_t loc =
          (iree_vm_NameLocDef_table_t)location.value;
      flatbuffers_string_t name = iree_vm_NameLocDef_name(loc);
      IREE_RETURN_IF_ERROR(iree_string_builder_append_format(
          builder, "\"%.*s\"", (int)flatbuffers_string_len(name), name));
      if (iree_vm_NameLocDef_child_location_is_present(loc)) {
        IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, "("));
        IREE_RETURN_IF_ERROR(iree_vm_bytecode_location_format(
            iree_vm_NameLocDef_child_location(loc), location_table, flags,
            builder));
        IREE_RETURN_IF_ERROR(iree_string_builder_append_cstring(builder, ")"));
      }
      return iree_ok_status();
    }
  }
}

static iree_status_t iree_vm_bytecode_module_source_location_format(
    void* self, uint64_t data[2], iree_vm_source_location_format_flags_t flags,
    iree_string_builder_t* builder) {
  iree_vm_DebugDatabaseDef_table_t debug_database_def =
      (iree_vm_DebugDatabaseDef_table_t)self;
  iree_vm_FunctionSourceMapDef_table_t source_map_def =
      (iree_vm_FunctionSourceMapDef_table_t)data[0];
  iree_vm_BytecodeLocationDef_vec_t locations =
      iree_vm_FunctionSourceMapDef_locations(source_map_def);
  iree_vm_source_offset_t source_offset = (iree_vm_source_offset_t)data[1];

  size_t location_def_ordinal =
      iree_vm_BytecodeLocationDef_vec_scan_by_bytecode_offset(
          locations, (int32_t)source_offset);
  if (location_def_ordinal == -1) {
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  }
  iree_vm_BytecodeLocationDef_struct_t location_def =
      iree_vm_BytecodeLocationDef_vec_at(locations, location_def_ordinal);
  if (!location_def) {
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  }

  // Print source location stack trace.
  iree_vm_LocationTypeDef_union_vec_t location_table =
      iree_vm_DebugDatabaseDef_location_table_union(debug_database_def);
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_location_format(
      location_def->location, location_table, flags, builder));

  return iree_ok_status();
}

static iree_status_t iree_vm_bytecode_module_resolve_source_location(
    void* self, iree_vm_stack_frame_t* frame,
    iree_vm_source_location_t* out_source_location) {
  // Get module debug database, if available.
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  iree_vm_BytecodeModuleDef_table_t module_def = module->def;
  iree_vm_DebugDatabaseDef_table_t debug_database_def =
      iree_vm_BytecodeModuleDef_debug_database(module_def);
  if (!debug_database_def) {
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  }

  // Map the (potentially) export ordinal into the internal function ordinal in
  // the function descriptor table.
  uint16_t ordinal;
  if (frame->function.linkage == IREE_VM_FUNCTION_LINKAGE_INTERNAL) {
    ordinal = frame->function.ordinal;
  } else {
    IREE_RETURN_IF_ERROR(iree_vm_bytecode_map_internal_ordinal(
        module, frame->function, &ordinal, NULL));
  }

  // Lookup the source map for the function, if available.
  iree_vm_FunctionSourceMapDef_vec_t source_maps_vec =
      iree_vm_DebugDatabaseDef_functions(debug_database_def);
  iree_vm_FunctionSourceMapDef_table_t source_map_def =
      ordinal < iree_vm_FunctionSourceMapDef_vec_len(source_maps_vec)
          ? iree_vm_FunctionSourceMapDef_vec_at(source_maps_vec, ordinal)
          : NULL;
  if (!source_map_def) {
    return iree_status_from_code(IREE_STATUS_UNAVAILABLE);
  }

  // The source location stores the source map and PC and will perform the
  // actual lookup within the source map on demand.
  out_source_location->self = (void*)debug_database_def;
  out_source_location->data[0] = (uint64_t)source_map_def;
  out_source_location->data[1] = (uint64_t)frame->pc;
  out_source_location->format = iree_vm_bytecode_module_source_location_format;
  return iree_ok_status();
}

// Lays out the nested tables within a |state| structure.
// Returns the total size of the structure and all tables with padding applied.
// |state| may be null if only the structure size is required for allocation.
static iree_host_size_t iree_vm_bytecode_module_layout_state(
    iree_vm_BytecodeModuleDef_table_t module_def,
    iree_vm_bytecode_module_state_t* state) {
  iree_vm_ModuleStateDef_table_t module_state_def =
      iree_vm_BytecodeModuleDef_module_state(module_def);
  iree_host_size_t rwdata_storage_capacity = 0;
  iree_host_size_t global_ref_count = 0;
  if (module_state_def) {
    rwdata_storage_capacity =
        iree_vm_ModuleStateDef_global_bytes_capacity(module_state_def);
    global_ref_count =
        iree_vm_ModuleStateDef_global_ref_count(module_state_def);
  }
  iree_host_size_t rodata_ref_count = iree_vm_RodataSegmentDef_vec_len(
      iree_vm_BytecodeModuleDef_rodata_segments(module_def));
  iree_host_size_t import_function_count = iree_vm_ImportFunctionDef_vec_len(
      iree_vm_BytecodeModuleDef_imported_functions(module_def));

  uint8_t* base_ptr = (uint8_t*)state;
  iree_host_size_t offset =
      iree_host_align(sizeof(iree_vm_bytecode_module_state_t), 16);

  if (state) {
    state->rwdata_storage =
        iree_make_byte_span(base_ptr + offset, rwdata_storage_capacity);
  }
  offset += iree_host_align(rwdata_storage_capacity, 16);

  if (state) {
    state->global_ref_count = global_ref_count;
    state->global_ref_table = (iree_vm_ref_t*)(base_ptr + offset);
  }
  offset += iree_host_align(global_ref_count * sizeof(iree_vm_ref_t), 16);

  if (state) {
    state->rodata_ref_count = rodata_ref_count;
    state->rodata_ref_table = (iree_vm_buffer_t*)(base_ptr + offset);
  }
  offset += iree_host_align(rodata_ref_count * sizeof(iree_vm_buffer_t), 16);

  if (state) {
    state->import_count = import_function_count;
    state->import_table = (iree_vm_bytecode_import_t*)(base_ptr + offset);
  }
  offset +=
      iree_host_align(import_function_count * sizeof(*state->import_table), 16);

  return offset;
}

static iree_status_t iree_vm_bytecode_module_alloc_state(
    void* self, iree_allocator_t allocator,
    iree_vm_module_state_t** out_module_state) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_module_state);
  *out_module_state = NULL;

  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  iree_vm_BytecodeModuleDef_table_t module_def = module->def;

  // Compute the total size required (with padding) for the state structure.
  iree_host_size_t total_state_struct_size =
      iree_vm_bytecode_module_layout_state(module_def, NULL);

  // Allocate the storage for the structure and all its nested tables.
  iree_vm_bytecode_module_state_t* state = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, total_state_struct_size,
                                (void**)&state));
  state->allocator = allocator;

  // Perform layout to get the pointers into the storage for each nested table.
  iree_vm_bytecode_module_layout_state(module_def, state);

  // Setup rodata segments to point directly at the FlatBuffer memory.
  iree_vm_RodataSegmentDef_vec_t rodata_segments =
      iree_vm_BytecodeModuleDef_rodata_segments(module_def);
  for (int i = 0; i < state->rodata_ref_count; ++i) {
    iree_vm_RodataSegmentDef_table_t segment =
        iree_vm_RodataSegmentDef_vec_at(rodata_segments, i);
    iree_byte_span_t byte_span = iree_byte_span_empty();
    if (iree_vm_RodataSegmentDef_embedded_data_is_present(segment)) {
      // Data is embedded in the FlatBuffer.
      byte_span = iree_make_byte_span(
          (uint8_t*)iree_vm_RodataSegmentDef_embedded_data(segment),
          flatbuffers_uint8_vec_len(
              iree_vm_RodataSegmentDef_embedded_data(segment)));
    } else {
      // Data is concatenated with the FlatBuffer at some relative offset.
      // Note that we've already verified the referenced range is in bounds.
      byte_span = iree_make_byte_span(
          (uint8_t*)module->archive_contents.data +
              module->archive_rodata_offset +
              iree_vm_RodataSegmentDef_external_data_offset(segment),
          iree_vm_RodataSegmentDef_external_data_length(segment));
    }
    iree_vm_buffer_t* ref = &state->rodata_ref_table[i];
    iree_vm_buffer_initialize(IREE_VM_BUFFER_ACCESS_ORIGIN_MODULE, byte_span,
                              iree_allocator_null(), ref);
  }

  *out_module_state = (iree_vm_module_state_t*)state;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}

static void iree_vm_bytecode_module_free_state(
    void* self, iree_vm_module_state_t* module_state) {
  if (!module_state) return;
  IREE_TRACE_ZONE_BEGIN(z0);

  iree_vm_bytecode_module_state_t* state =
      (iree_vm_bytecode_module_state_t*)module_state;

  // Release remaining global references.
  for (int i = 0; i < state->global_ref_count; ++i) {
    iree_vm_ref_release(&state->global_ref_table[i]);
  }

  // Ensure all rodata references are unused and deinitialized.
  for (int i = 0; i < state->rodata_ref_count; ++i) {
    iree_vm_buffer_t* ref = &state->rodata_ref_table[i];
    iree_vm_buffer_deinitialize(ref);
  }

  iree_allocator_free(state->allocator, module_state);

  IREE_TRACE_ZONE_END(z0);
}

static iree_status_t iree_vm_bytecode_module_resolve_import(
    void* self, iree_vm_module_state_t* module_state, iree_host_size_t ordinal,
    const iree_vm_function_t* function,
    const iree_vm_function_signature_t* signature) {
  IREE_ASSERT_ARGUMENT(module_state);
  iree_vm_bytecode_module_state_t* state =
      (iree_vm_bytecode_module_state_t*)module_state;
  if (ordinal >= state->import_count) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "import ordinal out of range (0 < %zu < %zu)",
                            ordinal, state->import_count);
  }

  iree_vm_bytecode_import_t* import = &state->import_table[ordinal];
  import->function = *function;

  // Split up arguments/results into fragments so that we can avoid scanning
  // during calling.
  IREE_RETURN_IF_ERROR(iree_vm_function_call_get_cconv_fragments(
      signature, &import->arguments, &import->results));

  // Precalculate bytes required to marshal argument/results across the ABI
  // boundary.
  iree_host_size_t argument_buffer_size = 0;
  iree_host_size_t result_buffer_size = 0;
  if (!iree_vm_function_call_is_variadic_cconv(import->arguments)) {
    // NOTE: variadic types don't support precalculation and the vm.call.import
    // dispatch code will handle calculating it per-call.
    IREE_RETURN_IF_ERROR(iree_vm_function_call_compute_cconv_fragment_size(
        import->arguments, /*segment_size_list=*/NULL, &argument_buffer_size));
  }
  IREE_RETURN_IF_ERROR(iree_vm_function_call_compute_cconv_fragment_size(
      import->results, /*segment_size_list=*/NULL, &result_buffer_size));
  if (argument_buffer_size > 16 * 1024 || result_buffer_size > 16 * 1024) {
    return iree_make_status(IREE_STATUS_INVALID_ARGUMENT,
                            "ABI marshaling buffer overflow on import %zu",
                            ordinal);
  }
  import->argument_buffer_size = (uint16_t)argument_buffer_size;
  import->result_buffer_size = (uint16_t)result_buffer_size;

  return iree_ok_status();
}

static iree_status_t IREE_API_PTR iree_vm_bytecode_module_notify(
    void* self, iree_vm_module_state_t* module_state, iree_vm_signal_t signal) {
  return iree_ok_status();
}

static iree_status_t iree_vm_bytecode_module_begin_call(
    void* self, iree_vm_stack_t* stack, const iree_vm_function_call_t* call,
    iree_vm_execution_result_t* out_result) {
  // NOTE: any work here adds directly to the invocation time. Avoid doing too
  // much work or touching too many unlikely-to-be-cached structures (such as
  // walking the FlatBuffer, which may cause page faults).
  IREE_ASSERT_ARGUMENT(out_result);
  memset(out_result, 0, sizeof(iree_vm_execution_result_t));

  // Map the (potentially) export ordinal into the internal function ordinal in
  // the function descriptor table.
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  uint16_t internal_ordinal = 0;
  iree_vm_FunctionSignatureDef_table_t signature_def = NULL;
  IREE_RETURN_IF_ERROR(iree_vm_bytecode_map_internal_ordinal(
      module, call->function, &internal_ordinal, &signature_def));

  // TODO(benvanik): rework this call type to avoid the need to copy it while
  // plumbing.
  iree_vm_function_call_t internal_call = *call;
  internal_call.function.linkage = IREE_VM_FUNCTION_LINKAGE_INTERNAL;
  internal_call.function.ordinal = internal_ordinal;

  // Grab calling convention string. This is not great as we are guaranteed to
  // have a bunch of cache misses, but without putting it on the descriptor
  // (which would duplicate data and slow down normal intra-module calls)
  // there's not a good way around it. In the grand scheme of things users
  // should be keeping their calls across this boundary relatively fat (compared
  // to the real work they do), so this only needs to be fast enough to blend
  // into the noise. Similar to JNI, P/Invoke, etc you don't want to have
  // imports that cost less to execute than the marshaling overhead (dozens to
  // hundreds of instructions).
  flatbuffers_string_t calling_convention =
      signature_def
          ? iree_vm_FunctionSignatureDef_calling_convention(signature_def)
          : 0;
  iree_vm_function_signature_t signature;
  memset(&signature, 0, sizeof(signature));
  signature.calling_convention.data = calling_convention;
  signature.calling_convention.size =
      flatbuffers_string_len(calling_convention);
  iree_string_view_t cconv_arguments = iree_string_view_empty();
  iree_string_view_t cconv_results = iree_string_view_empty();
  IREE_RETURN_IF_ERROR(iree_vm_function_call_get_cconv_fragments(
      &signature, &cconv_arguments, &cconv_results));

  // Jump into the dispatch routine to execute bytecode until the function
  // either returns (synchronous) or yields (asynchronous).
  return iree_vm_bytecode_dispatch_begin(stack, module, &internal_call,
                                         cconv_arguments, cconv_results,
                                         out_result);  // tail
}

static iree_status_t iree_vm_bytecode_module_resume_call(
    void* self, iree_vm_stack_t* stack, iree_byte_span_t call_results,
    iree_vm_execution_result_t* out_result) {
  IREE_ASSERT_ARGUMENT(out_result);
  memset(out_result, 0, sizeof(iree_vm_execution_result_t));

  // Resume the call by jumping back into the bytecode dispatch.
  iree_vm_bytecode_module_t* module = (iree_vm_bytecode_module_t*)self;
  return iree_vm_bytecode_dispatch_resume(stack, module, call_results,
                                          out_result);  // tail
}

IREE_API_EXPORT iree_status_t iree_vm_bytecode_module_create(
    iree_const_byte_span_t archive_contents, iree_allocator_t archive_allocator,
    iree_allocator_t allocator, iree_vm_module_t** out_module) {
  IREE_TRACE_ZONE_BEGIN(z0);
  IREE_ASSERT_ARGUMENT(out_module);
  *out_module = NULL;

  // Parse and verify the archive header to locate the FlatBuffer.
  iree_const_byte_span_t flatbuffer_contents = iree_const_byte_span_empty();
  iree_host_size_t archive_rodata_offset = 0;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_vm_bytecode_module_parse_header(
              archive_contents, &flatbuffer_contents, &archive_rodata_offset));

  IREE_TRACE_ZONE_BEGIN_NAMED(z1, "iree_vm_bytecode_module_flatbuffer_verify");
  iree_status_t status = iree_vm_bytecode_module_flatbuffer_verify(
      archive_contents, flatbuffer_contents, archive_rodata_offset);
  if (!iree_status_is_ok(status)) {
    IREE_TRACE_ZONE_END(z1);
    IREE_TRACE_ZONE_END(z0);
    return status;
  }
  IREE_TRACE_ZONE_END(z1);

  iree_vm_BytecodeModuleDef_table_t module_def =
      iree_vm_BytecodeModuleDef_as_root(flatbuffer_contents.data);
  if (!module_def) {
    IREE_TRACE_ZONE_END(z0);
    return iree_make_status(
        IREE_STATUS_INVALID_ARGUMENT,
        "failed getting root from FlatBuffer; expected identifier "
        "'" iree_vm_BytecodeModuleDef_file_identifier "' not found");
  }

  iree_vm_TypeDef_vec_t type_defs = iree_vm_BytecodeModuleDef_types(module_def);
  size_t type_table_size =
      iree_vm_TypeDef_vec_len(type_defs) * sizeof(iree_vm_type_def_t);

  iree_vm_bytecode_module_t* module = NULL;
  IREE_RETURN_AND_END_ZONE_IF_ERROR(
      z0, iree_allocator_malloc(allocator, sizeof(*module) + type_table_size,
                                (void**)&module));
  module->allocator = allocator;

  iree_vm_FunctionDescriptor_vec_t function_descriptors =
      iree_vm_BytecodeModuleDef_function_descriptors(module_def);
  module->function_descriptor_count =
      iree_vm_FunctionDescriptor_vec_len(function_descriptors);
  module->function_descriptor_table = function_descriptors;

  flatbuffers_uint8_vec_t bytecode_data =
      iree_vm_BytecodeModuleDef_bytecode_data(module_def);
  module->bytecode_data = iree_make_const_byte_span(
      bytecode_data, flatbuffers_uint8_vec_len(bytecode_data));

  module->archive_contents = archive_contents;
  module->archive_allocator = archive_allocator;
  module->archive_rodata_offset = archive_rodata_offset;
  module->def = module_def;

  module->type_count = iree_vm_TypeDef_vec_len(type_defs);
  iree_status_t resolve_status =
      iree_vm_bytecode_module_resolve_types(type_defs, module->type_table);
  if (!iree_status_is_ok(resolve_status)) {
    iree_allocator_free(allocator, module);
    IREE_TRACE_ZONE_END(z0);
    return resolve_status;
  }

  iree_vm_module_initialize(&module->interface, module);
  module->interface.destroy = iree_vm_bytecode_module_destroy;
  module->interface.name = iree_vm_bytecode_module_name;
  module->interface.signature = iree_vm_bytecode_module_signature;
  module->interface.get_function = iree_vm_bytecode_module_get_function;
  module->interface.lookup_function = iree_vm_bytecode_module_lookup_function;
#if IREE_VM_BACKTRACE_ENABLE
  module->interface.resolve_source_location =
      iree_vm_bytecode_module_resolve_source_location;
#endif  // IREE_VM_BACKTRACE_ENABLE
  module->interface.alloc_state = iree_vm_bytecode_module_alloc_state;
  module->interface.free_state = iree_vm_bytecode_module_free_state;
  module->interface.resolve_import = iree_vm_bytecode_module_resolve_import;
  module->interface.notify = iree_vm_bytecode_module_notify;
  module->interface.begin_call = iree_vm_bytecode_module_begin_call;
  module->interface.resume_call = iree_vm_bytecode_module_resume_call;
  module->interface.get_function_reflection_attr =
      iree_vm_bytecode_module_get_function_reflection_attr;

  *out_module = &module->interface;
  IREE_TRACE_ZONE_END(z0);
  return iree_ok_status();
}
