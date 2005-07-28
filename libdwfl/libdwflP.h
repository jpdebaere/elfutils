/* Internal definitions for libdwfl.
   Copyright (C) 2005 Red Hat, Inc.

   This program is Open Source software; you can redistribute it and/or
   modify it under the terms of the Open Software License version 1.0 as
   published by the Open Source Initiative.

   You should have received a copy of the Open Software License along
   with this program; if not, you may obtain a copy of the Open Software
   License version 1.0 from http://www.opensource.org/licenses/osl.php or
   by writing the Open Source Initiative c/o Lawrence Rosen, Esq.,
   3001 King Ranch Road, Ukiah, CA 95482.   */

#ifndef _LIBDWFLP_H
#define _LIBDWFLP_H	1

#ifndef PACKAGE
# include <config.h>
#endif
#include <libdwfl.h>
#include <libebl.h>
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* gettext helper macros.  */
#define _(Str) dgettext ("elfutils", Str)

#define DWFL_ERRORS							      \
  DWFL_ERROR (NOERROR, N_("no error"))					      \
  DWFL_ERROR (UNKNOWN_ERROR, N_("unknown error"))			      \
  DWFL_ERROR (NOMEM, N_("out of memory"))				      \
  DWFL_ERROR (ERRNO, N_("See errno"))					      \
  DWFL_ERROR (LIBELF, N_("See elf_errno"))				      \
  DWFL_ERROR (LIBDW, N_("See dwarf_errno"))				      \
  DWFL_ERROR (LIBEBL, N_("See ebl_errno (XXX missing)"))		      \
  DWFL_ERROR (NOREL, N_("Callbacks missing for ET_REL file"))		      \
  DWFL_ERROR (BADRELTYPE, N_("Unsupported relocation type"))		      \
  DWFL_ERROR (BADRELOFF, N_("r_offset is bogus"))			      \
  DWFL_ERROR (BADSTROFF, N_("offset out of range"))			      \
  DWFL_ERROR (RELUNDEF, N_("relocation refers to undefined symbol"))	      \
  DWFL_ERROR (CB, N_("Callback returned failure"))			      \
  DWFL_ERROR (NO_DWARF, N_("No DWARF information found"))		      \
  DWFL_ERROR (NO_SYMTAB, N_("No symbol table found"))			      \
  DWFL_ERROR (NO_PHDR, N_("No ELF program headers"))			      \
  DWFL_ERROR (OVERLAP, N_("address range overlaps an existing module"))	      \
  DWFL_ERROR (ADDR_OUTOFRANGE, N_("address out of range"))		      \
  DWFL_ERROR (NO_MATCH, N_("no matching address range"))		      \
  DWFL_ERROR (TRUNCATED, N_("image truncated"))				      \
  DWFL_ERROR (BADELF, N_("not a valid ELF file"))

#define DWFL_ERROR(name, text) DWFL_E_##name,
typedef enum { DWFL_ERRORS DWFL_E_NUM } Dwfl_Error;
#undef	DWFL_ERROR

#define OTHER_ERROR(name)	((unsigned int) DWFL_E_##name << 16)
#define DWFL_E(name, errno)	(OTHER_ERROR (name) | (errno))

extern int __libdwfl_canon_error (Dwfl_Error error) internal_function;
extern void __libdwfl_seterrno (Dwfl_Error error) internal_function;

struct Dwfl
{
  const Dwfl_Callbacks *callbacks;

  Dwfl_Module *modulelist;    /* List in order used by full traversals.  */

  Dwfl_Module **modules;
  size_t nmodules;
};

struct dwfl_file
{
  char *name;
  int fd;

  Elf *elf;
  GElf_Addr bias;		/* Actual load address - p_vaddr.  */
};

struct Dwfl_Module
{
  Dwfl *dwfl;
  struct Dwfl_Module *next;	/* Link on Dwfl.moduelist.  */

  void *userdata;

  char *name;			/* Iterator name for this module.  */
  GElf_Addr low_addr, high_addr;

  struct dwfl_file main, debug;
  Ebl *ebl;
  bool isrel;			/* True iff this is an ET_REL file.  */
  Dwfl_Error elferr;		/* Previous failure to open main file.  */

  struct dwfl_file *symfile;	/* Either main or debug.  */
  Elf_Data *symdata;		/* Data in the ELF symbol table section.  */
  size_t syments;		/* sh_size / sh_entsize of that section.  */
  const Elf_Data *symstrdata;	/* Data for its string table.  */
  Elf_Data *symxndxdata;	/* Data in the extended section index table. */
  Dwfl_Error symerr;		/* Previous failure to load symbols.  */

  Dwarf *dw;			/* libdw handle for its debugging info.  */
  Dwfl_Error dwerr;		/* Previous failure to load info.  */

  /* Known CU's in this module.  */
  struct dwfl_cu *first_cu, **cu;
  unsigned int ncu;

  void *lazy_cu_root;		/* Table indexed by Dwarf_Off of CU.  */
  unsigned int lazycu;		/* Possible users, deleted when none left.  */

  struct dwfl_arange *aranges;	/* Mapping of addresses in module to CUs.  */
  unsigned int naranges;

  bool gc;			/* Mark/sweep flag.  */
};



/* Information cached about each CU in Dwfl_Module.dw.  */
struct dwfl_cu
{
  /* This caches libdw information about the CU.  It's also the
     address passed back to users, so we take advantage of the
     fact that it's placed first to cast back.  */
  Dwarf_Die die;

  Dwfl_Module *mod;		/* Pointer back to containing module.  */

  struct dwfl_cu *next;		/* CU immediately following in the file.  */

  struct Dwfl_Lines *lines;
};

struct Dwfl_Lines
{
  struct dwfl_cu *cu;

  /* This is what the opaque Dwfl_Line * pointers we pass to users are.
     We need to recover pointers to our struct dwfl_cu and a record in
     libdw's Dwarf_Line table.  To minimize the memory used in addition
     to libdw's Dwarf_Lines buffer, we just point to our own index in
     this table, and have one pointer back to the CU.  The indices here
     match those in libdw's Dwarf_CU.lines->info table.  */
  struct Dwfl_Line
  {
    unsigned int idx;		/* My index in the dwfl_cu.lines table.  */
  } idx[0];
};

static inline struct dwfl_cu *
dwfl_linecu (const Dwfl_Line *line)
{
  const struct Dwfl_Lines *lines = ((const void *) line
				    - offsetof (struct Dwfl_Lines,
						idx[line->idx]));
  return lines->cu;
}

/* This describes a contiguous address range that lies in a single CU.
   We condense runs of Dwarf_Arange entries for the same CU into this.  */
struct dwfl_arange
{
  struct dwfl_cu *cu;
  size_t arange;		/* Index in Dwarf_Aranges.  */
};




extern void __libdwfl_module_free (Dwfl_Module *mod) internal_function;


/* Process relocations in debugging sections in an ET_REL file.
   MOD->debug.elf must be opened with ELF_C_READ_MMAP_PRIVATE or ELF_C_READ,
   to make it possible to relocate the data in place (or ELF_C_RDWR or
   ELF_C_RDWR_MMAP if you intend to modify the Elf file on disk).  After
   this, dwarf_begin_elf on MOD->debug.elf will read the relocated data.  */
extern Dwfl_Error __libdwfl_relocate (Dwfl_Module *) internal_function;

/* Adjust *VALUE from section-relative to absolute.
   MOD->dwfl->callbacks->section_address is called to determine the actual
   address of a loaded section.  */
extern Dwfl_Error __libdwfl_relocate_value (Dwfl_Module *mod,
					    size_t m_shstrndx,
					    Elf32_Word shndx,
					    GElf_Addr *value)
     internal_function;

/* Iterate through all the CU's in the module.  Start by passing a null
   LASTCU, and then pass the last *CU returned.  Success return with null
   *CU no more CUs.  */
extern Dwfl_Error __libdwfl_nextcu (Dwfl_Module *mod, struct dwfl_cu *lastcu,
				    struct dwfl_cu **cu) internal_function;

/* Find the CU by address.  */
extern Dwfl_Error __libdwfl_addrcu (Dwfl_Module *mod, Dwarf_Addr addr,
				    struct dwfl_cu **cu) internal_function;

/* Ensure that CU->lines (and CU->cu->lines) is set up.  */
extern Dwfl_Error __libdwfl_cu_getsrclines (struct dwfl_cu *cu)
     internal_function;




/* Avoid PLT entries.  */
INTDECL (dwfl_begin)
INTDECL (dwfl_errmsg)
INTDECL (dwfl_addrmodule)
INTDECL (dwfl_addrdwarf)
INTDECL (dwfl_addrdie)
INTDECL (dwfl_module_addrdie)
INTDECL (dwfl_module_getdwarf)
INTDECL (dwfl_module_getelf)
INTDECL (dwfl_module_getsrc)
INTDECL (dwfl_report_elf)
INTDECL (dwfl_report_begin)
INTDECL (dwfl_report_module)
INTDECL (dwfl_report_end)
INTDECL (dwfl_standard_find_debuginfo)
INTDECL (dwfl_linux_kernel_find_elf)
INTDECL (dwfl_linux_kernel_module_section_address)
INTDECL (dwfl_linux_proc_report)
INTDECL (dwfl_linux_proc_find_elf)
INTDECL (dwfl_linux_kernel_report_kernel)
INTDECL (dwfl_linux_kernel_report_modules)

/* Leading arguments standard to callbacks passed a Dwfl_Module.  */
#define MODCB_ARGS(mod)	(mod), &(mod)->userdata, (mod)->name, (mod)->low_addr
#define CBFAIL		(errno ? DWFL_E (ERRNO, errno) : DWFL_E_CB);


#endif	/* libdwflP.h */