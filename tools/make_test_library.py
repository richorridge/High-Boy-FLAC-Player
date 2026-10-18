#!/usr/bin/env python3
# Copyright (c) 2026 High Boy FLAC iPod contributors
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generate an SD-card image folder full of FLAC files for the simulator/tests.

All music is synthesised here (simple melodies over a bass line), artists and
albums are fictional, and the cover art is drawn with Pillow, so the library
can be redistributed freely. The set deliberately exercises edge cases:
embedded PNG / progressive-JPEG art, folder cover.jpg, accented tags, a
compilation, mono, 24-bit/96 kHz, ReplayGain, composer tags, an untagged file,
a corrupt .flac, an ID3-prefixed .flac and an .m3u playlist.

Usage: make_test_library.py OUT_DIR [--seconds N]
Requires: numpy, Pillow and the `flac` command-line encoder.
"""

import argparse
import io
import os
import shutil
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw

NOTE_NAMES = {"C": -9, "D": -7, "E": -5, "F": -4, "G": -2, "A": 0, "B": 2}
SCALES = {
    "major": [0, 2, 4, 5, 7, 9, 11],
    "minor": [0, 2, 3, 5, 7, 8, 10],
    "penta": [0, 2, 4, 7, 9],
}

# (relative path, tags, synth params, art)
#   art: ("embed", kind) | ("folder", kind) | None ; kind = png | jpeg | jpeg-progressive
TRACKS = [
    # --- The Polygons / Square Waves (embedded PNG, Electronic) ---------------
    ("Music/The Polygons/Square Waves/01 Triangle Theory.flac",
     dict(TITLE="Triangle Theory", ARTIST="The Polygons", ALBUM="Square Waves",
          GENRE="Electronic", DATE="2004", TRACKNUMBER="1", COMPOSER="Ada Vector"),
     dict(root=0, scale="major", bpm=128, wave="square", seed=1), ("embed", "png")),
    ("Music/The Polygons/Square Waves/02 Hexagon Sun.flac",
     dict(TITLE="Hexagon Sun", ARTIST="The Polygons", ALBUM="Square Waves",
          GENRE="Electronic", DATE="2004", TRACKNUMBER="2", COMPOSER="Ada Vector"),
     dict(root=2, scale="penta", bpm=120, wave="square", seed=2), ("embed", "png")),
    ("Music/The Polygons/Square Waves/03 Vertex.flac",
     dict(TITLE="Vertex", ARTIST="The Polygons", ALBUM="Square Waves",
          GENRE="Electronic", DATE="2004", TRACKNUMBER="3"),
     dict(root=5, scale="minor", bpm=132, wave="saw", seed=3), ("embed", "png")),
    ("Music/The Polygons/Square Waves/04 Acute Angles.flac",
     dict(TITLE="Acute Angles", ARTIST="The Polygons", ALBUM="Square Waves",
          GENRE="Electronic", DATE="2004", TRACKNUMBER="4"),
     dict(root=7, scale="major", bpm=124, wave="square", seed=4), ("embed", "png")),
    # --- Aurora Vale (progressive JPEG embedded; folder art; accents) --------
    ("Music/Aurora Vale/Northern Lights/1-01 Polar Night.flac",
     dict(TITLE="Polar Night", ARTIST="Aurora Vale", ALBUM="Northern Lights",
          GENRE="Ambient", DATE="2006-11-02", TRACKNUMBER="1/3", DISCNUMBER="1/1",
          REPLAYGAIN_TRACK_GAIN="+6.00 dB", REPLAYGAIN_ALBUM_GAIN="+4.50 dB"),
     dict(root=-3, scale="minor", bpm=70, wave="sine", seed=5, level=0.25),
     ("embed", "jpeg-progressive")),
    ("Music/Aurora Vale/Northern Lights/1-02 Magnetosphere.flac",
     dict(TITLE="Magnetosphere", ARTIST="Aurora Vale", ALBUM="Northern Lights",
          GENRE="Ambient", DATE="2006", TRACKNUMBER="2/3", DISCNUMBER="1/1",
          REPLAYGAIN_TRACK_GAIN="-3.20 dB"),
     dict(root=-1, scale="minor", bpm=76, wave="sine", seed=6),
     ("embed", "jpeg-progressive")),
    ("Music/Aurora Vale/Northern Lights/1-03 Solar Wind.flac",
     dict(TITLE="Solar Wind", ARTIST="Aurora Vale", ALBUM="Northern Lights",
          GENRE="Ambient", DATE="2006", TRACKNUMBER="3/3", DISCNUMBER="1/1"),
     dict(root=0, scale="penta", bpm=82, wave="triangle", seed=7),
     ("embed", "jpeg-progressive")),
    ("Music/Aurora Vale/Café Nocturne/01 Espresso Étude.flac",
     dict(TITLE="Espresso Étude", ARTIST="Aurora Vale", ALBUM="Café Nocturne",
          GENRE="Jazz", DATE="2008", TRACKNUMBER="1", COMPOSER="J. S. Bachelor"),
     dict(root=3, scale="major", bpm=96, wave="triangle", seed=8), ("folder", "jpeg")),
    ("Music/Aurora Vale/Café Nocturne/02 Last Order.flac",
     dict(TITLE="Last Order", ARTIST="Aurora Vale", ALBUM="Café Nocturne",
          GENRE="Jazz", DATE="2008", TRACKNUMBER="2", COMPOSER="J. S. Bachelor"),
     dict(root=1, scale="minor", bpm=88, wave="triangle", seed=9), ("folder", "jpeg")),
    # --- Zoë Brontë (UTF-8 everywhere, no art) --------------------------------
    ("Music/Zoë Brontë/Électrique/01 Ça Va.flac",
     dict(TITLE="Ça Va", ARTIST="Zoë Brontë", ALBUM="Électrique", GENRE="Pop",
          DATE="2011", TRACKNUMBER="1"),
     dict(root=4, scale="major", bpm=110, wave="saw", seed=10), None),
    ("Music/Zoë Brontë/Électrique/02 Smörgåsbord.flac",
     dict(TITLE="Smörgåsbord", ARTIST="Zoë Brontë", ALBUM="Électrique", GENRE="Pop",
          DATE="2011", TRACKNUMBER="2"),
     dict(root=6, scale="penta", bpm=116, wave="saw", seed=11), None),
    # --- Compilation: mono, 24-bit/96k, two other artists ---------------------
    ("Music/Compilations/Summer Mix 2005/01 Byte Me.flac",
     dict(TITLE="Byte Me", ARTIST="DJ Kilobyte", ALBUM="Summer Mix 2005",
          GENRE="Electronic", DATE="2005", TRACKNUMBER="1", COMPILATION="1"),
     dict(root=2, scale="minor", bpm=140, wave="square", seed=12), ("embed", "jpeg")),
    ("Music/Compilations/Summer Mix 2005/02 Mono Lake.flac",
     dict(TITLE="Mono Lake", ARTIST="Mono Lake", ALBUM="Summer Mix 2005",
          GENRE="Ambient", DATE="2005", TRACKNUMBER="2", COMPILATION="1"),
     dict(root=-2, scale="penta", bpm=90, wave="sine", seed=13, channels=1),
     ("embed", "jpeg")),
    ("Music/Compilations/Summer Mix 2005/03 Hi-Res Horizon.flac",
     dict(TITLE="Hi-Res Horizon", ARTIST="The Hi-Res Unit", ALBUM="Summer Mix 2005",
          GENRE="Electronic", DATE="2005", TRACKNUMBER="3", COMPILATION="1"),
     dict(root=5, scale="major", bpm=118, wave="saw", seed=14, rate=96000, bits=24),
     ("embed", "jpeg")),
    # --- Odd ones -------------------------------------------------------------
    ("Music/loose_track.flac", {}, dict(root=0, scale="major", bpm=100, wave="sine", seed=15),
     None),
    ("Music/Odd/ID3 Prefixed.flac",
     dict(TITLE="ID3 Prefixed", ARTIST="Tag Tester", ALBUM="Edge Cases", GENRE="Test",
          TRACKNUMBER="1"),
     dict(root=3, scale="minor", bpm=100, wave="square", seed=16), None),
]

PLAYLIST = """#EXTM3U
#EXTINF:-1,The Polygons - Hexagon Sun
The Polygons/Square Waves/02 Hexagon Sun.flac
Aurora Vale\\Northern Lights\\1-02 Magnetosphere.flac
/sdcard/Music/Zoë Brontë/Électrique/01 Ça Va.flac
./Compilations/Summer Mix 2005/02 Mono Lake.flac
Nowhere/Missing Song.flac
"""

PALETTES = [
    ((240, 80, 60), (250, 200, 60)),
    ((40, 70, 160), (90, 200, 220)),
    ((30, 120, 80), (220, 230, 120)),
    ((120, 40, 140), (250, 140, 200)),
]


def note_freq(semitones_from_a4):
    return 440.0 * 2 ** (semitones_from_a4 / 12.0)


def oscillator(kind, phase):
    if kind == "square":
        return np.sign(np.sin(phase)) * 0.6
    if kind == "saw":
        return (2 * ((phase / (2 * np.pi)) % 1.0) - 1) * 0.6
    if kind == "triangle":
        return 2 * np.abs(2 * ((phase / (2 * np.pi)) % 1.0) - 1) - 1
    return np.sin(phase)


def synth(seconds, root=0, scale="major", bpm=120, wave="sine", seed=0, rate=44100,
          channels=2, level=0.5, **_):
    rng = np.random.default_rng(seed)
    n = int(seconds * rate)
    t = np.arange(n) / rate
    out = np.zeros(n)
    beat = 60.0 / bpm / 2  # eighth notes
    steps = SCALES[scale]
    pos = 0.0
    degree = 0
    while pos < seconds:
        degree = int(np.clip(degree + rng.integers(-2, 3), -3, 9))
        octave, idx = divmod(degree, len(steps))
        semis = root + steps[idx] + 12 * octave
        dur = beat * rng.choice([1, 1, 2])
        a, b = int(pos * rate), min(n, int((pos + dur) * rate))
        seg_t = t[a:b] - pos
        env = np.minimum(1, seg_t / 0.01) * np.exp(-seg_t * 3.0)
        out[a:b] += 0.5 * oscillator(wave, 2 * np.pi * note_freq(semis) * seg_t) * env
        pos += dur
    bass = 0.35 * np.sin(2 * np.pi * note_freq(root - 24) * t) * (0.6 + 0.4 * np.sin(2 * np.pi * t * bpm / 120))
    out = (out + bass) * level
    fade = np.minimum(1, np.minimum(t / 0.05, (seconds - t) / 0.3))
    out *= np.clip(fade, 0, 1)
    if channels == 2:
        pan = 0.15 * np.sin(2 * np.pi * 0.25 * t)
        out = np.stack([out * (1 - pan), out * (1 + pan)], axis=1)
    else:
        out = out[:, None]
    return np.clip(out, -1, 1)


def write_wav(path, samples, rate, bits):
    scale = 2 ** (bits - 1) - 1
    ints = np.round(samples * scale).astype(np.int32)
    with wave.open(path, "wb") as w:
        w.setnchannels(samples.shape[1])
        w.setsampwidth(bits // 8)
        w.setframerate(rate)
        if bits == 16:
            w.writeframes(ints.astype("<i2").tobytes())
        else:  # 24-bit little-endian packed
            b = ints.astype("<i4").tobytes()
            packed = bytearray()
            for i in range(0, len(b), 4):
                packed += b[i:i + 3]
            w.writeframes(bytes(packed))


def draw_cover(title, artist, palette_idx, size=480):
    c1, c2 = PALETTES[palette_idx % len(PALETTES)]
    img = Image.new("RGB", (size, size))
    d = ImageDraw.Draw(img)
    for y in range(size):
        f = y / (size - 1)
        d.line([(0, y), (size, y)], fill=tuple(int(c1[i] * (1 - f) + c2[i] * f) for i in range(3)))
    for k in range(6):
        r = size * (0.42 - k * 0.06)
        d.ellipse([size / 2 - r, size / 2 - r, size / 2 + r, size / 2 + r],
                  outline=(255, 255, 255), width=4)
    d.rectangle([0, size * 0.78, size, size], fill=(0, 0, 0))
    d.text((size * 0.05, size * 0.81), title.encode("ascii", "replace").decode(), fill=(255, 255, 255))
    d.text((size * 0.05, size * 0.89), artist.encode("ascii", "replace").decode(), fill=(200, 200, 200))
    return img


def save_image(img, path, kind):
    if kind == "png":
        img.save(path, "PNG")
    elif kind == "jpeg-progressive":
        img.save(path, "JPEG", quality=85, progressive=True)
    else:
        img.save(path, "JPEG", quality=85)


BULK_WORDS = ["Amber", "Blue", "Cobalt", "Delta", "Echo", "Falcon", "Golden", "Harbor",
              "Indigo", "Jade", "Kepler", "Lunar", "Meadow", "Neon", "Orbit", "Prism",
              "Quartz", "River", "Silver", "Tidal", "Ultra", "Velvet", "Willow", "Xenon",
              "Yonder", "Zephyr", "Night", "Signal", "Static", "Summer", "Winter", "Fire"]


def make_bulk(root, tmp, n):
    rng = np.random.default_rng(1234)
    wav = os.path.join(tmp, "bulk.wav")
    write_wav(wav, synth(0.4, seed=99), 44100, 16)
    for i in range(n):
        title = f"{rng.choice(BULK_WORDS)} {rng.choice(BULK_WORDS)}"
        if i % 7 == 0:
            title = f"{i} {title}"  # some titles start with digits (sorted under #)
        artist = f"{rng.choice(BULK_WORDS)} Band"
        album = f"{rng.choice(BULK_WORDS)} Sessions"
        dst = os.path.join(root, "Music", "Bulk", artist, album, f"{i:04d}.flac")
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        subprocess.run(["flac", "--silent", "--force", "-0", "-o", dst,
                        "-T", f"TITLE={title}", "-T", f"ARTIST={artist}",
                        "-T", f"ALBUM={album}", "-T", f"TRACKNUMBER={i % 12 + 1}",
                        "-T", "GENRE=Bulk", wav], check=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--seconds", type=float, default=20.0)
    ap.add_argument("--bulk", type=int, default=0,
                    help="also add N short tracks (long lists, fast scrolling)")
    args = ap.parse_args()
    if shutil.which("flac") is None:
        sys.exit("the `flac` encoder is required (apt install flac)")

    root = os.path.abspath(args.out)
    tmp = os.path.join(root, ".gen_tmp")
    os.makedirs(tmp, exist_ok=True)
    for i, (rel, tags, params, art) in enumerate(TRACKS):
        dst = os.path.join(root, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        rate = params.get("rate", 44100)
        bits = params.get("bits", 16)
        samples = synth(args.seconds, **params)
        wav = os.path.join(tmp, "t.wav")
        write_wav(wav, samples, rate, bits)
        cmd = ["flac", "--silent", "--force", "-5", "-o", dst]
        for k, v in tags.items():
            cmd += ["-T", f"{k}={v}"]
        if art is not None:
            where, kind = art
            img = draw_cover(tags.get("ALBUM", "?"), tags.get("ARTIST", "?"), i // 4)
            if where == "embed":
                ext = "png" if kind == "png" else "jpg"
                pic = os.path.join(tmp, f"cover.{ext}")
                save_image(img, pic, kind)
                cmd += [f"--picture=3||Front Cover||{pic}"]
            else:
                save_image(img, os.path.join(os.path.dirname(dst), "cover.jpg"), kind)
        cmd.append(wav)
        subprocess.run(cmd, check=True)
        if "ID3 Prefixed" in rel:
            # Prepend a minimal ID3v2.3 tag (10-byte header + 20 bytes padding).
            with open(dst, "rb") as f:
                body = f.read()
            with open(dst, "wb") as f:
                f.write(b"ID3\x03\x00\x00\x00\x00\x00\x14" + b"\x00" * 20 + body)

    if args.bulk > 0:
        make_bulk(root, tmp, args.bulk)

    music = os.path.join(root, "Music")
    with open(os.path.join(music, "Road Trip.m3u"), "w", encoding="utf-8") as f:
        f.write(PLAYLIST)
    with open(os.path.join(music, "Odd", "notes.txt"), "w") as f:
        f.write("not music\n")
    with open(os.path.join(music, "Odd", "corrupt.flac"), "wb") as f:
        f.write(b"this is not a flac file" * 10)
    # TentacleOS's own folders must be ignored by the scanner if the card root is scanned.
    os.makedirs(os.path.join(root, "config"), exist_ok=True)
    shutil.rmtree(tmp)
    print(f"wrote {len(TRACKS)} tracks to {root}")


if __name__ == "__main__":
    main()
