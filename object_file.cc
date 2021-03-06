#include "mold.h"

#include "llvm/BinaryFormat/Magic.h"

#include <cstring>
#include <regex>

#define SHT_VERSYM 0x6fffffff
#define SHT_VERNEED 0x6ffffffe

using namespace llvm;
using namespace llvm::ELF;

ObjectFile::ObjectFile(MemoryBufferRef mb, StringRef archive_name)
  : InputFile(mb, false), archive_name(archive_name),
    is_in_archive(archive_name != "") {
  is_alive = (archive_name == "");
}

static const ELF64LE::Shdr
*find_section(ArrayRef<ELF64LE::Shdr> sections, u32 type) {
  for (const ELF64LE::Shdr &sec : sections)
    if (sec.sh_type == type)
      return &sec;
  return nullptr;
}

void ObjectFile::initialize_sections() {
  StringRef section_strtab = CHECK(obj.getSectionStringTable(elf_sections), this);

  // Read sections
  for (int i = 0; i < elf_sections.size(); i++) {
    const ELF64LE::Shdr &shdr = elf_sections[i];

    if ((shdr.sh_flags & SHF_EXCLUDE) && !(shdr.sh_flags & SHF_ALLOC))
      continue;

    switch (shdr.sh_type) {
    case SHT_GROUP: {
      // Get the signature of this section group.
      if (shdr.sh_info >= elf_syms.size())
        error(toString(this) + ": invalid symbol index");
      const ELF64LE::Sym &sym = elf_syms[shdr.sh_info];
      StringRef signature = CHECK(sym.getName(symbol_strtab), this);

      // Get comdat group members.
      ArrayRef<ELF64LE::Word> entries =
          CHECK(obj.template getSectionContentsAsArray<ELF64LE::Word>(shdr), this);
      if (entries.empty())
        error(toString(this) + ": empty SHT_GROUP");
      if (entries[0] == 0)
        continue;
      if (entries[0] != GRP_COMDAT)
        error(toString(this) + ": unsupported SHT_GROUP format");

      static ConcurrentMap<ComdatGroup> map;
      ComdatGroup *group = map.insert(signature, ComdatGroup(nullptr, 0));
      comdat_groups.push_back({group, entries});

      static Counter counter("comdats");
      counter.inc();
      break;
    }
    case SHT_SYMTAB_SHNDX:
      error(toString(this) + ": SHT_SYMTAB_SHNDX section is not supported");
      break;
    case SHT_SYMTAB:
    case SHT_STRTAB:
    case SHT_REL:
    case SHT_RELA:
    case SHT_NULL:
      break;
    default: {
      static Counter counter("regular_sections");
      counter.inc();

      StringRef name = CHECK(obj.getSectionName(shdr, section_strtab), this);
      this->sections[i] = new InputSection(this, shdr, name);
      break;
    }
    }
  }

  // Attach relocation sections to their target sections.
  for (const ELF64LE::Shdr &shdr : elf_sections) {
    if (shdr.sh_type != SHT_RELA)
      continue;

    if (shdr.sh_info >= sections.size())
      error(toString(this) + ": invalid relocated section index: " +
            Twine(shdr.sh_info));

    InputSection *target = sections[shdr.sh_info];
    if (target) {
      target->rels = CHECK(obj.relas(shdr), this);

      if (target->shdr.sh_flags & SHF_ALLOC) {
        static Counter counter("relocs_alloc");
        counter.inc(target->rels.size());
      }
    }
  }
}

void ObjectFile::initialize_symbols() {
  if (!symtab_sec)
    return;

  static Counter counter("all_syms");
  counter.inc(elf_syms.size());

  symbols.reserve(elf_syms.size());
  local_symbols.reserve(first_global);

  // First symbol entry is always null
  local_symbols.emplace_back("");
  symbols.push_back(&local_symbols.back());

  // Initialize local symbols
  for (int i = 1; i < first_global; i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    StringRef name = CHECK(esym.getName(symbol_strtab), this);

    local_symbols.emplace_back(name);
    Symbol &sym = local_symbols.back();

    sym.file = this;
    sym.type = esym.getType();
    sym.binding = esym.getBinding();
    sym.value = esym.st_value;
    sym.esym = &esym;

    if (!esym.isAbsolute()) {
      if (esym.isCommon())
        error("common local symbol?");
      sym.input_section = sections[esym.st_shndx];
    }

    symbols.push_back(&local_symbols.back());

    if (esym.getType() != STT_SECTION) {
      local_strtab_size += name.size() + 1;
      local_symtab_size += sizeof(ELF64LE::Sym);
    }
  }

  // Initialize global symbols
  for (int i = first_global; i < elf_syms.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    StringRef name = CHECK(esym.getName(symbol_strtab), this);
    int pos = name.find('@');
    if (pos != StringRef::npos)
      name = name.substr(0, pos);

    symbols.push_back(Symbol::intern(name));

    if (esym.isCommon())
      has_common_symbol = true;
  }
}

StringRef SharedFile::get_soname(ArrayRef<ELF64LE::Shdr> elf_sections) {
  const ELF64LE::Shdr *sec = find_section(elf_sections, SHT_DYNAMIC);
  if (!sec)
    return name;

  ArrayRef<ELF64LE::Dyn> tags =
    CHECK(obj.template getSectionContentsAsArray<ELF64LE::Dyn>(*sec), this);

  for (const ELF64LE::Dyn &dyn : tags)
    if (dyn.d_tag == DT_SONAME)
      return StringRef(symbol_strtab.data() + dyn.d_un.d_val);
  return name;
}

static const StringPieceRef *
binary_search(ArrayRef<StringPieceRef> pieces, u32 offset) {
  if (offset < pieces[0].input_offset)
    return nullptr;

  while (pieces.size() > 1) {
    u32 mid = pieces.size() / 2;
    const StringPieceRef &ref = pieces[mid];

    if (offset < ref.input_offset)
      pieces = pieces.slice(0, mid);
    else
      pieces = pieces.slice(mid);
  }
  return &pieces[0];
}

static bool is_mergeable(const ELF64LE::Shdr &shdr) {
  return (shdr.sh_flags & SHF_MERGE) &&
         (shdr.sh_flags & SHF_STRINGS) &&
         shdr.sh_entsize == 1;
}

void ObjectFile::initialize_mergeable_sections() {
  // Count the number of mergeable input sections.
  u32 num_mergeable = 0;

  for (InputSection *isec : sections)
    if (isec && is_mergeable(isec->shdr))
      num_mergeable++;

  mergeable_sections.reserve(num_mergeable);

  for (int i = 0; i < sections.size(); i++) {
    InputSection *isec = sections[i];
    if (isec && is_mergeable(isec->shdr)) {
      ArrayRef<u8> contents = CHECK(obj.getSectionContents(isec->shdr), this);
      mergeable_sections.emplace_back(isec, contents);
      isec->mergeable = &mergeable_sections.back();
    }
  }

  // Initialize rel_pieces
  for (InputSection *isec : sections) {
    if (!isec || isec->rels.empty())
      continue;

    isec->rel_pieces.resize(isec->rels.size());

    for (int i = 0; i < isec->rels.size(); i++) {
      const ELF64LE::Rela &rel = isec->rels[i];

      switch (rel.getType(false)) {
      case R_X86_64_64:
      case R_X86_64_PC32:
      case R_X86_64_32:
      case R_X86_64_32S:
      case R_X86_64_16:
      case R_X86_64_PC16:
      case R_X86_64_8:
      case R_X86_64_PC8:
        u32 sym_idx = rel.getSymbol(false);
        if (sym_idx >= this->first_global)
          continue;

        Symbol &sym = *symbols[sym_idx];
        if (sym.type != STT_SECTION || !sym.input_section)
          continue;

        MergeableSection *mergeable = sym.input_section->mergeable;
        if (!mergeable)
          continue;

        u32 offset = sym.value + rel.r_addend;
        const StringPieceRef *ref = binary_search(mergeable->pieces, offset);
        if (!ref)
          error(toString(this) + ": bad relocation at " + std::to_string(sym_idx));

        isec->rel_pieces[i].piece = ref->piece;
        isec->rel_pieces[i].addend = offset - ref->input_offset;
      }
    }
  }

  bool debug = (name == "setup.o");

  // Initialize sym_pieces
  sym_pieces.resize(elf_syms.size());

  for (int i = 0; i < elf_syms.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    if (esym.isAbsolute() || esym.isCommon())
      continue;

    InputSection *isec = sections[esym.st_shndx];
    if (!isec || !isec->mergeable)
      continue;

    ArrayRef<StringPieceRef> pieces = isec->mergeable->pieces;
    const StringPieceRef *ref = binary_search(pieces, esym.st_value);
    if (!ref)
      error(toString(this) + ": bad symbol value");

    if (i < first_global) {
      local_symbols[i].piece_ref = *ref;
    } else {
      sym_pieces[i].piece = ref->piece;
      sym_pieces[i].addend = esym.st_value - ref->input_offset;
    }
  }

  for (int i = 0; i < sections.size(); i++)
    if (sections[i] && sections[i]->mergeable)
      sections[i] = nullptr;
}

void ObjectFile::parse() {
  elf_sections = CHECK(obj.sections(), this);
  sections.resize(elf_sections.size());
  symtab_sec = find_section(elf_sections, SHT_SYMTAB);

  if (symtab_sec) {
    first_global = symtab_sec->sh_info;
    elf_syms = CHECK(obj.symbols(symtab_sec), this);
    symbol_strtab = CHECK(obj.getStringTableForSymtab(*symtab_sec, elf_sections), this);
  }

  initialize_sections();
  initialize_symbols();

  if (Counter::enabled) {
    static Counter defined("defined_syms");
    static Counter undefined("undefined_syms");

    for (const ELF64LE::Sym &esym : elf_syms) {
      if (esym.isDefined())
        defined.inc();
      else
        undefined.inc();
    }
  }
}

void SharedFile::parse() {
  ArrayRef<ELF64LE::Shdr> elf_sections = CHECK(obj.sections(), this);
  symtab_sec = find_section(elf_sections, SHT_DYNSYM);

  if (!symtab_sec)
    return;

  int first_global = symtab_sec->sh_info;
  ArrayRef<ELF64LE::Sym> esyms = CHECK(obj.symbols(symtab_sec), this);

  symbol_strtab = CHECK(obj.getStringTableForSymtab(*symtab_sec, elf_sections), this);
  soname = get_soname(elf_sections);

  ArrayRef<u16> versyms;
  if (const ELF64LE::Shdr *sec = find_section(elf_sections, SHT_VERSYM))
    versyms = CHECK(obj.template getSectionContentsAsArray<u16>(*sec), this);

  for (int i = first_global; i < esyms.size(); i++)
    if (versyms.empty() || (versyms[i] >> 15) == 0)
      elf_syms.push_back(esyms[i]);

  // Sort symbols by value for find_aliases(), as find_aliases() does
  // binary search on symbols.
  std::stable_sort(elf_syms.begin(), elf_syms.end(),
                   [](const ELF64LE::Sym &a, const ELF64LE::Sym &b) {
                     return a.st_value < b.st_value;
                   });

  for (ELF64LE::Sym &esym : elf_syms) {
    StringRef name = CHECK(esym.getName(symbol_strtab), this);
    symbols.push_back(Symbol::intern(name));
  }

  static Counter counter("dso_syms");
  counter.inc(elf_syms.size());
}

// Symbols with higher priorities overwrites symbols with lower priorities.
// Here is the list of priorities, from the highest to the lowest.
//
//  1. Strong defined symbol
//  2. Weak defined symbol
//  3. Defined symbol in an archive member
//  4. Unclaimed (nonexistent) symbol
//
// Ties are broken by file priority.
static u64 get_rank(InputFile *file, const ELF64LE::Sym &esym) {
  if (esym.isUndefined()) {
    assert(esym.getBinding() == STB_WEAK);
    return ((u64)2 << 32) + file->priority;
  }
  if (esym.getBinding() == STB_WEAK)
    return ((u64)1 << 32) + file->priority;
  return file->priority;
}

static u64 get_rank(const Symbol &sym) {
  if (!sym.file)
    return (u64)4 << 32;
  if (sym.is_placeholder)
    return ((u64)3 << 32) + sym.file->priority;
  return get_rank(sym.file, *sym.esym);
}

void ObjectFile::maybe_override_symbol(Symbol &sym, int symidx) {
  InputSection *isec = nullptr;
  const ELF64LE::Sym &esym = elf_syms[symidx];
  if (!esym.isAbsolute() && !esym.isCommon())
    isec = sections[esym.st_shndx];

  std::lock_guard lock(sym.mu);

  u64 new_rank = get_rank(this, esym);
  u64 existing_rank = get_rank(sym);

  if (new_rank < existing_rank) {
    sym.file = this;
    sym.input_section = isec;
    sym.piece_ref = sym_pieces[symidx];
    sym.value = esym.st_value;
    sym.type = esym.getType();
    sym.binding = esym.getBinding();
    sym.visibility = esym.getVisibility();
    sym.esym = &esym;
    sym.is_placeholder = false;
    sym.is_weak = (esym.getBinding() == STB_WEAK);
    sym.is_imported = false;

    if (UNLIKELY(sym.traced))
      message("trace: " + toString(sym.file) +
              (sym.is_weak ? ": weak definition of " : ": definition of ") +
              sym.name);
  }
}

void SharedFile::maybe_override_symbol(Symbol &sym, const ELF64LE::Sym &esym) {
  std::lock_guard lock(sym.mu);

  u64 new_rank = get_rank(this, esym);
  u64 existing_rank = get_rank(sym);

  if (new_rank < existing_rank) {
    sym.file = this;
    sym.input_section = nullptr;
    sym.piece_ref = {};
    sym.value = esym.st_value;
    sym.type = (esym.getType() == STT_GNU_IFUNC) ? STT_FUNC : esym.getType();
    sym.binding = esym.getBinding();
    sym.visibility = esym.getVisibility();
    sym.esym = &esym;
    sym.is_placeholder = false;
    sym.is_weak = (esym.getBinding() == STB_WEAK);
    sym.is_imported = true;

    if (UNLIKELY(sym.traced))
      message("trace: " + toString(sym.file) +
              (sym.is_weak ? ": weak definition of " : ": definition of ") +
              sym.name);
  }
}

void ObjectFile::resolve_symbols() {
  for (int i = first_global; i < symbols.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    if (!esym.isDefined())
      continue;

    Symbol &sym = *symbols[i];

    if (is_in_archive) {
      std::lock_guard lock(sym.mu);
      bool is_new = !sym.file;
      bool tie_but_higher_priority =
        sym.is_placeholder && this->priority < sym.file->priority;

      if (is_new || tie_but_higher_priority) {
        sym.file = this;
        sym.is_placeholder = true;

        if (UNLIKELY(sym.traced))
          message("trace: " + toString(sym.file) + ": lazy definition of " + sym.name);
      }
    } else {
      maybe_override_symbol(sym, i);
    }
  }
}

void SharedFile::resolve_symbols() {
  for (int i = 0; i < symbols.size(); i++)
    if (elf_syms[i].isDefined())
      maybe_override_symbol(*symbols[i], elf_syms[i]);
}

void
ObjectFile::mark_live_archive_members(tbb::parallel_do_feeder<ObjectFile *> &feeder) {
  assert(is_alive);

  for (int i = first_global; i < symbols.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    Symbol &sym = *symbols[i];

    if (esym.isDefined()) {
      if (is_in_archive)
        maybe_override_symbol(sym, i);
      continue;
    }

    if (UNLIKELY(sym.traced))
      message("trace: " + toString(this) + ": reference to " + sym.name);

    if (esym.getBinding() != STB_WEAK && sym.file && !sym.file->is_dso &&
        !sym.file->is_alive.exchange(true)) {
      feeder.add((ObjectFile *)sym.file);

      if (UNLIKELY(sym.traced))
        message("trace: " + toString(this) + " keeps " + toString(sym.file) +
                " for " + sym.name);
    }
  }
}

void ObjectFile::handle_undefined_weak_symbols() {
  if (!is_alive)
    return;

  for (int i = first_global; i < symbols.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    Symbol &sym = *symbols[i];

    if (esym.isUndefined() && esym.getBinding() == STB_WEAK) {
      std::lock_guard lock(sym.mu);

      bool is_new = !sym.file || sym.is_placeholder;
      bool tie_but_higher_priority =
        !is_new && sym.is_undef_weak && this->priority < sym.file->priority;

      if (is_new || tie_but_higher_priority) {
        sym.file = this;
        sym.input_section = nullptr;
        sym.value = 0;
        sym.visibility = esym.getVisibility();
        sym.esym = &esym;
        sym.is_placeholder = false;
        sym.is_undef_weak = true;
        sym.is_imported = false;

        if (UNLIKELY(sym.traced))
          message("trace: " + toString(this) + ": unresolved weak symbol " + sym.name);
      }
    }
  }
}

void ObjectFile::resolve_comdat_groups() {
  for (auto &pair : comdat_groups) {
    ComdatGroup *group = pair.first;
    ObjectFile *cur = group->file;
    while (!cur || cur->priority > this->priority)
      if (group->file.compare_exchange_strong(cur, this))
        break;
  }
}

void ObjectFile::eliminate_duplicate_comdat_groups() {
  for (auto &pair : comdat_groups) {
    ComdatGroup *group = pair.first;
    if (group->file == this)
      continue;

    ArrayRef<ELF64LE::Word> entries = pair.second;
    for (u32 i : entries)
      sections[i] = nullptr;

    static Counter counter("removed_comdat_mem");
    counter.inc(entries.size());
  }
}

void ObjectFile::convert_common_symbols() {
  if (!has_common_symbol)
    return;

  static OutputSection *bss =
    OutputSection::get_instance(".bss", SHF_WRITE | SHF_ALLOC, SHT_NOBITS);

  for (int i = first_global; i < elf_syms.size(); i++) {
    if (!elf_syms[i].isCommon())
      continue;

    Symbol *sym = symbols[i];
    if (sym->file != this)
      continue;

    auto *shdr = new ELF64LE::Shdr;
    memset(shdr, 0, sizeof(*shdr));
    shdr->sh_flags = SHF_ALLOC;
    shdr->sh_type = SHT_NOBITS;
    shdr->sh_size = elf_syms[i].st_size;
    shdr->sh_addralign = 1;

    auto *isec = new InputSection(this, *shdr, ".bss");
    isec->output_section = bss;
    sections.push_back(isec);

    sym->input_section = isec;
    sym->value = 0;
  }
}

void ObjectFile::compute_symtab() {
  for (int i = first_global; i < elf_syms.size(); i++) {
    const ELF64LE::Sym &esym = elf_syms[i];
    Symbol &sym = *symbols[i];

    if (esym.getType() != STT_SECTION && sym.file == this) {
      global_strtab_size += sym.name.size() + 1;
      global_symtab_size += sizeof(ELF64LE::Sym);
    }
  }
}

void ObjectFile::write_symtab(u64 symtab_off, u64 strtab_off, u32 start, u32 end) {
  u8 *symtab = out::buf + out::symtab->shdr.sh_offset;
  u8 *strtab = out::buf + out::strtab->shdr.sh_offset;

  for (int i = start; i < end; i++) {
    Symbol &sym = *symbols[i];
    if (sym.type == STT_SECTION || sym.file != this)
      continue;

    auto &esym = *(ELF64LE::Sym *)(symtab + symtab_off);
    symtab_off += sizeof(ELF64LE::Sym);

    esym = elf_syms[i];
    esym.st_name = strtab_off;
    esym.st_value = sym.get_addr();

    if (sym.input_section)
      esym.st_shndx = sym.input_section->output_section->shndx;
    else if (sym.shndx)
      esym.st_shndx = sym.shndx;
    else
      esym.st_shndx = SHN_ABS;

    write_string(strtab + strtab_off, sym.name);
    strtab_off += sym.name.size() + 1;
  }
}

void ObjectFile::write_local_symtab(u64 symtab_off, u64 strtab_off) {
  write_symtab(symtab_off, strtab_off, 1, first_global);
}

void ObjectFile::write_global_symtab(u64 symtab_off, u64 strtab_off) {
  write_symtab(symtab_off, strtab_off, first_global, elf_syms.size());
}

bool is_c_identifier(StringRef name) {
  static std::regex re("[a-zA-Z_][a-zA-Z0-9_]*");
  return std::regex_match(name.begin(), name.end(), re);
}

ObjectFile *ObjectFile::create_internal_file() {
  // Create a dummy object file.
  constexpr int bufsz = 256;
  char *buf = new char[bufsz];
  std::unique_ptr<MemoryBuffer> mb =
    MemoryBuffer::getMemBuffer(StringRef(buf, bufsz));

  auto *obj = new ObjectFile(mb->getMemBufferRef(), "");
  obj->name = "<internal>";
  mb.release();

  // Create linker-synthesized symbols.
  auto *elf_syms = new std::vector<ELF64LE::Sym>(1);
  obj->symbols.push_back(new Symbol(""));
  obj->first_global = 1;
  obj->is_alive = true;

  auto add = [&](StringRef name, u8 visibility = STV_DEFAULT) {
    ELF64LE::Sym esym = {};
    esym.setType(STT_NOTYPE);
    esym.st_shndx = SHN_ABS;
    esym.setBinding(STB_GLOBAL);
    esym.setVisibility(visibility);
    elf_syms->push_back(esym);

    Symbol *sym = Symbol::intern(name);
    obj->symbols.push_back(sym);
    return sym;
  };

  out::__ehdr_start = add("__ehdr_start", STV_HIDDEN);
  out::__rela_iplt_start = add("__rela_iplt_start", STV_HIDDEN);
  out::__rela_iplt_end = add("__rela_iplt_end", STV_HIDDEN);
  out::__init_array_start = add("__init_array_start", STV_HIDDEN);
  out::__init_array_end = add("__init_array_end", STV_HIDDEN);
  out::__fini_array_start = add("__fini_array_start", STV_HIDDEN);
  out::__fini_array_end = add("__fini_array_end", STV_HIDDEN);
  out::__preinit_array_start = add("__preinit_array_start", STV_HIDDEN);
  out::__preinit_array_end = add("__preinit_array_end", STV_HIDDEN);
  out::_DYNAMIC = add("_DYNAMIC", STV_HIDDEN);
  out::_GLOBAL_OFFSET_TABLE_ = add("_GLOBAL_OFFSET_TABLE_", STV_HIDDEN);
  out::__bss_start = add("__bss_start", STV_HIDDEN);
  out::_end = add("_end", STV_HIDDEN);
  out::_etext = add("_etext", STV_HIDDEN);
  out::_edata = add("_edata", STV_HIDDEN);

  for (OutputChunk *chunk : out::chunks) {
    if (!is_c_identifier(chunk->name))
      continue;

    auto *start = new std::string(("__start_" + chunk->name).str());
    auto *stop = new std::string(("__stop_" + chunk->name).str());
    add(*start, STV_HIDDEN);
    add(*stop, STV_HIDDEN);
  }

  obj->elf_syms = *elf_syms;
  obj->sym_pieces.resize(elf_syms->size());
  return obj;
}

std::string toString(InputFile *file) {
  if (file->is_dso)
    return file->name;

  ObjectFile *obj = (ObjectFile *)file;
  if (obj->archive_name == "")
    return obj->name;
  return (obj->archive_name + ":" + obj->name).str();
}

ArrayRef<Symbol *> SharedFile::find_aliases(Symbol *sym) {
  assert(sym->file == this);
  auto [begin, end] = std::equal_range(
    symbols.begin(), symbols.end(), sym,
    [&](Symbol *a, Symbol *b) { return a->value < b->value; });
  return ArrayRef<Symbol *>(&*begin, end - begin);
}
