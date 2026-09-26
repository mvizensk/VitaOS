#!/bin/sh
# frames from logo.py -> home/boot/boot.mp4 (H.264 baseline, 960x544, what AvPlayer decodes)
# usage: home/boot/encode.sh <frames_dir>
ffmpeg -v error -y -framerate 30 -i "$1/f%04d.png" -c:v libx264 -profile:v baseline -level 3.1 \
  -pix_fmt yuv420p -crf 18 -g 30 -movflags +faststart "$(dirname "$0")/boot.mp4"
