/* Find a named variable or parameter within given scopes.
   Copyright (C) 2005 Red Hat, Inc.

   This program is Open Source software; you can redistribute it and/or
   modify it under the terms of the Open Software License version 1.0 as
   published by the Open Source Initiative.

   You should have received a copy of the Open Software License along
   with this program; if not, you may obtain a copy of the Open Software
   License version 1.0 from http://www.opensource.org/licenses/osl.php or
   by writing the Open Source Initiative c/o Lawrence Rosen, Esq.,
   3001 King Ranch Road, Ukiah, CA 95482.   */

#ifdef HAVE_CONFIG_H
# include <config.h>
#endif

#include <stdbool.h>
#include <string.h>
#include "libdwP.h"
#include <dwarf.h>


/* Find the containing CU's files.  */
static int
getfiles (Dwarf_Die *die, Dwarf_Files **files)
{
  Dwarf_Die cudie =
    {
      .cu = die->cu,
      .addr = ((char *) die->cu->dbg->sectiondata[IDX_debug_info]->d_buf
	       + 3 * die->cu->offset_size - 4 + 3),
    };
  return INTUSE(dwarf_getsrcfiles) (&cudie, files, NULL);
}

/* Fetch an attribute that should have a constant integer form.  */
static int
getattr (Dwarf_Die *die, int search_name, Dwarf_Word *value)
{
  Dwarf_Attribute attr_mem;
  return INTUSE(dwarf_formudata) (INTUSE(dwarf_attr) (die, search_name,
						      &attr_mem), value);
}

/* Search SCOPES[0..NSCOPES-1] for a variable called NAME.
   Ignore the first SKIP_SHADOWS scopes that match the name.
   If MATCH_FILE is not null, accept only declaration in that source file;
   if MATCH_LINENO or MATCH_LINECOL are also nonzero, accept only declaration
   at that line and column.

   If successful, fill in *RESULT with the DIE of the variable found,
   and return N where SCOPES[N] is the scope defining the variable.
   Return -1 for errors or -2 for no matching variable found.  */

int
dwarf_getscopevar (Dwarf_Die *scopes, int nscopes,
		   const char *name, int skip_shadows,
		   const char *match_file, int match_lineno, int match_linecol,
		   Dwarf_Die *result)
{
  /* Match against the given file name.  */
  size_t match_file_len = match_file == NULL ? 0 : strlen (match_file);
  bool lastfile_matches = false;
  const char *lastfile = NULL;
  inline bool file_matches (Dwarf_Files *files, size_t idx)
    {
      if (idx >= files->nfiles)
	return false;

      const char *file = files->info[idx].name;
      if (file != lastfile)
	{
	  size_t len = strlen (file);
	  lastfile_matches = (len >= match_file_len
			      && !memcmp (match_file, file, match_file_len)
			      && (len == match_file_len
				  || file[len - match_file_len - 1] == '/'));
	}
      return lastfile_matches;
    }

  /* Start with the innermost scope and move out.  */
  for (int out = 0; out < nscopes; ++out)
    if (INTUSE(dwarf_haschildren) (&scopes[out]))
      {
	if (INTUSE(dwarf_child) (&scopes[out], result) != 0)
	  return -1;
	do
	  {
	    switch (INTUSE(dwarf_tag) (result))
	      {
	      case DW_TAG_variable:
	      case DW_TAG_formal_parameter:
		break;

	      default:
		continue;
	      }

	    /* Only get here for a variable or parameter.  Check the name.  */
	    Dwarf_Attribute attr_mem;
	    const char *diename = INTUSE(dwarf_formstring)
	      (INTUSE(dwarf_attr_integrate) (result, DW_AT_name, &attr_mem));
	    if (diename != NULL && !strcmp (name, diename))
	      {
		/* We have a matching name.  */

		if (skip_shadows > 0)
		  {
		    /* Punt this scope for the one it shadows.  */
		    --skip_shadows;
		    break;
		  }

		if (match_file != NULL)
		  {
		    /* Check its decl_file.  */

		    Dwarf_Word i;
		    Dwarf_Files *files;
		    if (getattr (result, DW_AT_decl_file, &i) != 0
			|| getfiles (&scopes[out], &files) != 0)
		      break;

		    if (!file_matches (files, i))
		      break;

		    if (match_lineno > 0
			&& (getattr (result, DW_AT_decl_line, &i) != 0
			    || (int) i != match_lineno))
		      break;
		    if (match_linecol > 0
			&& (getattr (result, DW_AT_decl_column, &i) != 0
			    || (int) i != match_linecol))
		      break;
		  }

		/* We have a winner!  */
		return out;
	      }
	  }
	while (INTUSE(dwarf_siblingof) (result, result) == 0);
      }

  return -2;
}