/*
* Basic Excel R Toolkit (BERT)
* Copyright (C) 2014 Structured Data, LLC
*
* This program is free software; you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation; either version 2 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License along
* with this program; if not, write to the Free Software Foundation, Inc.,
* 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*
*/

#ifndef __DEBUGOUT_H
#define __DEBUGOUT_H

#include <stdio.h>
#include <stdarg.h>

#ifdef _DEBUG

int DebugOut(const char *fmt, ...);

#else
#define DebugOut() 
#endif

#endif // #ifndef __DEBUGOUT_H


