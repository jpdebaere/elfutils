/* Relocate debug information.
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

typedef uint8_t GElf_Byte;

/* Adjust *VALUE to add the load address of the SHNDX section.
   We update the section header in place to cache the result.  */

Dwfl_Error
internal_function_def
__libdwfl_relocate_value (Dwfl_Module *mod, size_t symshstrndx,
			  Elf32_Word shndx, GElf_Addr *value)
{
  Elf_Scn *refscn = elf_getscn (mod->symfile->elf, shndx);
  GElf_Shdr refshdr_mem, *refshdr = gelf_getshdr (refscn, &refshdr_mem);
  if (refshdr == NULL)
    return DWFL_E_LIBELF;

  if ((refshdr->sh_flags & SHF_ALLOC) && refshdr->sh_addr == 0)
    {
      /* This is a loaded section.  Find its actual
	 address and update the section header.  */
      const char *name = elf_strptr (mod->symfile->elf, symshstrndx,
				     refshdr->sh_name);
      if (name == NULL)
	return DWFL_E_LIBELF;

      if ((*mod->dwfl->callbacks->section_address) (MODCB_ARGS (mod), name,
						    &refshdr->sh_addr))
	return CBFAIL;

      if (refshdr->sh_addr == 0)
	/* The callback resolved this to zero, indicating it wasn't
	   really loaded but we don't really care.  Mark it so we
	   don't check it again for the next relocation.  */
	refshdr->sh_flags &= ~SHF_ALLOC;

      /* Update the in-core file's section header to show the final
	 load address (or unloadedness).  This serves as a cache,
	 so we won't get here again for the same section.  */
      if (! gelf_update_shdr (refscn, refshdr))
	return DWFL_E_LIBELF;
    }

  /* Apply the adjustment.  */
  *value += refshdr->sh_addr;
  return DWFL_E_NOERROR;
}

Dwfl_Error
internal_function_def
__libdwfl_relocate (Dwfl_Module *mod)
{
  assert (mod->isrel);

  GElf_Ehdr ehdr_mem;
  const GElf_Ehdr *ehdr = gelf_getehdr (mod->debug.elf, &ehdr_mem);
  if (ehdr == NULL)
    return DWFL_E_LIBELF;

  size_t symshstrndx, d_shstrndx;
  if (elf_getshstrndx (mod->symfile->elf, &symshstrndx) < 0)
    return DWFL_E_LIBELF;
  if (mod->symfile == &mod->debug)
    d_shstrndx = symshstrndx;
  else if (elf_getshstrndx (mod->debug.elf, &d_shstrndx) < 0)
    return DWFL_E_LIBELF;

  /* Look at each section in the debuginfo file, and process the
     relocation sections for debugging sections.  */
  Dwfl_Error result = DWFL_E_NO_DWARF;
  Elf_Scn *scn = NULL;
  while ((scn = elf_nextscn (mod->debug.elf, scn)) != NULL)
    {
      GElf_Shdr shdr_mem;
      GElf_Shdr *shdr = gelf_getshdr (scn, &shdr_mem);

      if (shdr->sh_type == SHT_REL || shdr->sh_type == SHT_RELA)
	{
	  /* It's a relocation section.  First, fetch the name of the
	     section these relocations apply to.  */

	  Elf_Scn *tscn = elf_getscn (mod->debug.elf, shdr->sh_info);
	  if (tscn == NULL)
	    return DWFL_E_LIBELF;

	  GElf_Shdr tshdr_mem;
	  GElf_Shdr *tshdr = gelf_getshdr (tscn, &tshdr_mem);
	  const char *tname = elf_strptr (mod->debug.elf, d_shstrndx,
					  tshdr->sh_name);
	  if (tname == NULL)
	    return DWFL_E_LIBELF;

	  if (! ebl_debugscn_p (mod->ebl, tname))
	    /* This relocation section is not for a debugging section.
	       Nothing to do here.  */
	    continue;

	  /* Fetch the section data that needs the relocations applied.  */
	  Elf_Data *tdata = elf_rawdata (tscn, NULL);
	  if (tdata == NULL)
	    return DWFL_E_LIBELF;

	  /* Apply one relocation.  Returns true for any invalid data.  */
	  Dwfl_Error relocate (GElf_Addr offset, const GElf_Sxword *addend,
			       int rtype, int symndx)
	    {
	      /* First, resolve the symbol to an absolute value.  */
	      GElf_Addr value;
	      inline Dwfl_Error adjust (GElf_Word shndx)
		{
		  return __libdwfl_relocate_value (mod, symshstrndx,
						   shndx, &value);
		}

	      if (symndx == STN_UNDEF)
		/* When strip removes a section symbol referring to a
		   section moved into the debuginfo file, it replaces
		   that symbol index in relocs with STN_UNDEF.  We
		   don't actually need the symbol, because those relocs
		   are always references relative to the nonallocated
		   debugging sections, which start at zero.  */
		value = 0;
	      else
		{
		  GElf_Sym sym_mem;
		  GElf_Word shndx;
		  GElf_Sym *sym = gelf_getsymshndx (mod->symdata,
						    mod->symxndxdata,
						    symndx, &sym_mem,
						    &shndx);
		  if (sym == NULL)
		    return DWFL_E_LIBELF;

		  value = sym->st_value;
		  if (sym->st_shndx != SHN_XINDEX)
		    shndx = sym->st_shndx;
		  switch (shndx)
		    {
		    case SHN_ABS:
		      break;

		    case SHN_UNDEF:
		    case SHN_COMMON:
		      return DWFL_E_RELUNDEF;

		    default:
		      {
			Dwfl_Error error = adjust (shndx);
			if (error != DWFL_E_NOERROR)
			  return error;
			break;
		      }
		    }
		}

	      /* These are the types we can relocate.  */
#define TYPES		DO_TYPE (BYTE, Byte) DO_TYPE (HALF, Half) \
			DO_TYPE (WORD, Word) DO_TYPE (SWORD, Sword) \
			DO_TYPE (XWORD, Xword) DO_TYPE (SXWORD, Sxword)
	      size_t size;
	      Elf_Type type = ebl_reloc_simple_type (mod->ebl, rtype);
	      switch (type)
		{
#define DO_TYPE(NAME, Name)						      \
		    case ELF_T_##NAME:					      \
		      size = sizeof (GElf_##Name);			      \
		      break;
		  TYPES
#undef DO_TYPE
		    default:
		  return DWFL_E_BADRELTYPE;
		}

	      if (offset + size >= tdata->d_size)
		return DWFL_E_BADRELOFF;

#define DO_TYPE(NAME, Name) GElf_##Name Name;
	      union { TYPES } tmpbuf;
#undef DO_TYPE
	      Elf_Data tmpdata =
		{
		  .d_type = type,
		  .d_buf = &tmpbuf,
		  .d_size = size,
		  .d_version = EV_CURRENT,
		};
	      Elf_Data rdata =
		{
		  .d_type = type,
		  .d_buf = tdata->d_buf + offset,
		  .d_size = size,
		  .d_version = EV_CURRENT,
		};

	      /* XXX check for overflow? */
	      if (addend)
		{
		  /* For the addend form, we have the value already.  */
		  value += *addend;
		  switch (type)
		    {
#define DO_TYPE(NAME, Name)						      \
			case ELF_T_##NAME:				      \
			  tmpbuf.Name = value;				      \
			  break;
		      TYPES
#undef DO_TYPE
			default:
		      abort ();
		    }
		}
	      else
		{
		  /* Extract the original value and apply the reloc.  */
		  Elf_Data *d = gelf_xlatetom (mod->main.elf, &tmpdata, &rdata,
					       ehdr->e_ident[EI_DATA]);
		  if (d == NULL)
		    return DWFL_E_LIBELF;
		  assert (d == &tmpdata);
		  switch (type)
		    {
#define DO_TYPE(NAME, Name)						      \
			case ELF_T_##NAME:				      \
			  tmpbuf.Name += (GElf_##Name) value;		      \
			  break;
		      TYPES
#undef DO_TYPE
			default:
		      abort ();
		    }
		}

	      /* Now convert the relocated datum back to the target
		 format.  This will write into rdata.d_buf, which
		 points into the raw section data being relocated.  */
	      Elf_Data *s = gelf_xlatetof (mod->main.elf, &rdata, &tmpdata,
					   ehdr->e_ident[EI_DATA]);
	      if (s == NULL)
		return DWFL_E_LIBELF;
	      assert (s == &rdata);

	      /* We have applied this relocation!  */
	      return DWFL_E_NOERROR;
	    }

	  /* Fetch the relocation section and apply each reloc in it.  */
	  Elf_Data *reldata = elf_getdata (scn, NULL);
	  if (reldata == NULL)
	    return DWFL_E_LIBELF;

	  result = DWFL_E_NOERROR;
	  size_t nrels = shdr->sh_size / shdr->sh_entsize;
	  if (shdr->sh_type == SHT_REL)
	    for (size_t relidx = 0; !result && relidx < nrels; ++relidx)
	      {
		GElf_Rel rel_mem, *r = gelf_getrel (reldata, relidx, &rel_mem);
		if (r == NULL)
		  return DWFL_E_LIBELF;
		result = relocate (r->r_offset, NULL,
				   GELF_R_TYPE (r->r_info),
				   GELF_R_SYM (r->r_info));
	      }
	  else
	    for (size_t relidx = 0; !result && relidx < nrels; ++relidx)
	      {
		GElf_Rela rela_mem, *r = gelf_getrela (reldata, relidx,
						       &rela_mem);
		if (r == NULL)
		  return DWFL_E_LIBELF;
		result = relocate (r->r_offset, &r->r_addend,
				   GELF_R_TYPE (r->r_info),
				   GELF_R_SYM (r->r_info));
	      }
	  if (result != DWFL_E_NOERROR)
	    break;
	}
    }

  return result;
}