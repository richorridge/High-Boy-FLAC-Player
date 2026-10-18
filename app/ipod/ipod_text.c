// Copyright (c) 2026 High Boy iPod app contributors
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version. See LICENSE.

#include "ipod_text.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define LATIN1_FIRST    0xA0
#define LATIN1_LAST     0xFF
#define LATIN_EXT_FIRST 0x100
#define LATIN_EXT_LAST  0x17F
#define MS_PER_SEC      1000u
#define SEC_PER_MIN     60u
#define SEC_PER_HOUR    3600u
#define ARTICLE         "the "
#define ARTICLE_LEN     4

// U+00A0..U+00FF, one ASCII replacement per code point.
static const char *const LATIN1_FOLD[LATIN1_LAST - LATIN1_FIRST + 1] = {
    " ", "!",  "c", "L", "",  "Y", "|",  "S", "",  "(c)", "a", "<<", "",    "-",   "(R)", "",
    "o", "+-", "2", "3", "'", "u", "P",  ".", "",  "1",   "o", ">>", "1/4", "1/2", "3/4", "?",
    "A", "A",  "A", "A", "A", "A", "AE", "C", "E", "E",   "E", "E",  "I",   "I",   "I",   "I",
    "D", "N",  "O", "O", "O", "O", "O",  "x", "O", "U",   "U", "U",  "U",   "Y",   "Th",  "ss",
    "a", "a",  "a", "a", "a", "a", "ae", "c", "e", "e",   "e", "e",  "i",   "i",   "i",   "i",
    "d", "n",  "o", "o", "o", "o", "o",  "/", "o", "u",   "u", "u",  "u",   "y",   "th",  "y",
};

// U+0100..U+017F (Latin Extended-A), base letter per code point.
static const char LATIN_EXT_FOLD[] = "AaAaAa"
                                     "CcCcCcCc"
                                     "DdDd"
                                     "EeEeEeEeEe"
                                     "GgGgGgGg"
                                     "HhHh"
                                     "IiIiIiIiIi"
                                     "Ii"
                                     "Jj"
                                     "Kkk"
                                     "LlLlLlLlLl"
                                     "NnNnNnnNn"
                                     "OoOoOo"
                                     "Oo"
                                     "RrRrRr"
                                     "SsSsSsSs"
                                     "TtTtTt"
                                     "UuUuUuUuUuUu"
                                     "Ww"
                                     "YyY"
                                     "ZzZzZz"
                                     "s";

_Static_assert(sizeof(LATIN_EXT_FOLD) == (LATIN_EXT_LAST - LATIN_EXT_FIRST + 1) + 1,
               "Latin Extended-A fold table must cover U+0100..U+017F");

static uint32_t next_codepoint(const unsigned char **p);
static const char *fold_codepoint(uint32_t cp, char *scratch);

size_t ipod_text_fold(const char *in, char *out, size_t out_len) {
  if (out == NULL || out_len == 0)
    return 0;
  size_t n = 0;
  const unsigned char *p = (const unsigned char *)(in != NULL ? in : "");
  char scratch[2];
  while (*p != '\0') {
    uint32_t cp = next_codepoint(&p);
    const char *rep = fold_codepoint(cp, scratch);
    size_t len = strlen(rep);
    if (n + len >= out_len)
      break;
    memcpy(out + n, rep, len);
    n += len;
  }
  out[n] = '\0';
  return n;
}

void ipod_text_sort_key(const char *in, char *out, size_t out_len) {
  if (out == NULL || out_len == 0)
    return;
  char folded[256];
  ipod_text_fold(in, folded, sizeof(folded));
  for (char *c = folded; *c != '\0'; c++)
    *c = (char)tolower((unsigned char)*c);

  const char *start = folded;
  while (*start != '\0' && !isalnum((unsigned char)*start))
    start++;
  if (*start == '\0')
    start = folded; // all punctuation: keep it as-is
  if (strncmp(start, ARTICLE, ARTICLE_LEN) == 0 && start[ARTICLE_LEN] != '\0')
    start += ARTICLE_LEN;
  ipod_text_copy(out, out_len, start);
}

int ipod_text_key_cmp(const char *a, const char *b) {
  const bool a_alpha = isalpha((unsigned char)a[0]) != 0;
  const bool b_alpha = isalpha((unsigned char)b[0]) != 0;
  if (a_alpha != b_alpha)
    return a_alpha ? -1 : 1;
  return strcmp(a, b);
}

char ipod_text_index_char(const char *sort_key) {
  unsigned char c = (unsigned char)(sort_key != NULL ? sort_key[0] : 0);
  if (isalpha(c))
    return (char)toupper(c);
  return '#';
}

void ipod_text_fmt_time(char *out, size_t out_len, uint32_t ms) {
  uint32_t sec = ms / MS_PER_SEC;
  if (sec >= SEC_PER_HOUR) {
    snprintf(out,
             out_len,
             "%u:%02u:%02u",
             (unsigned)(sec / SEC_PER_HOUR),
             (unsigned)((sec % SEC_PER_HOUR) / SEC_PER_MIN),
             (unsigned)(sec % SEC_PER_MIN));
  } else {
    snprintf(out, out_len, "%u:%02u", (unsigned)(sec / SEC_PER_MIN), (unsigned)(sec % SEC_PER_MIN));
  }
}

bool ipod_text_contains_ci(const char *haystack, const char *needle) {
  if (needle == NULL || needle[0] == '\0')
    return true;
  if (haystack == NULL)
    return false;
  size_t nlen = strlen(needle);
  for (const char *h = haystack; *h != '\0'; h++) {
    size_t i = 0;
    while (i < nlen && h[i] != '\0' &&
           tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
      i++;
    if (i == nlen)
      return true;
  }
  return false;
}

bool ipod_text_equal_ci(const char *a, const char *b) {
  while (*a != '\0' && *b != '\0') {
    if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
      return false;
    a++;
    b++;
  }
  return *a == *b;
}

void ipod_text_copy(char *out, size_t out_len, const char *in) {
  if (out == NULL || out_len == 0)
    return;
  if (in == NULL)
    in = "";
  size_t len = strlen(in);
  if (len >= out_len)
    len = out_len - 1;
  memcpy(out, in, len);
  out[len] = '\0';
}

// Decode one UTF-8 sequence. Invalid bytes are taken as Latin-1, which is the
// most common mis-encoding found in hand-edited tags.
static uint32_t next_codepoint(const unsigned char **p) {
  const unsigned char *s = *p;
  uint32_t c = s[0];
  int extra = 0;
  if (c < 0x80) {
    *p = s + 1;
    return c;
  }
  if ((c & 0xE0) == 0xC0) {
    extra = 1;
    c &= 0x1F;
  } else if ((c & 0xF0) == 0xE0) {
    extra = 2;
    c &= 0x0F;
  } else if ((c & 0xF8) == 0xF0) {
    extra = 3;
    c &= 0x07;
  } else {
    *p = s + 1;
    return s[0];
  }
  for (int i = 1; i <= extra; i++) {
    if ((s[i] & 0xC0) != 0x80) {
      *p = s + 1;
      return s[0];
    }
    c = (c << 6) | (s[i] & 0x3F);
  }
  *p = s + 1 + extra;
  return c;
}

static const char *fold_codepoint(uint32_t cp, char *scratch) {
  if (cp >= 0x20 && cp < 0x7F) {
    scratch[0] = (char)cp;
    scratch[1] = '\0';
    return scratch;
  }
  if (cp == '\t')
    return " ";
  if (cp < 0x20 || cp == 0x7F)
    return "";
  if (cp >= LATIN1_FIRST && cp <= LATIN1_LAST)
    return LATIN1_FOLD[cp - LATIN1_FIRST];
  if (cp >= LATIN_EXT_FIRST && cp <= LATIN_EXT_LAST) {
    scratch[0] = LATIN_EXT_FOLD[cp - LATIN_EXT_FIRST];
    scratch[1] = '\0';
    return scratch;
  }
  switch (cp) {
    case 0x2018: // left/right single quotes, prime
    case 0x2019:
    case 0x2032:
      return "'";
    case 0x201C: // left/right double quotes
    case 0x201D:
      return "\"";
    case 0x2010: // hyphens and dashes
    case 0x2011:
    case 0x2012:
    case 0x2013:
    case 0x2014:
    case 0x2015:
    case 0x2212:
      return "-";
    case 0x2026:
      return "...";
    case 0x2022:
    case 0x00B7:
      return "*";
    case 0x2122:
      return "TM";
    case 0x20AC:
      return "EUR";
    case 0xFEFF: // byte-order mark
    case 0x200B: // zero-width space
      return "";
    default:
      return "?";
  }
}
