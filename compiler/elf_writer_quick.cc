/*
 * Copyright (C) 2012 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "elf_writer_quick.h"

#include <unordered_map>

#include "base/logging.h"
#include "base/unix_file/fd_file.h"
#include "buffered_output_stream.h"
#include "driver/compiler_driver.h"
#include "dwarf.h"
#include "elf_builder.h"
#include "elf_file.h"
#include "elf_utils.h"
#include "file_output_stream.h"
#include "globals.h"
#include "leb128.h"
#include "oat.h"
#include "oat_writer.h"
#include "utils.h"

namespace art {

static void PushByte(std::vector<uint8_t>* buf, int data) {
  buf->push_back(data & 0xff);
}

static uint32_t PushStr(std::vector<uint8_t>* buf, const char* str, const char* def = nullptr) {
  if (str == nullptr) {
    str = def;
  }

  uint32_t offset = buf->size();
  for (size_t i = 0; str[i] != '\0'; ++i) {
    buf->push_back(str[i]);
  }
  buf->push_back('\0');
  return offset;
}

static uint32_t PushStr(std::vector<uint8_t>* buf, const std::string &str) {
  uint32_t offset = buf->size();
  buf->insert(buf->end(), str.begin(), str.end());
  buf->push_back('\0');
  return offset;
}

static void UpdateWord(std::vector<uint8_t>* buf, int offset, int data) {
  (*buf)[offset+0] = data;
  (*buf)[offset+1] = data >> 8;
  (*buf)[offset+2] = data >> 16;
  (*buf)[offset+3] = data >> 24;
}

static void PushHalf(std::vector<uint8_t>* buf, int data) {
  buf->push_back(data & 0xff);
  buf->push_back((data >> 8) & 0xff);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Create(File* elf_file,
                            OatWriter* oat_writer,
                            const std::vector<const DexFile*>& dex_files,
                            const std::string& android_root,
                            bool is_host,
                            const CompilerDriver& driver) {
  ElfWriterQuick elf_writer(driver, elf_file);
  return elf_writer.Write(oat_writer, dex_files, android_root, is_host);
}

std::vector<uint8_t>* ConstructCIEFrameX86(bool is_x86_64) {
  std::vector<uint8_t>* cfi_info = new std::vector<uint8_t>;

  // Length (will be filled in later in this routine).
  if (is_x86_64) {
    PushWord(cfi_info, 0xffffffff);  // Indicates 64bit
    PushWord(cfi_info, 0);
    PushWord(cfi_info, 0);
  } else {
    PushWord(cfi_info, 0);
  }

  // CIE id: always 0.
  if (is_x86_64) {
    PushWord(cfi_info, 0);
    PushWord(cfi_info, 0);
  } else {
    PushWord(cfi_info, 0);
  }

  // Version: always 1.
  cfi_info->push_back(0x01);

  // Augmentation: 'zR\0'
  cfi_info->push_back(0x7a);
  cfi_info->push_back(0x52);
  cfi_info->push_back(0x0);

  // Code alignment: 1.
  EncodeUnsignedLeb128(1, cfi_info);

  // Data alignment.
  if (is_x86_64) {
    EncodeSignedLeb128(-8, cfi_info);
  } else {
    EncodeSignedLeb128(-4, cfi_info);
  }

  // Return address register.
  if (is_x86_64) {
    // R16(RIP)
    cfi_info->push_back(0x10);
  } else {
    // R8(EIP)
    cfi_info->push_back(0x08);
  }

  // Augmentation length: 1.
  cfi_info->push_back(1);

  // Augmentation data.
  if (is_x86_64) {
    // 0x04 ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata8).
    cfi_info->push_back(0x04);
  } else {
    // 0x03 ((DW_EH_PE_absptr << 4) | DW_EH_PE_udata4).
    cfi_info->push_back(0x03);
  }

  // Initial instructions.
  if (is_x86_64) {
    // DW_CFA_def_cfa R7(RSP) 8.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x07);
    cfi_info->push_back(0x08);

    // DW_CFA_offset R16(RIP) 1 (* -8).
    cfi_info->push_back(0x90);
    cfi_info->push_back(0x01);
  } else {
    // DW_CFA_def_cfa R4(ESP) 4.
    cfi_info->push_back(0x0c);
    cfi_info->push_back(0x04);
    cfi_info->push_back(0x04);

    // DW_CFA_offset R8(EIP) 1 (* -4).
    cfi_info->push_back(0x88);
    cfi_info->push_back(0x01);
  }

  // Padding to a multiple of 4
  while ((cfi_info->size() & 3) != 0) {
    // DW_CFA_nop is encoded as 0.
    cfi_info->push_back(0);
  }

  // Set the length of the CIE inside the generated bytes.
  if (is_x86_64) {
    uint32_t length = cfi_info->size() - 12;
    UpdateWord(cfi_info, 4, length);
  } else {
    uint32_t length = cfi_info->size() - 4;
    UpdateWord(cfi_info, 0, length);
  }
  return cfi_info;
}

std::vector<uint8_t>* ConstructCIEFrame(InstructionSet isa) {
  switch (isa) {
    case kX86:
      return ConstructCIEFrameX86(false);
    case kX86_64:
      return ConstructCIEFrameX86(true);

    default:
      // Not implemented.
      return nullptr;
  }
}

class OatWriterWrapper FINAL : public CodeOutput {
 public:
  explicit OatWriterWrapper(OatWriter* oat_writer) : oat_writer_(oat_writer) {}

  void SetCodeOffset(size_t offset) {
    oat_writer_->SetOatDataOffset(offset);
  }
  bool Write(OutputStream* out) OVERRIDE {
    return oat_writer_->Write(out);
  }
 private:
  OatWriter* const oat_writer_;
};

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
static void WriteDebugSymbols(const CompilerDriver* compiler_driver,
                              ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                                         Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>* builder,
                              OatWriter* oat_writer);

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
bool ElfWriterQuick<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
  Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>::Write(OatWriter* oat_writer,
                           const std::vector<const DexFile*>& dex_files_unused,
                           const std::string& android_root_unused,
                           bool is_host_unused) {
  constexpr bool debug = false;
  const OatHeader& oat_header = oat_writer->GetOatHeader();
  Elf_Word oat_data_size = oat_header.GetExecutableOffset();
  uint32_t oat_exec_size = oat_writer->GetSize() - oat_data_size;

  OatWriterWrapper wrapper(oat_writer);

  std::unique_ptr<ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                             Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr> > builder(
      new ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                     Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>(
          &wrapper,
          elf_file_,
          compiler_driver_->GetInstructionSet(),
          0,
          oat_data_size,
          oat_data_size,
          oat_exec_size,
          compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols(),
          debug));

  if (!builder->Init()) {
    return false;
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludeDebugSymbols()) {
    WriteDebugSymbols(compiler_driver_, builder.get(), oat_writer);
  }

  if (compiler_driver_->GetCompilerOptions().GetIncludePatchInformation()) {
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> oat_patches(
        ".oat_patches", SHT_OAT_PATCH, 0, NULL, 0, sizeof(uintptr_t), sizeof(uintptr_t));
    const std::vector<uintptr_t>& locations = oat_writer->GetAbsolutePatchLocations();
    const uint8_t* begin = reinterpret_cast<const uint8_t*>(&locations[0]);
    const uint8_t* end = begin + locations.size() * sizeof(locations[0]);
    oat_patches.GetBuffer()->assign(begin, end);
    if (debug) {
      LOG(INFO) << "Prepared .oat_patches for " << locations.size() << " patches.";
    }
    builder->RegisterRawSection(oat_patches);
  }

  return builder->Write();
}

class LineTableGenerator FINAL : public Leb128Encoder {
 public:
  LineTableGenerator(int line_base, int line_range, int opcode_base,
                     std::vector<uint8_t>* data, uintptr_t current_address,
                     size_t current_line)
    : Leb128Encoder(data), line_base_(line_base), line_range_(line_range),
      opcode_base_(opcode_base), current_address_(current_address),
      current_line_(current_line), current_file_index_(0) {}

  void PutDelta(unsigned delta_addr, int delta_line) {
    current_line_ += delta_line;
    current_address_ += delta_addr;

    if (delta_line >= line_base_ && delta_line < line_base_ + line_range_) {
      unsigned special_opcode = (delta_line - line_base_) +
                                (line_range_ * delta_addr) + opcode_base_;
      if (special_opcode <= 255) {
        PushByte(data_, special_opcode);
        return;
      }
    }

    // generate standart opcode for address advance
    if (delta_addr != 0) {
      PushByte(data_, DW_LNS_advance_pc);
      PushBackUnsigned(delta_addr);
    }

    // generate standart opcode for line delta
    if (delta_line != 0) {
      PushByte(data_, DW_LNS_advance_line);
      PushBackSigned(delta_line);
    }

    // generate standart opcode for new LTN entry
    PushByte(data_, DW_LNS_copy);
  }

  void SetAddr(uintptr_t addr) {
    if (current_address_ == addr) {
      return;
    }

    current_address_ = addr;

    PushByte(data_, 0);  // extended opcode:
    PushByte(data_, 1 + 4);  // length: opcode_size + address_size
    PushByte(data_, DW_LNE_set_address);
    PushWord(data_, addr);
  }

  void SetLine(unsigned line) {
    int delta_line = line - current_line_;
    if (delta_line) {
      current_line_ = line;
      PushByte(data_, DW_LNS_advance_line);
      PushBackSigned(delta_line);
    }
  }

  void SetFile(unsigned file_index) {
    if (current_file_index_ != file_index) {
      current_file_index_ = file_index;
      PushByte(data_, DW_LNS_set_file);
      PushBackUnsigned(file_index);
    }
  }

  void EndSequence() {
    // End of Line Table Program
    // 0(=ext), 1(len), DW_LNE_end_sequence
    PushByte(data_, 0);
    PushByte(data_, 1);
    PushByte(data_, DW_LNE_end_sequence);
  }

 private:
  const int line_base_;
  const int line_range_;
  const int opcode_base_;
  uintptr_t current_address_;
  size_t current_line_;
  unsigned current_file_index_;

  DISALLOW_COPY_AND_ASSIGN(LineTableGenerator);
};

// TODO: rewriting it using DexFile::DecodeDebugInfo needs unneeded stuff.
static void GetLineInfoForJava(const uint8_t* dbgstream, const SrcMap& pc2dex,
                               SrcMap* result, uint32_t start_pc = 0) {
  if (dbgstream == nullptr) {
    return;
  }

  int adjopcode;
  uint32_t dex_offset = 0;
  uint32_t java_line = DecodeUnsignedLeb128(&dbgstream);

  // skip parameters
  for (uint32_t param_count = DecodeUnsignedLeb128(&dbgstream); param_count != 0; --param_count) {
    DecodeUnsignedLeb128(&dbgstream);
  }

  for (bool is_end = false; is_end == false; ) {
    uint8_t opcode = *dbgstream;
    dbgstream++;
    switch (opcode) {
    case DexFile::DBG_END_SEQUENCE:
      is_end = true;
      break;

    case DexFile::DBG_ADVANCE_PC:
      dex_offset += DecodeUnsignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_ADVANCE_LINE:
      java_line += DecodeSignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_START_LOCAL:
    case DexFile::DBG_START_LOCAL_EXTENDED:
      DecodeUnsignedLeb128(&dbgstream);
      DecodeUnsignedLeb128(&dbgstream);
      DecodeUnsignedLeb128(&dbgstream);

      if (opcode == DexFile::DBG_START_LOCAL_EXTENDED) {
        DecodeUnsignedLeb128(&dbgstream);
      }
      break;

    case DexFile::DBG_END_LOCAL:
    case DexFile::DBG_RESTART_LOCAL:
      DecodeUnsignedLeb128(&dbgstream);
      break;

    case DexFile::DBG_SET_PROLOGUE_END:
    case DexFile::DBG_SET_EPILOGUE_BEGIN:
    case DexFile::DBG_SET_FILE:
      break;

    default:
      adjopcode = opcode - DexFile::DBG_FIRST_SPECIAL;
      dex_offset += adjopcode / DexFile::DBG_LINE_RANGE;
      java_line += DexFile::DBG_LINE_BASE + (adjopcode % DexFile::DBG_LINE_RANGE);

      for (SrcMap::const_iterator found = pc2dex.FindByTo(dex_offset);
          found != pc2dex.end() && found->to_ == static_cast<int32_t>(dex_offset);
          found++) {
        result->push_back({found->from_ + start_pc, static_cast<int32_t>(java_line)});
      }
      break;
    }
  }
}

/*
 * @brief Generate the DWARF debug_info and debug_abbrev sections
 * @param oat_writer The Oat file Writer.
 * @param dbg_info Compilation unit information.
 * @param dbg_abbrev Abbreviations used to generate dbg_info.
 * @param dbg_str Debug strings.
 */
static void FillInCFIInformation(OatWriter* oat_writer,
                                 std::vector<uint8_t>* dbg_info,
                                 std::vector<uint8_t>* dbg_abbrev,
                                 std::vector<uint8_t>* dbg_str,
                                 std::vector<uint8_t>* dbg_line,
                                 uint32_t text_section_offset) {
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetCFIMethodInfo();

  uint32_t producer_str_offset = PushStr(dbg_str, "Android dex2oat");

  // Create the debug_abbrev section with boilerplate information.
  // We only care about low_pc and high_pc right now for the compilation
  // unit and methods.

  // Tag 1: Compilation unit: DW_TAG_compile_unit.
  PushByte(dbg_abbrev, 1);
  PushByte(dbg_abbrev, DW_TAG_compile_unit);

  // There are children (the methods).
  PushByte(dbg_abbrev, DW_CHILDREN_yes);

  // DW_AT_producer DW_FORM_data1.
  // REVIEW: we can get rid of dbg_str section if
  // DW_FORM_string (immediate string) was used everywhere instead of
  // DW_FORM_strp (ref to string from .debug_str section).
  // DW_FORM_strp makes sense only if we reuse the strings.
  PushByte(dbg_abbrev, DW_AT_producer);
  PushByte(dbg_abbrev, DW_FORM_strp);

  // DW_LANG_Java DW_FORM_data1.
  PushByte(dbg_abbrev, DW_AT_language);
  PushByte(dbg_abbrev, DW_FORM_data1);

  // DW_AT_low_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_low_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // DW_AT_high_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_high_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  if (dbg_line != nullptr) {
    // DW_AT_stmt_list DW_FORM_sec_offset.
    PushByte(dbg_abbrev, DW_AT_stmt_list);
    PushByte(dbg_abbrev, DW_FORM_sec_offset);
  }

  // End of DW_TAG_compile_unit.
  PushHalf(dbg_abbrev, 0);

  // Tag 2: Compilation unit: DW_TAG_subprogram.
  PushByte(dbg_abbrev, 2);
  PushByte(dbg_abbrev, DW_TAG_subprogram);

  // There are no children.
  PushByte(dbg_abbrev, DW_CHILDREN_no);

  // Name of the method.
  PushByte(dbg_abbrev, DW_AT_name);
  PushByte(dbg_abbrev, DW_FORM_strp);

  // DW_AT_low_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_low_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // DW_AT_high_pc DW_FORM_addr.
  PushByte(dbg_abbrev, DW_AT_high_pc);
  PushByte(dbg_abbrev, DW_FORM_addr);

  // End of DW_TAG_subprogram.
  PushHalf(dbg_abbrev, 0);

  // Start the debug_info section with the header information
  // 'unit_length' will be filled in later.
  int cunit_length = dbg_info->size();
  PushWord(dbg_info, 0);

  // 'version' - 3.
  PushHalf(dbg_info, 3);

  // Offset into .debug_abbrev section (always 0).
  PushWord(dbg_info, 0);

  // Address size: 4.
  PushByte(dbg_info, 4);

  // Start the description for the compilation unit.
  // This uses tag 1.
  PushByte(dbg_info, 1);

  // The producer is Android dex2oat.
  PushWord(dbg_info, producer_str_offset);

  // The language is Java.
  PushByte(dbg_info, DW_LANG_Java);

  // low_pc and high_pc.
  uint32_t cunit_low_pc = 0 - 1;
  uint32_t cunit_high_pc = 0;
  int cunit_low_pc_pos = dbg_info->size();
  PushWord(dbg_info, 0);
  PushWord(dbg_info, 0);

  if (dbg_line == nullptr) {
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];

      cunit_low_pc = std::min(cunit_low_pc, dbg.low_pc_);
      cunit_high_pc = std::max(cunit_high_pc, dbg.high_pc_);

      // Start a new TAG: subroutine (2).
      PushByte(dbg_info, 2);

      // Enter name, low_pc, high_pc.
      PushWord(dbg_info, PushStr(dbg_str, dbg.method_name_));
      PushWord(dbg_info, dbg.low_pc_ + text_section_offset);
      PushWord(dbg_info, dbg.high_pc_ + text_section_offset);
    }
  } else {
    // TODO: in gdb info functions <regexp> - reports Java functions, but
    // source file is <unknown> because .debug_line is formed as one
    // compilation unit. To fix this it is possible to generate
    // a separate compilation unit for every distinct Java source.
    // Each of the these compilation units can have several non-adjacent
    // method ranges.

    // Line number table offset
    PushWord(dbg_info, dbg_line->size());

    size_t lnt_length = dbg_line->size();
    PushWord(dbg_line, 0);

    PushHalf(dbg_line, 4);  // LNT Version DWARF v4 => 4

    size_t lnt_hdr_length = dbg_line->size();
    PushWord(dbg_line, 0);  // TODO: 64-bit uses 8-byte here

    PushByte(dbg_line, 1);  // minimum_instruction_length (ubyte)
    PushByte(dbg_line, 1);  // maximum_operations_per_instruction (ubyte) = always 1
    PushByte(dbg_line, 1);  // default_is_stmt (ubyte)

    const int8_t LINE_BASE = -5;
    PushByte(dbg_line, LINE_BASE);  // line_base (sbyte)

    const uint8_t LINE_RANGE = 14;
    PushByte(dbg_line, LINE_RANGE);  // line_range (ubyte)

    const uint8_t OPCODE_BASE = 13;
    PushByte(dbg_line, OPCODE_BASE);  // opcode_base (ubyte)

    // Standard_opcode_lengths (array of ubyte).
    PushByte(dbg_line, 0); PushByte(dbg_line, 1); PushByte(dbg_line, 1);
    PushByte(dbg_line, 1); PushByte(dbg_line, 1); PushByte(dbg_line, 0);
    PushByte(dbg_line, 0); PushByte(dbg_line, 0); PushByte(dbg_line, 1);
    PushByte(dbg_line, 0); PushByte(dbg_line, 0); PushByte(dbg_line, 1);

    PushByte(dbg_line, 0);  // include_directories (sequence of path names) = EMPTY

    // File_names (sequence of file entries).
    std::unordered_map<const char*, size_t> files;
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];
      // TODO: add package directory to the file name
      const char* file_name = dbg.src_file_name_ == nullptr ? "null" : dbg.src_file_name_;
      auto found = files.find(file_name);
      if (found == files.end()) {
        size_t file_index = 1 + files.size();
        files[file_name] = file_index;
        PushStr(dbg_line, file_name);
        PushByte(dbg_line, 0);  // include directory index = LEB128(0) - no directory
        PushByte(dbg_line, 0);  // modification time = LEB128(0) - NA
        PushByte(dbg_line, 0);  // file length = LEB128(0) - NA
      }
    }
    PushByte(dbg_line, 0);  // End of file_names.

    // Set lnt header length.
    UpdateWord(dbg_line, lnt_hdr_length, dbg_line->size() - lnt_hdr_length - 4);

    // Generate Line Number Program code, one long program for all methods.
    LineTableGenerator line_table_generator(LINE_BASE, LINE_RANGE, OPCODE_BASE,
                                            dbg_line, 0, 1);

    SrcMap pc2java_map;
    for (size_t i = 0; i < method_info.size(); ++i) {
      const OatWriter::DebugInfo &dbg = method_info[i];
      const char* file_name = (dbg.src_file_name_ == nullptr) ? "null" : dbg.src_file_name_;
      size_t file_index = files[file_name];
      DCHECK_NE(file_index, 0U) << file_name;

      cunit_low_pc = std::min(cunit_low_pc, dbg.low_pc_);
      cunit_high_pc = std::max(cunit_high_pc, dbg.high_pc_);

      // Start a new TAG: subroutine (2).
      PushByte(dbg_info, 2);

      // Enter name, low_pc, high_pc.
      PushWord(dbg_info, PushStr(dbg_str, dbg.method_name_));
      PushWord(dbg_info, dbg.low_pc_ + text_section_offset);
      PushWord(dbg_info, dbg.high_pc_ + text_section_offset);

      GetLineInfoForJava(dbg.dbgstream_, dbg.compiled_method_->GetSrcMappingTable(),
                         &pc2java_map, dbg.low_pc_);
      pc2java_map.DeltaFormat({dbg.low_pc_, 1}, dbg.high_pc_);
      if (!pc2java_map.empty()) {
        line_table_generator.SetFile(file_index);
        line_table_generator.SetAddr(dbg.low_pc_ + text_section_offset);
        line_table_generator.SetLine(1);
        for (auto& src_map_elem : pc2java_map) {
          line_table_generator.PutDelta(src_map_elem.from_, src_map_elem.to_);
        }
        pc2java_map.clear();
      }
    }

    // End Sequence should have the highest address set.
    line_table_generator.SetAddr(cunit_high_pc + text_section_offset);
    line_table_generator.EndSequence();

    // set lnt length
    UpdateWord(dbg_line, lnt_length, dbg_line->size() - lnt_length - 4);
  }

  // One byte terminator
  PushByte(dbg_info, 0);

  // Fill in cunit's low_pc and high_pc.
  UpdateWord(dbg_info, cunit_low_pc_pos, cunit_low_pc + text_section_offset);
  UpdateWord(dbg_info, cunit_low_pc_pos + 4, cunit_high_pc + text_section_offset);

  // We have now walked all the methods.  Fill in lengths.
  UpdateWord(dbg_info, cunit_length, dbg_info->size() - cunit_length - 4);
}

template <typename Elf_Word, typename Elf_Sword, typename Elf_Addr,
          typename Elf_Dyn, typename Elf_Sym, typename Elf_Ehdr,
          typename Elf_Phdr, typename Elf_Shdr>
static void WriteDebugSymbols(const CompilerDriver* compiler_driver,
                              ElfBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Dyn,
                                         Elf_Sym, Elf_Ehdr, Elf_Phdr, Elf_Shdr>* builder,
                              OatWriter* oat_writer) {
  std::unique_ptr<std::vector<uint8_t>> cfi_info(
      ConstructCIEFrame(compiler_driver->GetInstructionSet()));

  Elf_Addr text_section_address = builder->GetTextBuilder().GetSection()->sh_addr;

  // Iterate over the compiled methods.
  const std::vector<OatWriter::DebugInfo>& method_info = oat_writer->GetCFIMethodInfo();
  ElfSymtabBuilder<Elf_Word, Elf_Sword, Elf_Addr, Elf_Sym, Elf_Shdr>* symtab =
      builder->GetSymtabBuilder();
  for (auto it = method_info.begin(); it != method_info.end(); ++it) {
    symtab->AddSymbol(it->method_name_, &builder->GetTextBuilder(), it->low_pc_, true,
                      it->high_pc_ - it->low_pc_, STB_GLOBAL, STT_FUNC);

    // Include CFI for compiled method, if possible.
    if (cfi_info.get() != nullptr) {
      DCHECK(it->compiled_method_ != nullptr);

      // Copy in the FDE, if present
      const std::vector<uint8_t>* fde = it->compiled_method_->GetCFIInfo();
      if (fde != nullptr) {
        // Copy the information into cfi_info and then fix the address in the new copy.
        int cur_offset = cfi_info->size();
        cfi_info->insert(cfi_info->end(), fde->begin(), fde->end());

        bool is_64bit = *(reinterpret_cast<const uint32_t*>(fde->data())) == 0xffffffff;

        // Set the 'CIE_pointer' field.
        uint64_t CIE_pointer = cur_offset + (is_64bit ? 12 : 4);
        uint64_t offset_to_update = CIE_pointer;
        if (is_64bit) {
          (*cfi_info)[offset_to_update+0] = CIE_pointer;
          (*cfi_info)[offset_to_update+1] = CIE_pointer >> 8;
          (*cfi_info)[offset_to_update+2] = CIE_pointer >> 16;
          (*cfi_info)[offset_to_update+3] = CIE_pointer >> 24;
          (*cfi_info)[offset_to_update+4] = CIE_pointer >> 32;
          (*cfi_info)[offset_to_update+5] = CIE_pointer >> 40;
          (*cfi_info)[offset_to_update+6] = CIE_pointer >> 48;
          (*cfi_info)[offset_to_update+7] = CIE_pointer >> 56;
        } else {
          (*cfi_info)[offset_to_update+0] = CIE_pointer;
          (*cfi_info)[offset_to_update+1] = CIE_pointer >> 8;
          (*cfi_info)[offset_to_update+2] = CIE_pointer >> 16;
          (*cfi_info)[offset_to_update+3] = CIE_pointer >> 24;
        }

        // Set the 'initial_location' field.
        offset_to_update += is_64bit ? 8 : 4;
        if (is_64bit) {
          const uint64_t quick_code_start = it->low_pc_ + text_section_address;
          (*cfi_info)[offset_to_update+0] = quick_code_start;
          (*cfi_info)[offset_to_update+1] = quick_code_start >> 8;
          (*cfi_info)[offset_to_update+2] = quick_code_start >> 16;
          (*cfi_info)[offset_to_update+3] = quick_code_start >> 24;
          (*cfi_info)[offset_to_update+4] = quick_code_start >> 32;
          (*cfi_info)[offset_to_update+5] = quick_code_start >> 40;
          (*cfi_info)[offset_to_update+6] = quick_code_start >> 48;
          (*cfi_info)[offset_to_update+7] = quick_code_start >> 56;
        } else {
          const uint32_t quick_code_start = it->low_pc_ + text_section_address;
          (*cfi_info)[offset_to_update+0] = quick_code_start;
          (*cfi_info)[offset_to_update+1] = quick_code_start >> 8;
          (*cfi_info)[offset_to_update+2] = quick_code_start >> 16;
          (*cfi_info)[offset_to_update+3] = quick_code_start >> 24;
        }
      }
    }
  }

  bool hasCFI = (cfi_info.get() != nullptr);
  bool hasLineInfo = false;
  for (auto& dbg_info : oat_writer->GetCFIMethodInfo()) {
    if (dbg_info.dbgstream_ != nullptr &&
        !dbg_info.compiled_method_->GetSrcMappingTable().empty()) {
      hasLineInfo = true;
      break;
    }
  }

  if (hasLineInfo || hasCFI) {
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> debug_info(".debug_info",
                                                                   SHT_PROGBITS,
                                                                   0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> debug_abbrev(".debug_abbrev",
                                                                     SHT_PROGBITS,
                                                                     0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> debug_str(".debug_str",
                                                                  SHT_PROGBITS,
                                                                  0, nullptr, 0, 1, 0);
    ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> debug_line(".debug_line",
                                                                   SHT_PROGBITS,
                                                                   0, nullptr, 0, 1, 0);

    FillInCFIInformation(oat_writer, debug_info.GetBuffer(),
                         debug_abbrev.GetBuffer(), debug_str.GetBuffer(),
                         hasLineInfo ? debug_line.GetBuffer() : nullptr,
                         text_section_address);

    builder->RegisterRawSection(debug_info);
    builder->RegisterRawSection(debug_abbrev);

    if (hasCFI) {
      ElfRawSectionBuilder<Elf_Word, Elf_Sword, Elf_Shdr> eh_frame(".eh_frame",
                                                                   SHT_PROGBITS,
                                                                   SHF_ALLOC,
                                                                   nullptr, 0, 4, 0);
      eh_frame.SetBuffer(std::move(*cfi_info.get()));
      builder->RegisterRawSection(eh_frame);
    }

    if (hasLineInfo) {
      builder->RegisterRawSection(debug_line);
    }

    builder->RegisterRawSection(debug_str);
  }
}

// Explicit instantiations
template class ElfWriterQuick<Elf32_Word, Elf32_Sword, Elf32_Addr, Elf32_Dyn,
                              Elf32_Sym, Elf32_Ehdr, Elf32_Phdr, Elf32_Shdr>;
template class ElfWriterQuick<Elf64_Word, Elf64_Sword, Elf64_Addr, Elf64_Dyn,
                              Elf64_Sym, Elf64_Ehdr, Elf64_Phdr, Elf64_Shdr>;

}  // namespace art
