/*
 * kexec: Linux boots Linux
 *
 * Created by: Mohan Kumar M (mohan@in.ibm.com)
 *
 * Copyright (C) IBM Corporation, 2005. All rights reserved
 *
 * Code taken from kexec-tools.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation (version 2 of the License).
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "purgatory.h"
#include "purgatory-ppc64.h"

unsigned int panic_kernel = 0;
unsigned long backup_start = 0;
unsigned long stack = 0;
unsigned long dt_offset = 0;
unsigned long my_toc = 0;
unsigned long kernel = 0;
unsigned int debug = 0;
unsigned long opal_base = 0;
unsigned long opal_entry = 0;

void setup_arch(void)
{
}

void post_verification_setup_arch(void)
{
	if (panic_kernel)
		crashdump_backup_memory();
}
