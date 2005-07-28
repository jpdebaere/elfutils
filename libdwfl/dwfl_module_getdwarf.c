/* Find debugging and symbol information for a module in libdwfl.
   Copyright (C) 2005 Red Hat, Inc.

   This program is Open Source software; you can redistribute it and/or
   modify it under the terms of the Open Software License version 1.0 as
   published by the Open Source Initiative.

   You should have received a copy of the Open Software License along
   with this program; if not, you may obtain a copy of the Open Software
   License version 1.0 from http://www.opensource.org/licenses/osl.php or
   by writing the Open Source Initiative c/o Lawrence Rosen, Esq.,
   3001 King Ranch Road, Ukiah, CA 95482.   */

#include "libdwflP.h"
#include <fcntl.h>
#include <string.h>
#include "../libdw/libdwP.h"	/* DWARF_E_* values are here.  */


/* Open libelf FILE->fd and compute the load base of ELF as loaded in MOD.
   When we return success, FILE->elf and FILE->bias are set up.  */
static inline Dwfl_Error
open_elf (Dwfl_Module *mod, struct dwfl_file *file)
{
  if (file->elf == NULL)
    {
      if (file->fd < 0)
	return CBFAIL;

      file->elf = elf_begin (file->fd, ELF_C_READ_MMAP_PRIVATE, NULL);
    }

  GElf_Ehdr ehdr_mem, *ehdr = gelf_getehdr (file->elf, &ehdr_mem);
  if (ehdr == NULL)
    return DWFL_E (LIBELF, elf_errno ());

  mod->isrel = ehdr->e_type == ET_REL;

  file->bias = 0;
  for (uint_fast16_t i = 0; i < ehdr->e_phnum; ++i)
    {
      GElf_Phdr ph_mem;
      GElf_Phdr *ph = gelf_getphdr (file->elf, i, &ph_mem);
      if (ph == NULL)
	return DWFL_E_LIBELF;
      if (ph->p_type == PT_LOAD)
	{
	  file->bias = ((mod->low_addr & -ph->p_align)
			- (ph->p_vaddr & -ph->p_align));
	  break;
	}
    }

  return DWFL_E_NOERROR;
}

/* Find the main ELF file for this module and open libelf on it.
   When we return success, MOD->main.elf and MOD->main.bias are set up.  */
static void
find_file (Dwfl_Module *mod)
{
  if (mod->main.elf != NULL	/* Already done.  */
      || mod->elferr != DWFL_E_NOERROR)	/* Cached failure.  */
    return;

  mod->main.fd = (*mod->dwfl->callbacks->find_elf) (MODCB_ARGS (mod),
						    &mod->main.name,
						    &mod->main.elf);
  mod->elferr = open_elf (mod, &mod->main);
}

/* Find the separate debuginfo file for this module and open libelf on it.
   When we return success, MOD->debug is set up.  */
static Dwfl_Error
find_debuginfo (Dwfl_Module *mod)
{
  size_t shstrndx;
  if (elf_getshstrndx (mod->main.elf, &shstrndx) < 0)
    return DWFL_E_LIBELF;

  Elf_Scn *scn = elf_getscn (mod->main.elf, 0);
  if (scn == NULL)
    return DWFL_E_LIBELF;
  do
    {
      GElf_Shdr shdr_mem, *shdr = gelf_getshdr (scn, &shdr_mem);
      if (shdr == NULL)
	return DWFL_E_LIBELF;

      const char *name = elf_strptr (mod->main.elf, shstrndx, shdr->sh_name);
      if (name == NULL)
	return DWFL_E_LIBELF;

      if (!strcmp (name, ".gnu_debuglink"))
	break;

      scn = elf_nextscn (mod->main.elf, scn);
    } while (scn != NULL);

  const char *debuglink_file = NULL;
  GElf_Word debuglink_crc = 0;
  if (scn != NULL)
    {
      /* Found the .gnu_debuglink section.  Extract its contents.  */
      Elf_Data *rawdata = elf_rawdata (scn, NULL);
      if (rawdata == NULL)
	return DWFL_E_LIBELF;

      Elf_Data crcdata =
	{
	  .d_type = ELF_T_WORD,
	  .d_buf = &debuglink_crc,
	  .d_size = sizeof debuglink_crc,
	  .d_version = EV_CURRENT,
	};
      Elf_Data conv =
	{
	  .d_type = ELF_T_WORD,
	  .d_buf = rawdata->d_buf + rawdata->d_size - sizeof debuglink_crc,
	  .d_size = sizeof debuglink_crc,
	  .d_version = EV_CURRENT,
	};

      GElf_Ehdr ehdr_mem;
      GElf_Ehdr *ehdr = gelf_getehdr (mod->main.elf, &ehdr_mem);
      if (ehdr == NULL)
	return DWFL_E_LIBELF;

      Elf_Data *d = gelf_xlatetom (mod->main.elf, &crcdata, &conv,
				   ehdr->e_ident[EI_DATA]);
      if (d == NULL)
	return DWFL_E_LIBELF;
      assert (d == &crcdata);

      debuglink_file = rawdata->d_buf;
    }

  mod->debug.fd = (*mod->dwfl->callbacks->find_debuginfo) (MODCB_ARGS (mod),
							   mod->main.name,
							   debuglink_file,
							   debuglink_crc,
							   &mod->debug.name);
  return open_elf (mod, &mod->debug);
}


/* Try to find a symbol table in FILE.  */
static Dwfl_Error
load_symtab (struct dwfl_file *file, struct dwfl_file **symfile,
	     Elf_Scn **symscn, Elf_Scn **xndxscn,
	     size_t *syments, GElf_Word *strshndx)
{
  Elf_Scn *scn = NULL;
  while ((scn = elf_nextscn (file->elf, scn)) != NULL)
    {
      GElf_Shdr shdr_mem, *shdr = gelf_getshdr (scn, &shdr_mem);
      if (shdr != NULL)
	switch (shdr->sh_type)
	  {
	  case SHT_SYMTAB:
	    *symscn = scn;
	    *symfile = file;
	    *strshndx = shdr->sh_link;
	    *syments = shdr->sh_size / shdr->sh_entsize;
	    if (*symscn != NULL && *xndxscn != NULL)
	      return DWFL_E_NOERROR;
	    break;

	  case SHT_DYNSYM:
	    /* Use this if need be, but keep looking for SHT_SYMTAB.  */
	    *symscn = scn;
	    *symfile = file;
	    *strshndx = shdr->sh_link;
	    *syments = shdr->sh_size / shdr->sh_entsize;
	    break;

	  case SHT_SYMTAB_SHNDX:
	    *xndxscn = scn;
	    break;

	  default:
	    break;
	  }
    }
  return DWFL_E_NO_SYMTAB;
}

/* Try to find a symbol table in either MOD->main.elf or MOD->debug.elf.  */
static void
find_symtab (Dwfl_Module *mod)
{
  if (mod->symdata != NULL	/* Already done.  */
      || mod->symerr != DWFL_E_NOERROR) /* Cached previous failure.  */
    return;

  find_file (mod);
  mod->symerr = mod->elferr;
  if (mod->symerr != DWFL_E_NOERROR)
    return;

  /* First see if the main ELF file has the debugging information.  */
  Elf_Scn *symscn = NULL, *xndxscn = NULL;
  GElf_Word strshndx;
  mod->symerr = load_symtab (&mod->main, &mod->symfile, &symscn,
			     &xndxscn, &mod->syments, &strshndx);
  switch (mod->symerr)
    {
    default:
      return;

    case DWFL_E_NOERROR:
      break;

    case DWFL_E_NO_SYMTAB:
      /* Now we have to look for a separate debuginfo file.  */
      mod->symerr = find_debuginfo (mod);
      switch (mod->symerr)
	{
	default:
	  return;

	case DWFL_E_NOERROR:
	  mod->symerr = load_symtab (&mod->debug, &mod->symfile, &symscn,
				     &xndxscn, &mod->syments, &strshndx);
	  break;

	case DWFL_E_CB:		/* The find_debuginfo hook failed.  */
	  mod->symerr = DWFL_E_NO_SYMTAB;
	  break;
	}

      switch (mod->symerr)
	{
	default:
	  return;

	case DWFL_E_NOERROR:
	  break;

	case DWFL_E_NO_SYMTAB:
	  if (symscn == NULL)
	    return;
	  /* We still have the dynamic symbol table.  */
	  mod->symerr = DWFL_E_NOERROR;
	  break;
	}
      break;
    }

  /* This does some sanity checks on the string table section.  */
  if (elf_strptr (mod->symfile->elf, strshndx, 0) == NULL)
    {
    elferr:
      mod->symerr = DWFL_E (LIBELF, elf_errno ());
      return;
    }

  /* Cache the data; MOD->syments was set above.  */

  mod->symstrdata = elf_rawdata (elf_getscn (mod->symfile->elf, strshndx),
				 NULL);
  if (mod->symstrdata == NULL)
    goto elferr;

  if (xndxscn == NULL)
    mod->symxndxdata = NULL;
  else
    {
      mod->symxndxdata = elf_rawdata (xndxscn, NULL);
      if (mod->symxndxdata == NULL)
	goto elferr;
    }

  mod->symdata = elf_rawdata (symscn, NULL);
  if (mod->symdata == NULL)
    goto elferr;
}


/* Try to start up libdw on DEBUGFILE.  */
static Dwfl_Error
load_dw (Dwfl_Module *mod, Elf *debugfile)
{
  if (mod->isrel)
    {
      const Dwfl_Callbacks *const cb = mod->dwfl->callbacks;

      /* The debugging sections have to be relocated.  */
      if (cb->section_address == NULL)
	return DWFL_E_NOREL;

      if (mod->ebl == NULL)
	{
	  mod->ebl = ebl_openbackend (mod->main.elf);
	  if (mod->ebl == NULL)
	    return DWFL_E_LIBEBL;
	}

      find_symtab (mod);
      Dwfl_Error result = mod->symerr;
      if (result == DWFL_E_NOERROR)
	result = __libdwfl_relocate (mod);
      if (result != DWFL_E_NOERROR)
	return result;
    }

  mod->dw = dwarf_begin_elf (debugfile, DWARF_C_READ, NULL);
  if (mod->dw == NULL)
    {
      int err = dwarf_errno ();
      return err == DWARF_E_NO_DWARF ? DWFL_E_NO_DWARF : DWFL_E (LIBDW, err);
    }

  /* Until we have iterated through all CU's, we might do lazy lookups.  */
  mod->lazycu = 1;

  return DWFL_E_NOERROR;
}

/* Try to start up libdw on either the main file or the debuginfo file.  */
static void
find_dw (Dwfl_Module *mod)
{
  if (mod->dw != NULL		/* Already done.  */
      || mod->dwerr != DWFL_E_NOERROR) /* Cached previous failure.  */
    return;

  find_file (mod);
  mod->dwerr = mod->elferr;
  if (mod->dwerr != DWFL_E_NOERROR)
    return;

  /* First see if the main ELF file has the debugging information.  */
  mod->dwerr = load_dw (mod, mod->main.elf);
  switch (mod->dwerr)
    {
    case DWFL_E_NOERROR:
      mod->debug.elf = mod->main.elf;
      mod->debug.bias = mod->main.bias;
      return;

    case DWFL_E_NO_DWARF:
      break;

    default:
      goto canonicalize;
    }

  /* Now we have to look for a separate debuginfo file.  */
  mod->dwerr = find_debuginfo (mod);
  switch (mod->dwerr)
    {
    case DWFL_E_NOERROR:
      mod->dwerr = load_dw (mod, mod->debug.elf);
      break;

    case DWFL_E_CB:		/* The find_debuginfo hook failed.  */
      mod->dwerr = DWFL_E_NO_DWARF;
      return;

    default:
      break;
    }

 canonicalize:
  mod->dwerr = __libdwfl_canon_error (mod->dwerr);
}


Elf *
dwfl_module_getelf (Dwfl_Module *mod, GElf_Addr *loadbase)
{
  if (mod == NULL)
    return NULL;

  find_file (mod);
  if (mod->elferr == DWFL_E_NOERROR)
    {
      *loadbase = mod->main.bias;
      return mod->main.elf;
    }

  __libdwfl_seterrno (mod->elferr);
  return NULL;
}
INTDEF (dwfl_module_getelf)


Dwarf *
dwfl_module_getdwarf (Dwfl_Module *mod, Dwarf_Addr *bias)
{
  if (mod == NULL)
    return NULL;

  find_dw (mod);
  if (mod->dwerr == DWFL_E_NOERROR)
    {
      *bias = mod->debug.bias;
      return mod->dw;
    }

  __libdwfl_seterrno (mod->dwerr);
  return NULL;
}
INTDEF (dwfl_module_getdwarf)


const char *
dwfl_module_addrname (Dwfl_Module *mod, GElf_Addr addr)
{
  if (mod == NULL)
    return NULL;

  find_symtab (mod);
  if (mod->symerr != DWFL_E_NOERROR)
    {
      __libdwfl_seterrno (mod->symerr);
      return NULL;
    }

  addr -= mod->symfile->bias;

  /* Look through the symbol table for a matching symbol.  */
  size_t symshstrndx = SHN_UNDEF;
  for (size_t i = 1; i < mod->syments; ++i)
    {
      GElf_Sym sym_mem;
      GElf_Word shndx;
      GElf_Sym *sym = gelf_getsymshndx (mod->symdata, mod->symxndxdata,
					i, &sym_mem, &shndx);
      if (sym != NULL)
	{
	  GElf_Addr symaddr = sym->st_value;

	  if (sym->st_shndx != SHN_XINDEX)
	    shndx = sym->st_shndx;

	  if (mod->isrel)
	    /* In an ET_REL file, the symbol table values are relative
	       to the section, not to the module's load base.  */
	    switch (shndx)
	      {
	      case SHN_UNDEF:	/* Undefined symbol can't match an address.  */
	      case SHN_COMMON:	/* Nor can a common defn.  */
		continue;

	      case SHN_ABS:	/* Symbol value is already absolute.  */
		break;

	      default:
		{
		  Dwfl_Error result = DWFL_E_LIBELF;
		  if (likely (symshstrndx != SHN_UNDEF)
		      || elf_getshstrndx (mod->symfile->elf,
					  &symshstrndx) == 0)
		    result = __libdwfl_relocate_value (mod, symshstrndx,
						       shndx, &symaddr);
		  if (unlikely (result != DWFL_E_NOERROR))
		    {
		      __libdwfl_seterrno (result);
		      return NULL;
		    }
		  break;
		}
	      }

	  if (symaddr <= addr && addr < symaddr + sym->st_size)
	    {
	      if (unlikely (sym->st_name >= mod->symstrdata->d_size))
		{
		  __libdwfl_seterrno (DWFL_E_BADSTROFF);
		  return NULL;
		}
	      return (const char *) mod->symstrdata->d_buf + sym->st_name;
	    }
	}
    }

  return NULL;
}