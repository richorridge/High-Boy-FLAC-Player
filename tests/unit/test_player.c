// Copyright (c) 2026 High Boy iPod app contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Integration tests for the decoder task and the art worker, run against the
// simulator's audio_i2s. Two modes (one per process, as the audio backend is
// chosen once):
//   test_player fast LIB      decode flat out, capture the speaker output and
//                             compare it sample-for-sample with `flac -d`
//   test_player realtime LIB  real-time pacing; transport controls and timing

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hb_sim.h"
#include "ipod_art.h"
#include "ipod_library.h"
#include "ipod_player.h"

static int g_checks;
static int g_failures;
static char g_root[4096];
static char g_capture[4200];

#define CHECK(cond)                                                                                \
  do {                                                                                             \
    g_checks++;                                                                                    \
    if (!(cond)) {                                                                                 \
      g_failures++;                                                                                \
      fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                     \
    }                                                                                              \
  } while (0)

static const char *lib_path(const char *rel) {
  static char buf[4][4200];
  static int k;
  k = (k + 1) % 4;
  snprintf(buf[k], sizeof(buf[k]), "%s/%s", g_root, rel);
  return buf[k];
}

static void sleep_ms(uint32_t ms) {
  vTaskDelay(pdMS_TO_TICKS(ms));
}

static ipod_player_status_t status(void) {
  ipod_player_status_t st;
  ipod_player_get_status(&st);
  return st;
}

static bool wait_end(uint32_t end_seq_before, uint32_t timeout_ms) {
  for (uint32_t t = 0; t < timeout_ms; t += 10) {
    if (status().end_seq != end_seq_before)
      return true;
    sleep_ms(10);
  }
  return false;
}

static int find_title(const ipod_library_t *lib, const char *title) {
  for (uint32_t i = 0; i < ipod_lib_count(lib); i++) {
    if (strcmp(ipod_lib_str(lib, ipod_lib_track(lib, i)->title), title) == 0)
      return (int)i;
  }
  return -1;
}

static size_t album_tracks(const ipod_library_t *lib, const char *song, uint32_t **out) {
  int i = find_title(lib, song);
  if (i < 0)
    return 0;
  const ipod_track_t *t = ipod_lib_track(lib, (uint32_t)i);
  ipod_filter_t f = {.album = t->album, .album_artist = t->album_artist};
  return ipod_lib_query_tracks(lib, &f, IPOD_ORDER_ALBUM, out);
}

// Reference: `flac -d` -> mono exactly as the app's DSP does it.
static int16_t *reference_mono(const char *path, size_t *out_n) {
  char cmd[4400];
  snprintf(cmd, sizeof(cmd), "metaflac --show-bps --show-channels \"%s\"", path);
  FILE *p = popen(cmd, "r");
  int bps = 0;
  int ch = 0;
  if (p == NULL || fscanf(p, "%d %d", &bps, &ch) != 2) {
    if (p != NULL)
      pclose(p);
    return NULL;
  }
  pclose(p);
  snprintf(cmd,
           sizeof(cmd),
           "flac -d -s -c --force-raw-format --endian=little --sign=signed \"%s\"",
           path);
  p = popen(cmd, "r");
  if (p == NULL)
    return NULL;
  const int bytes = bps / 8;
  const int decim = (bps == 24) ? 2 : 1; // the 24-bit fixture is 96 kHz
  size_t cap = 1 << 16;
  size_t n = 0;
  int16_t *out = malloc(cap * sizeof(int16_t));
  uint8_t frame[64];
  for (;;) {
    int32_t acc = 0;
    int got_all = 1;
    for (int d = 0; d < decim && got_all; d++) {
      if (fread(frame, (size_t)bytes, (size_t)ch, p) != (size_t)ch) {
        got_all = 0;
        break;
      }
      for (int c = 0; c < ch; c++) {
        int32_t s;
        if (bytes == 2) {
          s = (int16_t)(frame[2 * c] | (frame[2 * c + 1] << 8));
        } else {
          int32_t s24 = (int32_t)((uint32_t)frame[3 * c] << 8 | (uint32_t)frame[3 * c + 1] << 16 |
                                  (uint32_t)frame[3 * c + 2] << 24) >> 8;
          s = s24 >> 8; // dr_flac's s16 conversion keeps the top 16 bits
        }
        acc += s;
      }
    }
    if (!got_all)
      break;
    if (n == cap) {
      cap *= 2;
      out = realloc(out, cap * sizeof(int16_t));
    }
    out[n++] = (int16_t)(acc / (decim * ch));
  }
  pclose(p);
  *out_n = n;
  return out;
}

static int16_t *read_capture(size_t *out_n) {
  FILE *f = fopen(g_capture, "rb");
  if (f == NULL)
    return NULL;
  fseek(f, 0, SEEK_END);
  long len = ftell(f);
  fseek(f, 44, SEEK_SET);
  size_t n = len > 44 ? (size_t)(len - 44) / 2 : 0;
  int16_t *s = malloc(n * sizeof(int16_t) + 1);
  if (fread(s, sizeof(int16_t), n, f) != n)
    n = 0;
  fclose(f);
  *out_n = n;
  return s;
}

static void check_capture_matches(size_t from, const char *const *rels, size_t nrels) {
  size_t cap_n = 0;
  int16_t *cap = read_capture(&cap_n);
  CHECK(cap != NULL);
  size_t pos = from;
  size_t mismatches = 0;
  for (size_t r = 0; r < nrels; r++) {
    size_t ref_n = 0;
    int16_t *ref = reference_mono(lib_path(rels[r]), &ref_n);
    CHECK(ref != NULL && ref_n > 0);
    CHECK(pos + ref_n <= cap_n);
    for (size_t i = 0; ref != NULL && i < ref_n && pos + i < cap_n; i++) {
      if (cap[pos + i] != ref[i])
        mismatches++;
    }
    pos += ref_n;
    free(ref);
  }
  CHECK(pos == cap_n); // nothing extra: no gaps, no padding between songs
  CHECK(mismatches == 0);
  if (mismatches != 0 || pos != cap_n)
    fprintf(stderr, "  capture %zu samples, expected %zu, %zu mismatches\n", cap_n, pos, mismatches);
  free(cap);
}

static void test_fast(ipod_library_t *lib) {
  hb_audio_stats_t a0;
  hb_audio_get_stats(&a0);

  uint32_t *tracks = NULL;
  size_t n = album_tracks(lib, "Triangle Theory", &tracks);
  CHECK(n == 4);
  uint32_t end0 = status().end_seq;
  ipod_player_play_list(tracks, NULL, n, 0);
  CHECK(wait_end(end0, 20000));
  free(tracks);
  static const char *const SQUARE[] = {
      "Music/The Polygons/Square Waves/01 Triangle Theory.flac",
      "Music/The Polygons/Square Waves/02 Hexagon Sun.flac",
      "Music/The Polygons/Square Waves/03 Vertex.flac",
      "Music/The Polygons/Square Waves/04 Acute Angles.flac",
  };
  check_capture_matches(0, SQUARE, 4);
  hb_audio_stats_t a1;
  hb_audio_get_stats(&a1);
  CHECK(a1.stream_starts - a0.stream_starts == 1); // one stream for the whole album: gapless

  for (int i = 0; i < 4; i++) {
    int idx = find_title(lib, i == 0 ? "Triangle Theory" : i == 1 ? "Hexagon Sun" : i == 2 ? "Vertex" : "Acute Angles");
    CHECK(idx >= 0 && ipod_lib_track(lib, (uint32_t)idx)->play_count == 1);
  }
  CHECK(ipod_player_take_stats_dirty());
  CHECK(!ipod_player_take_stats_dirty());
  CHECK(status().state == IPOD_PLAYER_STOPPED && !status().has_track);

  // 24-bit / 96 kHz is decimated to 48 kHz; a mono file passes straight through.
  size_t cap_before = 0;
  free(read_capture(&cap_before));
  int hi = find_title(lib, "Hi-Res Horizon");
  int mono = find_title(lib, "Mono Lake");
  CHECK(hi >= 0 && mono >= 0);
  uint32_t list[2] = {(uint32_t)hi, (uint32_t)mono};
  end0 = status().end_seq;
  ipod_player_play_list(list, NULL, 2, 0);
  CHECK(wait_end(end0, 20000));
  static const char *const MIXED[] = {
      "Music/Compilations/Summer Mix 2005/03 Hi-Res Horizon.flac",
      "Music/Compilations/Summer Mix 2005/02 Mono Lake.flac",
  };
  check_capture_matches(cap_before, MIXED, 2);
  hb_audio_get_stats(&a1);
  CHECK(a1.last_rate == 44100);
  CHECK(a1.stream_starts - a0.stream_starts == 3); // 48 kHz then back to 44.1 kHz
  char rates_path[4300];
  snprintf(rates_path, sizeof(rates_path), "%s.rates", g_capture);
  FILE *rf = fopen(rates_path, "r");
  bool saw48 = false;
  unsigned long long off = 0;
  unsigned rate = 0;
  while (rf != NULL && fscanf(rf, "%llu %u", &off, &rate) == 2)
    saw48 |= rate == 48000 && off == cap_before;
  if (rf != NULL)
    fclose(rf);
  CHECK(saw48);
}

static void copy_file(const char *src, const char *dst) {
  FILE *in = fopen(src, "rb");
  FILE *out = fopen(dst, "wb");
  char buf[8192];
  size_t got;
  while (in != NULL && out != NULL && (got = fread(buf, 1, sizeof(buf), in)) > 0)
    fwrite(buf, 1, got, out);
  if (in != NULL)
    fclose(in);
  if (out != NULL)
    fclose(out);
}

static void test_realtime(ipod_library_t *lib) {
  uint32_t *tracks = NULL;
  size_t n = album_tracks(lib, "Triangle Theory", &tracks);
  CHECK(n == 4);
  if (n != 4)
    return;
  ipod_player_play_list(tracks, NULL, n, 0);
  sleep_ms(500);
  ipod_player_status_t st = status();
  CHECK(st.state == IPOD_PLAYER_PLAYING && st.has_track && st.track == tracks[0]);
  CHECK(st.pos_ms >= 200 && st.pos_ms <= 900);
  CHECK(st.queue_count == 4 && st.queue_pos == 0);
  CHECK(st.dur_ms == 4000);
  CHECK(st.out_rate == 44100);

  ipod_player_set_paused(true);
  sleep_ms(150);
  uint32_t paused_at = status().pos_ms;
  sleep_ms(400);
  st = status();
  CHECK(st.state == IPOD_PLAYER_PAUSED);
  CHECK(st.pos_ms == paused_at);

  ipod_player_toggle_pause();
  ipod_player_seek_to(3000);
  sleep_ms(150);
  st = status();
  CHECK(st.state == IPOD_PLAYER_PLAYING);
  CHECK(st.pos_ms >= 3000 && st.pos_ms < 3500);

  uint32_t seq = st.track_seq;
  ipod_player_prev(); // past 3 s: restart the song
  sleep_ms(100);
  st = status();
  CHECK(st.queue_pos == 0 && st.track_seq == seq && st.pos_ms < 600);

  ipod_player_prev(); // near the start of the first song: stays on it
  sleep_ms(100);
  st = status();
  CHECK(st.queue_pos == 0 && st.track == tracks[0]);

  ipod_player_next();
  sleep_ms(150);
  st = status();
  CHECK(st.queue_pos == 1 && st.track == tracks[1] && st.pos_ms < 600);

  ipod_player_seek_by(10000); // fast-forward off the end goes to the next song
  sleep_ms(150);
  st = status();
  CHECK(st.queue_pos == 2 && st.track == tracks[2]);

  ipod_player_seek_by(-60000); // rewinding clamps at 0
  sleep_ms(100);
  CHECK(status().queue_pos == 2 && status().pos_ms < 600);

  ipod_player_set_repeat(IPOD_REPEAT_ONE);
  ipod_player_seek_to(3700);
  seq = status().track_seq;
  sleep_ms(700);
  st = status();
  CHECK(st.queue_pos == 2 && st.track == tracks[2] && st.track_seq == seq + 1);
  CHECK(st.repeat == IPOD_REPEAT_ONE);
  ipod_player_set_repeat(IPOD_REPEAT_OFF);

  ipod_player_set_shuffle(IPOD_SHUFFLE_SONGS);
  st = status();
  CHECK(st.track == tracks[2] && st.queue_pos == 0 && st.shuffle == IPOD_SHUFFLE_SONGS);
  uint32_t *q = NULL;
  CHECK(ipod_player_copy_queue(&q) == 4 && q[0] == tracks[2]);
  free(q);
  ipod_player_set_shuffle(IPOD_SHUFFLE_OFF);
  st = status();
  CHECK(st.track == tracks[2] && st.queue_pos == 2);

  // Clicker: mixed into the stream while playing, a UI click otherwise.
  hb_audio_stats_t a0;
  hb_audio_stats_t a1;
  hb_audio_get_stats(&a0);
  ipod_player_click();
  hb_audio_get_stats(&a1);
  CHECK(a1.ui_clicks == a0.ui_clicks);

  ipod_player_next();
  sleep_ms(100);
  uint32_t end0 = status().end_seq;
  ipod_player_next(); // past the last song with repeat off: the list ends
  sleep_ms(100);
  st = status();
  CHECK(st.state == IPOD_PLAYER_STOPPED && !st.has_track && st.end_seq == end0 + 1);
  sleep_ms(200);
  hb_audio_get_stats(&a0);
  CHECK(!a0.streaming);
  ipod_player_click();
  hb_audio_get_stats(&a1);
  CHECK(a1.ui_clicks == a0.ui_clicks + 1);

  // Play/Pause after the list ended restarts it.
  ipod_player_set_paused(false);
  sleep_ms(200);
  CHECK(status().state == IPOD_PLAYER_PLAYING && status().has_track);
  ipod_player_stop();
  sleep_ms(100);
  CHECK(status().state == IPOD_PLAYER_STOPPED);
  free(tracks);

  // Unplayable songs are skipped; an all-broken list just stops.
  char dir[4300];
  snprintf(dir, sizeof(dir), "%s/broken_lib", g_root);
  mkdir(dir, 0755);
  char a[4400];
  char b[4400];
  snprintf(a, sizeof(a), "%s/a.flac", dir);
  snprintf(b, sizeof(b), "%s/b.flac", dir);
  copy_file(lib_path("Music/The Polygons/Square Waves/01 Triangle Theory.flac"), a);
  copy_file(lib_path("Music/The Polygons/Square Waves/02 Hexagon Sun.flac"), b);
  ipod_library_t *broken = ipod_lib_scan(dir, NULL, NULL);
  CHECK(broken != NULL && ipod_lib_count(broken) == 2);
  if (broken != NULL && ipod_lib_count(broken) == 2) {
    ipod_player_set_library(broken);
    FILE *f = fopen(a, "wb");
    if (f != NULL)
      fclose(f);
    uint32_t ia = 0;
    uint32_t ib = 0;
    CHECK(ipod_lib_find_path(broken, a, &ia) && ipod_lib_find_path(broken, b, &ib));
    uint32_t pair[2] = {ia, ib};
    uint32_t err0 = status().error_seq;
    ipod_player_play_list(pair, NULL, 2, 0);
    sleep_ms(300);
    st = status();
    CHECK(st.error_seq == err0 + 1);
    CHECK(st.state == IPOD_PLAYER_PLAYING && st.track == ib && st.queue_pos == 1);
    f = fopen(b, "wb");
    if (f != NULL)
      fclose(f);
    end0 = st.end_seq;
    ipod_player_play_list(pair, NULL, 2, 0);
    sleep_ms(300);
    st = status();
    CHECK(st.state == IPOD_PLAYER_STOPPED && st.end_seq == end0 + 1);
    ipod_player_set_library(lib);
  }
  ipod_lib_destroy(broken);
  remove(a);
  remove(b);
  rmdir(dir);
}

static void test_art(void) {
  ipod_art_image_t img;
  const char *tri = lib_path("Music/The Polygons/Square Waves/01 Triangle Theory.flac");
  ipod_library_t *lib = ipod_lib_scan(lib_path("Music"), NULL, NULL);
  CHECK(lib != NULL);
  int t = find_title(lib, "Triangle Theory");
  int polar = find_title(lib, "Polar Night");
  int etude = find_title(lib, "Espresso \xC3\x89tude");
  int cava = find_title(lib, "\xC3\x87"
                             "a Va");
  CHECK(t >= 0 && polar >= 0 && etude >= 0 && cava >= 0);
  if (t < 0 || polar < 0 || etude < 0 || cava < 0) {
    ipod_lib_destroy(lib);
    return;
  }
  const ipod_track_t *tt = ipod_lib_track(lib, (uint32_t)t);
  CHECK(ipod_art_decode(tri, tt->picture_offset, tt->picture_size, 120, 30, 0xFFFF, &img));
  CHECK(img.w == 120 && img.h == 120 && img.pixels != NULL);
  CHECK(img.refl_h == 30 && img.reflection != NULL);
  // The reflection's first row is a faded copy of the art's last row.
  CHECK(img.reflection[0] != 0xFFFF);
  ipod_art_image_free(&img);
  CHECK(img.pixels == NULL);

  const ipod_track_t *pt = ipod_lib_track(lib, (uint32_t)polar);
  CHECK(ipod_art_decode(ipod_lib_str(lib, pt->path), pt->picture_offset, pt->picture_size, 140, 0,
                        0xFFFF, &img)); // progressive JPEG
  CHECK(img.w == 140 && img.h == 140 && img.reflection == NULL);
  ipod_art_image_free(&img);

  const ipod_track_t *et = ipod_lib_track(lib, (uint32_t)etude);
  CHECK(et->picture_size == 0);
  CHECK(ipod_art_decode(ipod_lib_str(lib, et->path), 0, 0, 64, 8, 0xFFFF, &img)); // cover.jpg
  CHECK(img.w == 64 && img.h == 64);
  ipod_art_image_free(&img);

  const ipod_track_t *ct = ipod_lib_track(lib, (uint32_t)cava);
  CHECK(!ipod_art_decode(ipod_lib_str(lib, ct->path), 0, 0, 64, 8, 0xFFFF, &img));
  CHECK(img.pixels == NULL);
  // A bogus offset must fail cleanly rather than decode garbage.
  CHECK(!ipod_art_decode(tri, 4, tt->picture_size, 64, 8, 0xFFFF, &img));

  CHECK(ipod_art_start() == ESP_OK);
  uint32_t first = ipod_art_request(tri, tt->picture_offset, tt->picture_size, 100, 20, 0xFFFF);
  uint32_t id = ipod_art_request(ipod_lib_str(lib, pt->path), pt->picture_offset, pt->picture_size,
                                 100, 20, 0xFFFF);
  CHECK(first != 0 && id != first);
  bool got = false;
  for (int i = 0; i < 300 && !got; i++) {
    got = ipod_art_take(id, &img);
    if (!got)
      sleep_ms(10);
  }
  CHECK(got && img.w == 100 && img.pixels != NULL);
  ipod_art_image_free(&img);
  CHECK(!ipod_art_take(first, &img)); // superseded
  uint32_t none = ipod_art_request(ipod_lib_str(lib, ct->path), 0, 0, 100, 20, 0xFFFF);
  got = false;
  for (int i = 0; i < 300 && !got; i++) {
    got = ipod_art_take(none, &img);
    if (!got)
      sleep_ms(10);
  }
  CHECK(got && img.pixels == NULL); // "no art" is delivered too
  ipod_art_shutdown();
  ipod_lib_destroy(lib);
}

int main(int argc, char **argv) {
  if (argc < 3) {
    fprintf(stderr, "usage: %s fast|realtime LIBRARY_DIR\n", argv[0]);
    return 2;
  }
  if (realpath(argv[2], g_root) == NULL) {
    perror(argv[2]);
    return 2;
  }
  const bool fast = strcmp(argv[1], "fast") == 0;
  snprintf(g_capture, sizeof(g_capture), "%s/capture_%d.wav", g_root, (int)getpid());
  setenv("HB_AUDIO", fast ? "fast" : "null", 1);
  if (fast)
    setenv("HB_AUDIO_CAPTURE", g_capture, 1);

  ipod_library_t *lib = ipod_lib_scan(lib_path("Music"), NULL, NULL);
  CHECK(lib != NULL);
  if (lib == NULL)
    return 1;
  CHECK(ipod_player_start(lib) == ESP_OK);
  ipod_player_set_volume(100);
  if (fast) {
    test_fast(lib);
  } else {
    test_realtime(lib);
    test_art();
  }
  ipod_player_shutdown();
  ipod_lib_destroy(lib);
  if (fast) {
    char rates[4300];
    snprintf(rates, sizeof(rates), "%s.rates", g_capture);
    remove(g_capture);
    remove(rates);
  }
  printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
