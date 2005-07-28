/* Find FDE for given address.
   Copyright (C) 2001, 2002 Red Hat, Inc.
   Written by Ulrich Drepper <drepper@redhat.com>, 2001.

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

#include "libdwarfP.h"


int
dwarf_get_fde_at_pc (fde_data, pc_of_interest, returned_fde, lopc, hipc, error)
     Dwarf_Fde *fde_data;
     Dwarf_Addr pc_of_interest;
     Dwarf_Fde *returned_fde;
     Dwarf_Addr *lopc;
     Dwarf_Addr *hipc;
     Dwarf_Error *error;
{
  Dwarf_Debug dbg = fde_data[0]->cie->dbg;
  int low = 0;
  int high = dbg->fde_cnt - 1;

  /* Since the FDEs are sorted by their addresses and since there can
     potentially be many FDEs we better use binary search.  */
  while (low <= high)
    {
      int curidx = (low + high) / 2;
      Dwarf_Fde cur = fde_data[curidx];

      if (pc_of_interest < cur->initial_location)
	high = curidx - 1;
      else if (likely (cur->initial_location + cur->address_range
		       <= pc_of_interest))
	low = curidx + 1;
      else
	{
	  *returned_fde = cur;
	  *lopc = cur->initial_location;
	  *hipc = cur->initial_location + cur->address_range - 1;
	  return DW_DLV_OK;
	}
    }

  return DW_DLV_NO_ENTRY;
}