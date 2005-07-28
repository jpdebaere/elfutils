/* Find source location for PC address.
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

Dwfl_Line *
dwfl_getsrc (Dwfl *dwfl, Dwarf_Addr addr)
{
  return INTUSE(dwfl_module_getsrc) (INTUSE(dwfl_addrmodule) (dwfl, addr),
				     addr);
}