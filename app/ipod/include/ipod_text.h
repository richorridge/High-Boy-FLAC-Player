// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#ifndef IPOD_TEXT_H
#define IPOD_TEXT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fold UTF-8 text to the printable ASCII the device fonts can draw.
 *
 * The built-in Montserrat fonts only carry ASCII, so accented Latin letters
 * are reduced to their base letter (e.g. "Beyoncé" -> "Beyonce"), typographic
 * punctuation to its ASCII twin, and anything else to '?'. Always
 * NUL-terminates when @p out_len > 0.
 *
 * @return Length of the folded string (excluding the terminator).
 */
size_t ipod_text_fold(const char *in, char *out, size_t out_len);

/**
 * @brief Build the iPod sort key for a title/name: folded, lower-cased,
 *        leading "the " and leading punctuation removed.
 */
void ipod_text_sort_key(const char *in, char *out, size_t out_len);

/**
 * @brief Compare two sort keys the way the iPod orders lists: letters A-Z
 *        first, then names starting with digits or symbols.
 */
int ipod_text_key_cmp(const char *a, const char *b);

/** @brief Index letter of a sort key: 'A'..'Z', or '#' for everything else. */
char ipod_text_index_char(const char *sort_key);

/** @brief Format milliseconds as "m:ss" (or "h:mm:ss" past an hour). */
void ipod_text_fmt_time(char *out, size_t out_len, uint32_t ms);

/** @brief Case-insensitive ASCII substring test (both inputs already folded). */
bool ipod_text_contains_ci(const char *haystack, const char *needle);

/** @brief Case-insensitive ASCII string equality. */
bool ipod_text_equal_ci(const char *a, const char *b);

/** @brief Copy at most out_len-1 bytes and NUL-terminate. */
void ipod_text_copy(char *out, size_t out_len, const char *in);

#ifdef __cplusplus
}
#endif

#endif // IPOD_TEXT_H
