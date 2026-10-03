# Host test data

Synthetic streams for `test_mp2_audio.c`, `test_cadence.c` and `test_convert.c`, made with ffmpeg
8.1 (sine tones and test patterns; nothing from the game). From this folder:

```sh
# 1 kHz left, 3 kHz right, each at half of full scale, 0.5 s, MP2 in an MPEG-1 program stream
ffmpeg -f lavfi -i "sine=frequency=1000:sample_rate=44100:duration=0.5" \
       -f lavfi -i "sine=frequency=3000:sample_rate=44100:duration=0.5" \
       -filter_complex "[0][1]amerge=inputs=2,volume=4" -c:a mp2 -b:a 192k -f mpeg tone_stereo_192k.mpg
#   ... the same with -b:a 128k: tone_stereo_128k.mpg
# a mono 15 kHz tone at half of full scale, 64 kbit/s
ffmpeg -f lavfi -i "sine=frequency=15000:sample_rate=44100:duration=0.5" \
       -af volume=4 -ac 1 -c:a mp2 -b:a 64k -f mpeg tone_mono_15k.mpg

# the references: ffmpeg's float decoder, mixed to mono, resampled to 22,050 Hz, 16-bit
for f in tone_stereo_192k tone_stereo_128k; do
  ffmpeg -c:a mp2float -i $f.mpg \
         -af "pan=mono|c0=0.5*c0+0.5*c1,aresample=22050:filter_size=128:cutoff=0.97:phase_shift=10" \
         -f s16le $f.ref.s16
done
```

The references are another decoder's reading of the same streams, so the test checks the
firmware's decoder, not the encoder (ffmpeg's encoder leaves its own artifacts on loud pure
tones: they are in both).

A video clip with B pictures and open groups for `test_cadence.c` and `test_convert.c`, at the
game's size and picture rate, and the order ffprobe shows its pictures in:

```sh
ffmpeg -f lavfi -i "testsrc=size=352x240:rate=30000/1001:duration=2" \
       -c:v mpeg1video -bf 2 -g 15 -q:v 20 -f mpeg ibp_352x240.mpg
ffprobe -v error -select_streams v:0 -show_frames -show_entries frame=pict_type -of csv=p=0 \
        ibp_352x240.mpg | cut -c1 | tr -d '\n' > ibp_352x240.types
```

A still clip for `test_convert.c` (one group, so every picture after the first P picture is the
same):

```sh
ffmpeg -f lavfi -i "smptebars=size=352x240:rate=30000/1001:duration=1" \
       -c:v mpeg1video -bf 2 -g 30 -q:v 8 -f mpeg still_352x240.mpg
```
