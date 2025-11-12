/*
    This file is part of Android File Transfer For Linux.
    Copyright (C) 2015-2020  Vladimir Menshakov

    This library is free software; you can redistribute it and/or modify it
    under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 2.1 of the License,
    or (at your option) any later version.

    This library is distributed in the hope that it will be useful, but
    WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
    Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this library; if not, write to the Free Software Foundation,
    Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#ifndef AFTL_MTP_BACKEND_WINDOWS_COMPAT_H
#define AFTL_MTP_BACKEND_WINDOWS_COMPAT_H

#ifdef _WIN32

#include <string.h>
#include <time.h>

// Windows compatibility layer for POSIX functions

// Case-insensitive substring search (replacement for strcasestr)
inline const char* strcasestr(const char* haystack, const char* needle)
{
	if (!needle || !*needle)
		return haystack;

	size_t needle_len = strlen(needle);

	for (const char* p = haystack; *p; ++p)
	{
		if (_strnicmp(p, needle, needle_len) == 0)
			return p;
	}

	return nullptr;
}

// Thread-safe gmtime (replacement for gmtime_r)
inline struct tm* gmtime_r(const time_t* timep, struct tm* result)
{
	if (gmtime_s(result, timep) == 0)
		return result;
	return nullptr;
}

// Thread-safe localtime (replacement for localtime_r)
inline struct tm* localtime_r(const time_t* timep, struct tm* result)
{
	if (localtime_s(result, timep) == 0)
		return result;
	return nullptr;
}

// Parse time string (simplified replacement for strptime)
// This is a minimal implementation that handles the common MTP format: "YYYYMMDDTHHmmss"
inline char* strptime(const char* s, const char* format, struct tm* tm)
{
	// Simple implementation for MTP date format: "YYYYMMDDTHHmmss" or "%Y%m%dT%H%M%S"
	// If format is different, this needs to be extended

	if (!s || !format || !tm)
		return nullptr;

	memset(tm, 0, sizeof(struct tm));

	// Check if format matches common MTP patterns
	if (strcmp(format, "%Y%m%dT%H%M%S") == 0 || strcmp(format, "%Y%m%dT%H%M%S.0") == 0)
	{
		// Parse: YYYYMMDDTHHmmss
		if (sscanf_s(s, "%4d%2d%2dT%2d%2d%2d",
			&tm->tm_year, &tm->tm_mon, &tm->tm_mday,
			&tm->tm_hour, &tm->tm_min, &tm->tm_sec) == 6)
		{
			tm->tm_year -= 1900;  // years since 1900
			tm->tm_mon -= 1;      // months since January (0-11)
			return const_cast<char*>(s + 15);  // Point past parsed characters
		}
	}

	// Fallback: return nullptr for unsupported formats
	return nullptr;
}

#endif // _WIN32

#endif // AFTL_MTP_BACKEND_WINDOWS_COMPAT_H
