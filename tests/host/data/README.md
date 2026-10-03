# Host test data

Synthetic streams for `test_mp2_audio.c`, made with ffmpeg 8.1 (sine tones; nothing from
the game). From this folder:

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
