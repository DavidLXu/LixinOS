// QEMU virt loads an ELF at p_paddr. Give each allocated section its own
// segment so BSS costs no disk space and read-only data stays non-executable.
static int link_kernel(TCCState *s, uint64_t *descriptor, int loader) {
    // The bootstrap runs above the kernel and user image so it can load a
    // replacement into the normal kernel address range without overwriting itself.
    const size_t base = loader ? 0x44400000 : 0x40200000;
    const size_t limit = loader ? 0x45000000 : 0x44000000;
    const size_t page = 4096;
    size_t address = base;
    resolve_common_syms(s);
    build_got_entries(s, 0);
    unsigned count = 0;
    for (int i = 1; i < s->nb_sections; i++) {
        Section *sec = s->sections[i];
        if ((sec->sh_flags & SHF_ALLOC) && sec->data_offset)
            count++;
    }
    if (!count || count >= 65535) {
        puts("tcc: invalid kernel segment count");
        return 1;
    }
    size_t header_size = sizeof(Elf64_Ehdr) + count * sizeof(Elf64_Phdr);
    Elf64_Ehdr *header = tcc_mallocz(header_size);
    Elf64_Phdr *segments = (Elf64_Phdr *)(header + 1);
    struct image_parts *out = tcc_malloc(sizeof(*out) + (1 + count) * sizeof(struct file_part));
    out->count = 1;
    out->parts[0] = (struct file_part){0, (uintptr_t)header, header_size};
    size_t file_size = header_size;
    unsigned index = 0;
    for (int i = 1; i < s->nb_sections; i++) {
        Section *sec = s->sections[i];
        if (!(sec->sh_flags & SHF_ALLOC) || !sec->data_offset)
            continue;
        size_t align = sec->sh_addralign > page ? sec->sh_addralign : page;
        if (align > limit - base || (align & (align - 1))) {
            puts("tcc: invalid kernel alignment");
            return 1;
        }
        address = (address + align - 1) & -align;
        if (address > limit || sec->data_offset > limit - address) {
            puts("tcc: kernel exceeds physical load region");
            return 1;
        }
        sec->sh_addr = address;
        Elf64_Phdr *ph = &segments[index++];
        ph->p_type = PT_LOAD;
        ph->p_flags = PF_R | ((sec->sh_flags & SHF_EXECINSTR) ? PF_X : 0) |
                      ((sec->sh_flags & SHF_WRITE) ? PF_W : 0);
        ph->p_align = align;
        ph->p_paddr = ph->p_vaddr = address;
        ph->p_memsz = sec->data_offset;
        if (sec->sh_type != SHT_NOBITS) {
            size_t offset = (file_size + align - 1) & -align;
            if (offset > IMAGE_LIMIT || sec->data_offset > IMAGE_LIMIT - offset) {
                puts("tcc: kernel exceeds guest file limit");
                return 1;
            }
            ph->p_offset = offset;
            ph->p_filesz = sec->data_offset;
            file_size = offset + ph->p_filesz;
            out->parts[out->count++] =
                (struct file_part){offset, (uintptr_t)sec->data, ph->p_filesz};
        }
        address += ph->p_memsz;
    }
    relocate_syms(s, s->symtab, 1);
    if (s->nb_errors)
        return 1;
    relocate_plt(s);
    relocate_sections(s);
    if (s->nb_errors)
        return 1;
    addr_t entry = get_sym_addr(s, "_start", 1, 1);
    unsigned executable = 0;
    for (unsigned i = 0; i < count; i++) {
        Elf64_Phdr *ph = &segments[i];
        if ((ph->p_flags & PF_X) && entry >= ph->p_vaddr &&
            entry - ph->p_vaddr + 4 <= ph->p_filesz && !(entry & 3))
            executable = 1;
    }
    if (!executable || s->nb_errors) {
        puts("tcc: invalid kernel entry _start");
        return 1;
    }
    memcpy(header->e_ident, "\177ELF", 4);
    header->e_ident[EI_CLASS] = ELFCLASS64;
    header->e_ident[EI_DATA] = ELFDATA2LSB;
    header->e_ident[EI_VERSION] = EV_CURRENT;
    header->e_type = ET_EXEC;
    header->e_machine = EM_AARCH64;
    header->e_version = EV_CURRENT;
    header->e_entry = entry;
    header->e_ehsize = sizeof(*header);
    header->e_phoff = sizeof(*header);
    header->e_phentsize = sizeof(*segments);
    header->e_phnum = count;
    *descriptor = ((uint64_t)(uintptr_t)out << 32) | file_size;
    return 0;
}
