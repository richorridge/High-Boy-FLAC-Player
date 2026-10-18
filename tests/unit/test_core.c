// Copyright (c) 2026 High Boy iPod app contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Unit tests for the iPod app's core (non-UI) modules. Run with the folder
// produced by tools/make_test_library.py as the only argument.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "dr_flac.h"
#include "ipod_dsp.h"
#include "ipod_library.h"
#include "ipod_meta.h"
#include "ipod_queue.h"
#include "ipod_settings.h"
#include "ipod_text.h"

static int g_checks;
static int g_failures;
static char g_root[4096];

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    g_checks++;                                                                                    \
    if (!(cond)) {                                                                                 \
      g_failures++;                                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

#define CHECK_STR(a, b)                                                                            \
  do {                                                                                             \
    g_checks++;                                                                                    \
    const char *_a = (a);                                                                          \
    const char *_b = (b);                                                                          \
    if (strcmp(_a, _b) != 0) {                                                                     \
      g_failures++;                                                                                \
      fprintf(stderr, "%s:%d: \"%s\" != \"%s\"\n", __FILE__, __LINE__, _a, _b);                    \
    }                                                                                              \
  } while (0)

static const char *lib_path(const char *rel) {
  static char buf[4][1024];
  static int k;
  k = (k + 1) % 4;
  snprintf(buf[k], sizeof(buf[k]), "%s/%s", g_root, rel);
  return buf[k];
}

// ---------------------------------------------------------------------------

static void test_text(void) {
  char out[64];
  ipod_text_fold("Beyoncé", out, sizeof(out));
  CHECK_STR(out, "Beyonce");
  ipod_text_fold("Zoë Brontë", out, sizeof(out));
  CHECK_STR(out, "Zoe Bronte");
  ipod_text_fold("Smörgåsbord", out, sizeof(out));
  CHECK_STR(out, "Smorgasbord");
  ipod_text_fold("Ça Va", out, sizeof(out));
  CHECK_STR(out, "Ca Va");
  ipod_text_fold("Łódź Ærø Straße", out, sizeof(out));
  CHECK_STR(out, "Lodz AEro Strasse");
  ipod_text_fold("\xE2\x80\x9CHi\xE2\x80\x9D\xE2\x80\x94\xE2\x80\xA6", out, sizeof(out));
  CHECK_STR(out, "\"Hi\"-...");
  ipod_text_fold("\xE6\x97\xA5\xE6\x9C\xAC", out, sizeof(out)); // CJK
  CHECK_STR(out, "??");
  ipod_text_fold("\xE9t\xE9", out, sizeof(out)); // Latin-1 bytes, not UTF-8
  CHECK_STR(out, "ete");
  ipod_text_fold("abcdef", out, 4);
  CHECK_STR(out, "abc");
  ipod_text_fold("\xEF\xBB\xBFtab\there", out, sizeof(out));
  CHECK_STR(out, "tab here");

  ipod_text_sort_key("The Polygons", out, sizeof(out));
  CHECK_STR(out, "polygons");
  ipod_text_sort_key("\"Heroes\"", out, sizeof(out));
  CHECK_STR(out, "heroes\"");
  ipod_text_sort_key("The", out, sizeof(out));
  CHECK_STR(out, "the");
  ipod_text_sort_key("Électrique", out, sizeof(out));
  CHECK_STR(out, "electrique");

  CHECK(ipod_text_key_cmp("zebra", "10 songs") < 0);
  CHECK(ipod_text_key_cmp("apple", "banana") < 0);
  CHECK(ipod_text_key_cmp("same", "same") == 0);
  CHECK(ipod_text_index_char("polygons") == 'P');
  CHECK(ipod_text_index_char("10 songs") == '#');
  CHECK(ipod_text_index_char("") == '#');

  ipod_text_fmt_time(out, sizeof(out), 0);
  CHECK_STR(out, "0:00");
  ipod_text_fmt_time(out, sizeof(out), 61999);
  CHECK_STR(out, "1:01");
  ipod_text_fmt_time(out, sizeof(out), 3723000);
  CHECK_STR(out, "1:02:03");

  CHECK(ipod_text_contains_ci("Hexagon Sun", "SUN"));
  CHECK(!ipod_text_contains_ci("abc", "abd"));
  CHECK(ipod_text_contains_ci("abc", ""));
  CHECK(ipod_text_equal_ci("FLAC", "flac"));
  CHECK(!ipod_text_equal_ci("flac", "flacs"));
}

// ---------------------------------------------------------------------------

static void test_meta(void) {
  ipod_meta_t m;
  CHECK(ipod_meta_read_flac(lib_path("Music/Aurora Vale/Northern Lights/1-01 Polar Night.flac"), &m));
  CHECK_STR(m.title, "Polar Night");
  CHECK_STR(m.artist, "Aurora Vale");
  CHECK_STR(m.album, "Northern Lights");
  CHECK_STR(m.genre, "Ambient");
  CHECK(m.year == 2006);
  CHECK(m.track_no == 1);
  CHECK(m.disc_no == 1);
  CHECK(m.has_rg_track && fabsf(m.rg_track_gain_db - 6.0f) < 0.01f);
  CHECK(m.has_rg_album && fabsf(m.rg_album_gain_db - 4.5f) < 0.01f);
  CHECK(m.sample_rate == 44100 && m.channels == 2 && m.bits_per_sample == 16);
  CHECK(m.total_frames == 4u * 44100u);
  CHECK(ipod_meta_duration_ms(&m) == 4000);
  CHECK_STR(m.picture_mime, "image/jpeg");
  CHECK(m.picture_offset > 0 && m.picture_size > 1000);
  FILE *f = fopen(lib_path("Music/Aurora Vale/Northern Lights/1-01 Polar Night.flac"), "rb");
  unsigned char magic[4] = {0};
  CHECK(f != NULL);
  if (f != NULL) {
    fseek(f, (long)m.picture_offset, SEEK_SET);
    CHECK(fread(magic, 1, 2, f) == 2);
    fclose(f);
  }
  CHECK(magic[0] == 0xFF && magic[1] == 0xD8);

  CHECK(ipod_meta_read_flac(lib_path("Music/The Polygons/Square Waves/01 Triangle Theory.flac"), &m));
  CHECK_STR(m.composer, "Ada Vector");
  CHECK_STR(m.picture_mime, "image/png");
  CHECK(!m.has_rg_track);
  f = fopen(lib_path("Music/The Polygons/Square Waves/01 Triangle Theory.flac"), "rb");
  if (f != NULL) {
    fseek(f, (long)m.picture_offset, SEEK_SET);
    CHECK(fread(magic, 1, 4, f) == 4);
    fclose(f);
  }
  CHECK(memcmp(magic, "\x89PNG", 4) == 0);

  CHECK(ipod_meta_read_flac(lib_path("Music/Compilations/Summer Mix 2005/03 Hi-Res Horizon.flac"), &m));
  CHECK(m.sample_rate == 96000 && m.bits_per_sample == 24);
  CHECK(m.is_compilation);
  CHECK(ipod_meta_read_flac(lib_path("Music/Compilations/Summer Mix 2005/02 Mono Lake.flac"), &m));
  CHECK(m.channels == 1);
  CHECK(ipod_meta_read_flac(lib_path("Music/Odd/ID3 Prefixed.flac"), &m));
  CHECK_STR(m.title, "ID3 Prefixed");
  CHECK(ipod_meta_read_flac(lib_path("Music/Zoë Brontë/Électrique/02 Smörgåsbord.flac"), &m));
  CHECK_STR(m.title, "Smörgåsbord");
  CHECK(m.picture_offset == 0);
  CHECK(ipod_meta_read_flac(lib_path("Music/loose_track.flac"), &m));
  CHECK(m.title[0] == '\0');

  CHECK(!ipod_meta_read_flac(lib_path("Music/Odd/corrupt.flac"), &m));
  CHECK(!ipod_meta_read_flac(lib_path("Music/Odd/notes.txt"), &m));
  CHECK(!ipod_meta_read_flac(lib_path("Music/does-not-exist.flac"), &m));
  CHECK(!ipod_meta_read_flac(NULL, &m));
}

// ---------------------------------------------------------------------------

static int find_title(const ipod_library_t *lib, const char *title) {
  for (uint32_t i = 0; i < ipod_lib_count(lib); i++) {
    if (strcmp(ipod_lib_str(lib, ipod_lib_track(lib, i)->title), title) == 0)
      return (int)i;
  }
  return -1;
}

static void check_groups(const ipod_library_t *lib,
                         ipod_field_t field,
                         const ipod_filter_t *filter,
                         const char *const *expected,
                         size_t n_expected) {
  uint32_t *g = NULL;
  size_t n = ipod_lib_query_groups(lib, field, filter, &g);
  CHECK(n == n_expected);
  for (size_t i = 0; i < n && i < n_expected; i++)
    CHECK_STR(ipod_lib_disp(lib, ipod_lib_field_value(lib, field, g[i])), expected[i]);
  free(g);
}

static void check_library_contents(const ipod_library_t *lib) {
  CHECK(ipod_lib_count(lib) == 16);
  CHECK(ipod_lib_playlist_count(lib) == 1);
  CHECK_STR(ipod_lib_playlist_name(lib, 0), "Road Trip");

  static const char *const ARTISTS[] = {
      "Aurora Vale", "The Polygons", "Tag Tester", "Unknown Artist", "Various Artists",
      "Zoe Bronte"};
  check_groups(lib, IPOD_FIELD_ARTIST, NULL, ARTISTS, 6);
  static const char *const ALBUMS[] = {"Cafe Nocturne",   "Edge Cases",   "Electrique",
                                       "Northern Lights", "Square Waves", "Summer Mix 2005",
                                       "Unknown Album"};
  check_groups(lib, IPOD_FIELD_ALBUM, NULL, ALBUMS, 7);
  static const char *const GENRES[] = {"Ambient", "Electronic", "Jazz", "Pop", "Test"};
  check_groups(lib, IPOD_FIELD_GENRE, NULL, GENRES, 5);
  static const char *const COMPOSERS[] = {"Ada Vector", "J. S. Bachelor"};
  check_groups(lib, IPOD_FIELD_COMPOSER, NULL, COMPOSERS, 2);

  int polar = find_title(lib, "Polar Night");
  CHECK(polar >= 0);
  if (polar < 0)
    return;
  const ipod_track_t *t = ipod_lib_track(lib, (uint32_t)polar);
  ipod_filter_t by_artist = {.album_artist = t->album_artist};
  static const char *const AURORA_ALBUMS[] = {"Cafe Nocturne", "Northern Lights"};
  check_groups(lib, IPOD_FIELD_ALBUM, &by_artist, AURORA_ALBUMS, 2);

  ipod_filter_t by_album = {.album_artist = t->album_artist, .album = t->album};
  uint32_t *tr = NULL;
  size_t n = ipod_lib_query_tracks(lib, &by_album, IPOD_ORDER_ALBUM, &tr);
  CHECK(n == 3);
  if (n == 3) {
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[0])->title), "Polar Night");
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[1])->title), "Magnetosphere");
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[2])->title), "Solar Wind");
  }
  free(tr);

  n = ipod_lib_query_tracks(lib, NULL, IPOD_ORDER_TITLE, &tr);
  CHECK(n == 16);
  for (size_t i = 1; i < n; i++) {
    const char *a = ipod_lib_key(lib, ipod_lib_track(lib, tr[i - 1])->title);
    const char *b = ipod_lib_key(lib, ipod_lib_track(lib, tr[i])->title);
    CHECK(ipod_text_key_cmp(a, b) <= 0);
  }
  if (n > 0)
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[0])->title), "Acute Angles");
  free(tr);

  CHECK(ipod_lib_compilation_count(lib) == 3);
  ipod_filter_t comp = {.compilations_only = true};
  static const char *const COMP_ALBUMS[] = {"Summer Mix 2005"};
  check_groups(lib, IPOD_FIELD_ALBUM, &comp, COMP_ALBUMS, 1);

  int mono = find_title(lib, "Mono Lake");
  CHECK(mono >= 0);
  if (mono >= 0) {
    const ipod_track_t *mt = ipod_lib_track(lib, (uint32_t)mono);
    CHECK_STR(ipod_lib_str(lib, mt->artist), "Mono Lake");
    CHECK_STR(ipod_lib_str(lib, mt->album_artist), "Various Artists");
    ipod_filter_t ambient = {.genre = mt->genre};
    static const char *const AMBIENT_ARTISTS[] = {"Aurora Vale", "Various Artists"};
    check_groups(lib, IPOD_FIELD_ARTIST, &ambient, AMBIENT_ARTISTS, 2);
  }

  int loose = find_title(lib, "loose_track");
  CHECK(loose >= 0);
  int hires = find_title(lib, "Hi-Res Horizon");
  CHECK(hires >= 0 && ipod_lib_track(lib, (uint32_t)hires)->sample_rate == 96000);
  CHECK(ipod_lib_track(lib, (uint32_t)polar)->rg_track_cdb == 600);
  CHECK(ipod_lib_track(lib, (uint32_t)polar)->duration_ms == 4000);

  ipod_hit_t *hits = NULL;
  n = ipod_lib_search(lib, "sun", 50, &hits);
  CHECK(n == 1 && hits[0].kind == IPOD_HIT_SONG);
  free(hits);
  n = ipod_lib_search(lib, "AURORA", 50, &hits);
  CHECK(n >= 1 && hits[0].kind == IPOD_HIT_ARTIST);
  free(hits);
  n = ipod_lib_search(lib, "mix", 50, &hits);
  CHECK(n == 1 && hits[0].kind == IPOD_HIT_ALBUM);
  free(hits);
  n = ipod_lib_search(lib, "smorg", 50, &hits); // matches the folded "Smörgåsbord"
  CHECK(n == 1 && hits[0].kind == IPOD_HIT_SONG);
  free(hits);
  n = ipod_lib_search(lib, "a", 3, &hits);
  CHECK(n == 3);
  free(hits);
  n = ipod_lib_search(lib, "zzzz", 50, &hits);
  CHECK(n == 0 && hits == NULL);

  // Relative, backslash and ./ entries resolve; the absolute /sdcard entry and
  // the missing file do not on the host.
  n = ipod_lib_playlist_tracks(lib, 0, &tr);
  CHECK(n == 3);
  if (n == 3) {
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[0])->title), "Hexagon Sun");
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[1])->title), "Magnetosphere");
    CHECK_STR(ipod_lib_str(lib, ipod_lib_track(lib, tr[2])->title), "Mono Lake");
  }
  free(tr);
}

static void test_library(void) {
  ipod_scan_progress_t prog = {0};
  ipod_library_t *lib = ipod_lib_scan(lib_path("Music"), NULL, &prog);
  CHECK(lib != NULL);
  if (lib == NULL)
    return;
  CHECK(prog.files_found == 17); // 16 valid + corrupt.flac
  CHECK(prog.files_parsed == 17);
  check_library_contents(lib);

  // Case-insensitive path lookup, as on FAT.
  char upper[1024];
  snprintf(upper, sizeof(upper), "%s", lib_path("MUSIC/LOOSE_TRACK.FLAC"));
  uint32_t idx = 0;
  CHECK(ipod_lib_find_path(lib, upper, &idx));

  // Statistics, smart playlists.
  int a = find_title(lib, "Vertex");
  int b = find_title(lib, "Last Order");
  CHECK(a >= 0 && b >= 0);
  ipod_lib_track_mut(lib, (uint32_t)a)->play_count = 5;
  ipod_lib_track_mut(lib, (uint32_t)a)->rating = 5;
  ipod_lib_track_mut(lib, (uint32_t)a)->last_played = 7;
  ipod_lib_track_mut(lib, (uint32_t)b)->play_count = 2;
  ipod_lib_track_mut(lib, (uint32_t)b)->rating = 4;
  ipod_lib_track_mut(lib, (uint32_t)b)->last_played = 9;
  uint32_t *s = NULL;
  size_t n = ipod_lib_smart(lib, IPOD_SMART_TOP_PLAYED, &s);
  CHECK(n == 2 && s[0] == (uint32_t)a && s[1] == (uint32_t)b);
  free(s);
  n = ipod_lib_smart(lib, IPOD_SMART_RECENTLY_PLAYED, &s);
  CHECK(n == 2 && s[0] == (uint32_t)b);
  free(s);
  n = ipod_lib_smart(lib, IPOD_SMART_TOP_RATED, &s);
  CHECK(n == 2 && s[0] == (uint32_t)a);
  free(s);
  n = ipod_lib_smart(lib, IPOD_SMART_RECENTLY_ADDED, &s);
  CHECK(n == 16);
  free(s);
  CHECK(ipod_lib_max_play_seq(lib) == 9);
  uint64_t ms = 0;
  uint64_t bytes = 0;
  ipod_lib_totals(lib, &ms, &bytes);
  CHECK(ms == 16u * 4000u);
  CHECK(bytes > 0);

  // Cache round trip keeps everything, including statistics.
  char cache[1100];
  snprintf(cache, sizeof(cache), "%s/test_cache.db", g_root);
  CHECK(ipod_lib_save(lib, cache));
  ipod_library_t *loaded = ipod_lib_load(cache);
  CHECK(loaded != NULL);
  if (loaded != NULL) {
    check_library_contents(loaded);
    CHECK(ipod_lib_count(loaded) == ipod_lib_count(lib));
    for (uint32_t i = 0; i < ipod_lib_count(lib) && i < ipod_lib_count(loaded); i++) {
      const ipod_track_t *x = ipod_lib_track(lib, i);
      const ipod_track_t *y = ipod_lib_track(loaded, i);
      CHECK_STR(ipod_lib_str(lib, x->path), ipod_lib_str(loaded, y->path));
      CHECK_STR(ipod_lib_str(lib, x->title), ipod_lib_str(loaded, y->title));
      CHECK_STR(ipod_lib_str(lib, x->album_artist), ipod_lib_str(loaded, y->album_artist));
      CHECK(x->duration_ms == y->duration_ms && x->mtime == y->mtime);
      CHECK(x->rating == y->rating && x->play_count == y->play_count);
      CHECK(x->rg_track_cdb == y->rg_track_cdb && x->picture_offset == y->picture_offset);
    }
  }

  // Incremental rescan: nothing re-parsed except the corrupt file, stats kept.
  ipod_scan_progress_t prog2 = {0};
  ipod_library_t *rescan = ipod_lib_scan(lib_path("Music"), loaded, &prog2);
  CHECK(rescan != NULL);
  CHECK(prog2.files_parsed == 1);
  if (rescan != NULL) {
    check_library_contents(rescan);
    int a2 = find_title(rescan, "Vertex");
    CHECK(a2 >= 0 && ipod_lib_track(rescan, (uint32_t)a2)->play_count == 5);
    CHECK(a2 >= 0 && ipod_lib_track(rescan, (uint32_t)a2)->rating == 5);
  }

  // Background rescans leave statistics alone; they are merged afterwards.
  ipod_scan_progress_t prog3 = {.skip_stats = true};
  ipod_library_t *bg = ipod_lib_scan(lib_path("Music"), loaded, &prog3);
  CHECK(bg != NULL && prog3.files_parsed == 1);
  if (bg != NULL && rescan != NULL) {
    int a3 = find_title(bg, "Vertex");
    CHECK(a3 >= 0 && ipod_lib_track(bg, (uint32_t)a3)->play_count == 0);
    CHECK(ipod_lib_same_files(bg, rescan));
    CHECK(ipod_lib_same_files(rescan, loaded));
    ipod_lib_copy_stats(bg, rescan);
    CHECK(a3 >= 0 && ipod_lib_track(bg, (uint32_t)a3)->play_count == 5);
    CHECK(a3 >= 0 && ipod_lib_track(bg, (uint32_t)a3)->rating == 5);
  }
  ipod_library_t *empty_lib = ipod_lib_scan(lib_path("nope"), NULL, NULL);
  CHECK(!ipod_lib_same_files(bg, empty_lib));
  CHECK(!ipod_lib_same_files(bg, NULL));
  ipod_lib_destroy(empty_lib);
  ipod_lib_destroy(bg);

  // Corrupted caches are rejected rather than half-loaded.
  FILE *f = fopen(cache, "r+b");
  if (f != NULL) {
    fseek(f, -2, SEEK_END);
    fputc('X', f);
    fclose(f);
  }
  ipod_library_t *broken = ipod_lib_load(cache);
  CHECK(broken == NULL);
  ipod_lib_destroy(broken);
  f = fopen(cache, "wb");
  if (f != NULL) {
    fputs("IPDBgarbage", f);
    fclose(f);
  }
  CHECK(ipod_lib_load(cache) == NULL);
  remove(cache);
  CHECK(ipod_lib_load(cache) == NULL);

  // m3u write/read round trip (On-The-Go uses this).
  char m3u[1100];
  snprintf(m3u, sizeof(m3u), "%s/otg_test.m3u", g_root);
  uint32_t picks[3] = {(uint32_t)b, (uint32_t)a, 0};
  CHECK(ipod_lib_write_m3u(lib, m3u, picks, 3));
  n = ipod_lib_read_m3u(lib, m3u, &s);
  CHECK(n == 3 && s[0] == picks[0] && s[1] == picks[1] && s[2] == picks[2]);
  free(s);
  remove(m3u);

  // Cancelling a scan returns nothing.
  ipod_scan_progress_t cancelled = {.cancel = true};
  CHECK(ipod_lib_scan(lib_path("Music"), NULL, &cancelled) == NULL);
  // Scanning a missing folder yields an empty library, not a failure.
  ipod_library_t *empty = ipod_lib_scan(lib_path("nope"), NULL, NULL);
  CHECK(empty != NULL && ipod_lib_count(empty) == 0);
  uint32_t *none = NULL;
  CHECK(ipod_lib_query_tracks(empty, NULL, IPOD_ORDER_TITLE, &none) == 0 && none == NULL);
  CHECK(ipod_lib_query_groups(empty, IPOD_FIELD_ARTIST, NULL, &none) == 0 && none == NULL);

  ipod_lib_destroy(empty);
  ipod_lib_destroy(rescan);
  ipod_lib_destroy(loaded);
  ipod_lib_destroy(lib);
}

// ---------------------------------------------------------------------------

static bool is_permutation(const ipod_queue_t *q, const uint32_t *tracks, size_t n) {
  int seen[64] = {0};
  for (size_t i = 0; i < n; i++)
    seen[q->tracks[q->order[i]] - tracks[0]]++;
  for (size_t i = 0; i < n; i++) {
    if (seen[i] != 1)
      return false;
  }
  return true;
}

static void test_queue(void) {
  uint32_t tracks[10];
  for (int i = 0; i < 10; i++)
    tracks[i] = 100u + (uint32_t)i;
  ipod_queue_t q;
  ipod_queue_init(&q, 1234);
  uint32_t cur = 0;
  CHECK(!ipod_queue_current(&q, &cur));
  CHECK(!ipod_queue_next(&q, true));

  CHECK(ipod_queue_set(&q, tracks, NULL, 10, 3));
  CHECK(ipod_queue_current(&q, &cur) && cur == 103);
  for (int i = 0; i < 6; i++)
    CHECK(ipod_queue_next(&q, false));
  CHECK(ipod_queue_current(&q, &cur) && cur == 109);
  CHECK(!ipod_queue_next(&q, false)); // end of list, repeat off
  CHECK(ipod_queue_current(&q, &cur) && cur == 109);

  q.repeat = IPOD_REPEAT_ALL;
  CHECK(ipod_queue_next(&q, false));
  CHECK(ipod_queue_current(&q, &cur) && cur == 100);
  CHECK(ipod_queue_prev(&q));
  CHECK(ipod_queue_current(&q, &cur) && cur == 109);

  q.repeat = IPOD_REPEAT_ONE;
  CHECK(ipod_queue_next(&q, false));
  CHECK(ipod_queue_current(&q, &cur) && cur == 109); // held for a natural end
  CHECK(ipod_queue_next(&q, true));
  CHECK(ipod_queue_current(&q, &cur) && cur == 100); // a skip still moves (and wraps)

  q.repeat = IPOD_REPEAT_OFF;
  CHECK(ipod_queue_prev(&q));
  CHECK(ipod_queue_current(&q, &cur) && cur == 100); // stays at the first track

  // Song shuffle: the chosen track plays first, every track exactly once.
  q.shuffle = IPOD_SHUFFLE_SONGS;
  CHECK(ipod_queue_set(&q, tracks, NULL, 10, 6));
  CHECK(ipod_queue_current(&q, &cur) && cur == 106);
  CHECK(is_permutation(&q, tracks, 10));
  bool differs = false;
  for (size_t i = 0; i < 10; i++)
    differs |= q.order[i] != i;
  CHECK(differs);

  // Toggling shuffle keeps the current song.
  CHECK(ipod_queue_next(&q, true));
  CHECK(ipod_queue_current(&q, &cur));
  uint32_t before = cur;
  ipod_queue_set_shuffle(&q, IPOD_SHUFFLE_OFF);
  CHECK(ipod_queue_current(&q, &cur) && cur == before);
  CHECK(q.order[q.pos] == before - 100);
  ipod_queue_set_shuffle(&q, IPOD_SHUFFLE_SONGS);
  CHECK(ipod_queue_current(&q, &cur) && cur == before);
  CHECK(is_permutation(&q, tracks, 10));

  // Shuffle with a random start (main menu "Shuffle Songs").
  CHECK(ipod_queue_set(&q, tracks, NULL, 10, SIZE_MAX));
  CHECK(q.pos == 0 && is_permutation(&q, tracks, 10));

  // Album shuffle keeps albums contiguous and in track order.
  uint32_t groups[10] = {0, 0, 0, 1, 1, 2, 2, 2, 2, 3};
  q.shuffle = IPOD_SHUFFLE_ALBUMS;
  CHECK(ipod_queue_set(&q, tracks, groups, 10, 4));
  CHECK(ipod_queue_current(&q, &cur) && cur == 104);
  CHECK(is_permutation(&q, tracks, 10));
  CHECK(q.order[0] == 3 && q.order[1] == 4); // the chosen album comes first
  int switches = 0;
  for (size_t i = 1; i < 10; i++) {
    if (groups[q.order[i]] != groups[q.order[i - 1]])
      switches++;
    else
      CHECK(q.order[i] == q.order[i - 1] + 1);
  }
  CHECK(switches == 3);

  // Shuffle + repeat all reshuffles on wrap and stays a permutation.
  q.repeat = IPOD_REPEAT_ALL;
  q.shuffle = IPOD_SHUFFLE_SONGS;
  CHECK(ipod_queue_set(&q, tracks, NULL, 10, 0));
  for (int i = 0; i < 25; i++)
    CHECK(ipod_queue_next(&q, false));
  CHECK(is_permutation(&q, tracks, 10));

  CHECK(!ipod_queue_set(&q, tracks, NULL, 0, 0));
  CHECK(!ipod_queue_current(&q, &cur));
  ipod_queue_free(&q);
}

// ---------------------------------------------------------------------------

static double rms_of(const int16_t *x, size_t n) {
  double acc = 0;
  for (size_t i = 0; i < n; i++)
    acc += (double)x[i] * x[i];
  return sqrt(acc / (double)n);
}

static double eq_ratio(int preset, double hz) {
  enum { RATE = 44100, N = RATE };
  static int16_t in[N];
  static int16_t out_eq[N];
  static int16_t out_off[N];
  for (int i = 0; i < N; i++)
    in[i] = (int16_t)(8000.0 * sin(2.0 * M_PI * hz * i / RATE));
  ipod_dsp_t d;
  ipod_dsp_configure(&d, RATE, preset, 0.0f);
  ipod_dsp_process(&d, in, N, 1, out_eq);
  ipod_dsp_configure(&d, RATE, IPOD_EQ_OFF, 0.0f);
  ipod_dsp_process(&d, in, N, 1, out_off);
  return rms_of(out_eq + N / 2, N / 2) / rms_of(out_off + N / 2, N / 2);
}

static void test_dsp(void) {
  ipod_dsp_t d;
  ipod_dsp_configure(&d, 44100, IPOD_EQ_OFF, 0.0f);
  CHECK(!d.use_float && d.decim == 1 && d.out_rate == 44100);
  int16_t st[4] = {100, 200, -50, -150};
  int16_t out[8];
  CHECK(ipod_dsp_process(&d, st, 2, 2, out) == 2);
  CHECK(out[0] == 150 && out[1] == -100);
  int16_t mono_in[3] = {7, -8, 9};
  CHECK(ipod_dsp_process(&d, mono_in, 3, 1, out) == 3);
  CHECK(out[0] == 7 && out[1] == -8 && out[2] == 9);

  ipod_dsp_configure(&d, 96000, IPOD_EQ_OFF, 0.0f);
  CHECK(d.decim == 2 && d.out_rate == 48000);
  int16_t hi[8] = {100, 100, 300, 300, 10, 20, 30, 40};
  CHECK(ipod_dsp_process(&d, hi, 4, 2, out) == 2);
  CHECK(out[0] == 200 && out[1] == 25);
  ipod_dsp_configure(&d, 192000, IPOD_EQ_OFF, 0.0f);
  CHECK(d.decim == 4 && d.out_rate == 48000);
  ipod_dsp_configure(&d, 88200, IPOD_EQ_OFF, 0.0f);
  CHECK(d.decim == 2 && d.out_rate == 44100);
  ipod_dsp_configure(&d, 48000, IPOD_EQ_OFF, 0.0f);
  CHECK(d.decim == 1);

  ipod_dsp_configure(&d, 44100, IPOD_EQ_OFF, 6.0206f);
  CHECK(d.use_float);
  int16_t one[1] = {1000};
  ipod_dsp_process(&d, one, 1, 1, out);
  CHECK(out[0] >= 1995 && out[0] <= 2005);
  int16_t loud[1] = {30000};
  ipod_dsp_process(&d, loud, 1, 1, out);
  CHECK(out[0] == 32767); // clipped, not wrapped

  CHECK(ipod_eq_count() == 23);
  CHECK_STR(ipod_eq_name(0), "Off");
  CHECK_STR(ipod_eq_name(2), "Bass Booster");
  CHECK_STR(ipod_eq_name(99), "Off");
  // Presets are judged by tilt (low vs high gain), since boosting presets also
  // apply pre-gain headroom to the whole signal.
  CHECK(eq_ratio(2, 50.0) / eq_ratio(2, 5000.0) > 1.6);     // Bass Booster
  CHECK(eq_ratio(3, 50.0) / eq_ratio(3, 5000.0) < 0.6);     // Bass Reducer
  CHECK(eq_ratio(20, 10000.0) / eq_ratio(20, 100.0) > 1.6); // Treble Booster
  CHECK(eq_ratio(21, 10000.0) / eq_ratio(21, 100.0) < 0.6); // Treble Reducer
  CHECK(eq_ratio(2, 5000.0) < 1.0); // boosting presets leave headroom
  double flat = eq_ratio(8, 1000.0); // Flat
  CHECK(flat > 0.999 && flat < 1.001);

  ipod_dsp_configure(&d, 44100, IPOD_EQ_OFF, 0.0f);
  int16_t zeros[200] = {0};
  int16_t clicked[200];
  ipod_dsp_click(&d);
  ipod_dsp_process(&d, zeros, 200, 1, clicked);
  CHECK(clicked[0] != 0 && clicked[d.click_len - 1] != 0 && clicked[d.click_len] == 0);
}

// ---------------------------------------------------------------------------

static void test_settings(void) {
  char path[1100];
  snprintf(path, sizeof(path), "%s/settings_test.cfg", g_root);
  ipod_settings_t s;
  ipod_settings_defaults(&s);
  CHECK(s.volume == IPOD_VOLUME_DEFAULT && s.clicker && !s.sound_check);
  s.volume = 42;
  s.shuffle = IPOD_SHUFFLE_ALBUMS;
  s.repeat = IPOD_REPEAT_ONE;
  s.eq_preset = 17;
  s.sound_check = true;
  s.clicker = false;
  s.volume_limit = 80;
  CHECK(ipod_settings_save(&s, path));
  ipod_settings_t r;
  ipod_settings_defaults(&r);
  CHECK(ipod_settings_load(&r, path));
  CHECK(memcmp(&r, &s, sizeof(s)) == 0);

  FILE *f = fopen(path, "w");
  if (f != NULL) {
    fputs("volume=900\nvolume_limit=50\neq=-4\nshuffle=7\nbogus line\n", f);
    fclose(f);
  }
  ipod_settings_defaults(&r);
  CHECK(ipod_settings_load(&r, path));
  CHECK(r.volume == 50 && r.volume_limit == 50 && r.eq_preset == 0);
  CHECK(r.shuffle == IPOD_SHUFFLE_ALBUMS); // clamped to the last valid mode
  remove(path);
  CHECK(!ipod_settings_load(&r, path));
}

// ---------------------------------------------------------------------------
// Decoder bit-exactness against the reference `flac` decoder.

static uint8_t *run_flac_decode(const char *path, size_t *out_len) {
  char cmd[2048];
  snprintf(cmd,
           sizeof(cmd),
           "flac -d -s -c --force-raw-format --endian=little --sign=signed \"%s\"",
           path);
  FILE *p = popen(cmd, "r");
  if (p == NULL)
    return NULL;
  size_t cap = 1 << 20;
  size_t len = 0;
  uint8_t *buf = malloc(cap);
  size_t got;
  while (buf != NULL && (got = fread(buf + len, 1, cap - len, p)) > 0) {
    len += got;
    if (len == cap) {
      cap *= 2;
      buf = realloc(buf, cap);
    }
  }
  pclose(p);
  *out_len = len;
  return buf;
}

static void check_decode_exact(const char *rel) {
  const char *path = lib_path(rel);
  size_t ref_len = 0;
  uint8_t *ref = run_flac_decode(path, &ref_len);
  CHECK(ref != NULL && ref_len > 0);
  drflac *fl = drflac_open_file(path, NULL);
  CHECK(fl != NULL);
  if (fl == NULL || ref == NULL) {
    free(ref);
    drflac_close(fl);
    return;
  }
  const unsigned ch = fl->channels;
  const unsigned bps = fl->bitsPerSample;
  const unsigned bytes = bps / 8;
  const size_t frames = ref_len / (ch * bytes);
  CHECK(frames == fl->totalPCMFrameCount);
  int32_t *pcm = malloc(frames * ch * sizeof(int32_t));
  size_t got = drflac_read_pcm_frames_s32(fl, frames, pcm);
  CHECK(got == frames);
  size_t mismatches = 0;
  for (size_t i = 0; i < got * ch; i++) {
    int32_t r;
    if (bytes == 2) {
      r = (int16_t)(ref[2 * i] | (ref[2 * i + 1] << 8));
    } else {
      r = (int32_t)((uint32_t)ref[3 * i] << 8 | (uint32_t)ref[3 * i + 1] << 16 |
                    (uint32_t)ref[3 * i + 2] << 24) >> 8;
    }
    if ((pcm[i] >> (32 - bps)) != r)
      mismatches++;
  }
  CHECK(mismatches == 0);
  if (mismatches != 0)
    fprintf(stderr, "  %s: %zu mismatching samples\n", rel, mismatches);
  free(pcm);
  free(ref);
  drflac_close(fl);
}

static void test_decode(void) {
  check_decode_exact("Music/The Polygons/Square Waves/01 Triangle Theory.flac");
  check_decode_exact("Music/Compilations/Summer Mix 2005/02 Mono Lake.flac");
  check_decode_exact("Music/Compilations/Summer Mix 2005/03 Hi-Res Horizon.flac");
  check_decode_exact("Music/Odd/ID3 Prefixed.flac");
  CHECK(drflac_open_file(lib_path("Music/Odd/corrupt.flac"), NULL) == NULL);
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s TEST_LIBRARY_DIR\n", argv[0]);
    return 2;
  }
  // Library paths are absolute on the device (/sdcard/...); mirror that.
  if (realpath(argv[1], g_root) == NULL) {
    perror(argv[1]);
    return 2;
  }
  test_text();
  test_meta();
  test_library();
  test_queue();
  test_dsp();
  test_settings();
  test_decode();
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
