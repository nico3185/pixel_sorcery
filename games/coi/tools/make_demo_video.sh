#!/bin/zsh
# make_demo_video.sh ROM POLLS [FROM_POLL] — feature video of the native port, rendered
# headless from a recorded play session: the same gameplay in 4:3, 16:9 and 21:9 (sound
# from the port's own synth), and a tour of the settings menu. Output: build/demo/demo.mp4
set -eu
here=${0:A:h}/..
rom=${1:A}; polls=${2:A}; from=${3:-1200}
cd $here
out=build/demo; rm -rf $out; mkdir -p $out
py=../../engine/.venv/bin/python
exe=./build-video/coi
[[ -x $exe ]] || { cmake -S . -B build-video -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null; ninja -C build-video coi >/dev/null; }
len=1500                                   # frames per clip (25 s)

clip() {  # name aspect [extra env]
  mkdir -p $out/$1
  env PS_WIDE=$2 PS_POLL_REPLAY=$polls PS_FRAMES_DIR=$out/$1 PS_FRAMES_FROM=$from PS_FRAMES_TO=$((from + len - 1)) \
      PS_WAV=$out/$1.wav ${@:3} $exe --headless --frames $((from + len)) $rom >/dev/null 2>&1
  # audio of the clip's frames only (44100/59.92 samples per frame)
  ffmpeg -loglevel error -y -i $out/$1.wav -ss $(python3 -c "print($from/59.9227)") -t $(python3 -c "print($len/59.9227)") $out/$1.cut.wav
}
clip narrow 4:3
clip wide 16:9
clip ultra 21:9
clip menu 16:9 PS_MENU_DEMO=1

# title cards (text rendered with Pillow's built-in font)
$py - $out <<'PY'
import sys
from PIL import Image, ImageDraw, ImageFont
out = sys.argv[1]
cards = {
  'title': ['CASTLE OF ILLUSION', 'native Mac port - PixelSorcery', 'recompiled 68000 + Z80 code, own video and sound, no emulation'],
  'narrow': ['1. Original 4:3', 'cycle-exact: identical to the original game, poll by poll'],
  'wide': ['2. 16:9 widescreen', 'level edges from a per-room world cache, enemies live past the edges'],
  'ultra': ['3. 21:9', 'any aspect ratio: 16:10, 16:9, 21:9, 32:9'],
  'menu': ['4. Settings menu (F1 / Tab / gamepad Back)', 'aspect, widescreen enemies, latency, volume, fullscreen, cheats'],
}
font = ImageFont.load_default()
for name, lines in cards.items():
    im = Image.new('RGB', (1280, 720), (12, 12, 24)); d = ImageDraw.Draw(im)
    y = 300
    for i, t in enumerate(lines):
        big = font.font_variant(size=44 if i == 0 else 24) if hasattr(font, 'font_variant') else font
        w = d.textlength(t, font=big); d.text(((1280 - w) / 2, y), t, fill=(255, 224, 112) if i == 0 else (200, 200, 220), font=big)
        y += 70 if i == 0 else 40
    im.save(f'{out}/card_{name}.png')
PY

seg() {  # name: title card (3 s) then the clip scaled to 720p, letterboxed
  ffmpeg -loglevel error -y -loop 1 -t 3 -i $out/card_$1.png -f lavfi -t 3 -i anullsrc=r=44100:cl=stereo \
    -vf "fps=60,format=yuv420p" -c:v libx264 -crf 18 -c:a aac -shortest $out/s_card_$1.mp4
  # frames can change width mid-clip (4:3 menus, wide gameplay): ffmpeg would size the
  # video from the first frame and squash the rest, so pad each to the widest first
  local mw=$($py -c "import glob,sys;from PIL import Image;print(max(Image.open(f).size[0] for f in glob.glob(sys.argv[1]+'/f*.png')))" $out/$1)
  ffmpeg -loglevel error -y -framerate 59.9227 -i $out/$1/f%06d.png -i $out/$1.cut.wav \
    -vf "pad=$mw:224:(ow-iw)/2:0:color=black,scale=1280:672:force_original_aspect_ratio=decrease:force_divisible_by=2:flags=neighbor,pad=1280:720:(ow-iw)/2:(oh-ih)/2:color=0x0c0c18,fps=60,format=yuv420p" \
    -c:v libx264 -crf 18 -c:a aac -shortest $out/s_clip_$1.mp4
}
ffmpeg -loglevel error -y -loop 1 -t 4 -i $out/card_title.png -f lavfi -t 4 -i anullsrc=r=44100:cl=stereo \
  -vf "fps=60,format=yuv420p" -c:v libx264 -crf 18 -c:a aac -shortest $out/s_title.mp4
: > $out/list.txt; print "file 's_title.mp4'" >> $out/list.txt
for s in narrow wide ultra menu; do seg $s; print "file 's_card_$s.mp4'\nfile 's_clip_$s.mp4'" >> $out/list.txt; done
ffmpeg -loglevel error -y -f concat -safe 0 -i $out/list.txt -c copy $out/demo.mp4
print "video: $out/demo.mp4"
