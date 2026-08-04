set -e

python gen.py

ROOT=/media/jamesb/CIRCUITPY/
ROOT=/Volumes/CIRCUITPY
rm -rf image
mkdir image image/lib/
cp an.py  image/
cp announcer.py  image/code.py
rsync -rv --checksum image/ $ROOT/
sync
