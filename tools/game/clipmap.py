#!/usr/bin/env python3
"""The game's sequences placed in the CD-ROM's clips.

DirkSimple's sequences (https://github.com/icculus/DirkSimple, zlib licence,
by Ryan C. Gordon; game.lua dumped by game_to_json.lua) start at times of
the laserdisc. The SNES project's segment table
(https://github.com/astrobleem/SNES-SuperDragonsLairArcade, MIT licence, by
Chad Doebelin; data/segment_timing.json) cuts the laserdisc into segments,
each with its first frame and its length. The CD-ROM's clips are those
segments, re-cut: S01..S18 are the segments dls00..dls17 by name, the later
ones are matched scene by scene by their clips' lengths (a scene's main clip
and its others: deaths, the last part). This script places every sequence
that seeks in a clip, at a frame of the clip file (25 a second).

  python3 tools/game/clipmap.py GAME_JSON SEGMENTS_JSON CLIPS_DIR

CLIPS_DIR holds a set of clip files (*.DLC: their frames come from their
headers). It prints the segments' clips, then every sequence's place, and
what it could not place.
"""

import glob
import json
import os
import re
import struct
import sys

LASERDISC_FPS = 23.976
CONTENT_START_MS = 6297.0  # DirkSimple: the disc's content starts here
CLIP_FPS = 25
START_SLACK = 3     # laserdisc frames: a start this early is the next segment's
OVERRUN_SLACK = 2   # clip frames: the last picture held, not the next clip
MATCH_MS = 350      # a clip and a segment this close in length are the same


def clip_frames(clips_dir):
    frames = {}
    for path in glob.glob(os.path.join(clips_dir, "*.DLC")):
        with open(path, "rb") as f:
            header = f.read(64)
        frames[os.path.basename(path)[:-4]] = struct.unpack_from("<I", header, 8)[0]
    return frames


def groups(names, pattern):
    out = {}
    for name in names:
        m = re.match(pattern, name)
        if m:
            out.setdefault(int(m.group(1)), []).append((m.group(2) or "", name))
    return out


def match_members(seg_members, clip_members, seg_ms, clip_ms):
    """A scene's segments to its clips: the main one and the last part (b)
    by kind; the others (deaths, the scene's second part) in order, the
    segments in the disc's order against the clips in their numbers' order,
    a clip as long as one segment or as two in a row (then it holds both).
    """
    out = {}
    clips = {kind: name for kind, name in clip_members}
    for kind, seg in seg_members:
        if kind in ("", "b") and kind in clips:
            out[seg] = (clips[kind], 0.0)

    def number(name):
        m = re.search(r"(\d+)$", name)
        return int(m.group(1)) if m else 0

    segs = sorted((s for k, s in seg_members if k not in ("", "b")),
                  key=lambda s: seg_ms[s][1])
    cl = sorted((c for k, c in clip_members if k not in ("", "b")), key=number)
    i = j = 0
    while i < len(segs) and j < len(cl):
        one = seg_ms[segs[i]][2]
        two = one + seg_ms[segs[i + 1]][2] if i + 1 < len(segs) else None
        have = clip_ms[cl[j]]
        if abs(have - one) <= MATCH_MS:
            out[segs[i]] = (cl[j], 0.0)
            i += 1
            j += 1
        elif two is not None and abs(have - two) <= MATCH_MS:
            out[segs[i]] = (cl[j], 0.0)
            out[segs[i + 1]] = (cl[j], one)
            i += 2
            j += 1
        else:
            i += 1  # a segment with no clip of its own here
    return out


def scene_cost(seg_members, clip_members, seg_ms, clip_ms):
    main_s = [seg_ms[s][2] for k, s in seg_members if k == ""]
    main_c = [clip_ms[c] for k, c in clip_members if k == ""]
    cost = abs((main_s or [0])[0] - (main_c or [0])[0])
    rest_s = sorted(seg_ms[s][2] for k, s in seg_members if k != "")
    rest_c = sorted(clip_ms[c] for k, c in clip_members if k != "")
    return cost + abs(sum(rest_s) - sum(rest_c))


def main():
    game = json.load(open(sys.argv[1]))
    segments = json.load(open(sys.argv[2]))["segments"]
    frames = clip_frames(sys.argv[3])
    clip_ms = {c: n * 1000.0 / CLIP_FPS for c, n in frames.items()}

    # Every segment (by its place on the disc: one name can be there twice).
    seg_ms = {}
    for s in segments:
        name = s["filename"].split(".")[0]
        seg_ms.setdefault(name, s["frame"])
    seg_ms = {name: (name, frame, next(s["duration_ms"] for s in segments if s["filename"].startswith(name + ".")))
              for name, frame in seg_ms.items()}

    sg = groups(seg_ms, r"dls(\d\d)(.*)")
    cg = groups(frames, r"S(\d\d)(.*)")
    scene_of = {g: g + 1 for g in sg if g + 1 in cg and g <= 17}
    left_s = sorted(g for g in sg if g not in scene_of)
    left_c = sorted(g for g in cg if g not in scene_of.values())
    pairs = sorted((scene_cost(sg[a], cg[b], seg_ms, clip_ms), a, b) for a in left_s for b in left_c)
    for cost, a, b in pairs:
        if a not in scene_of and b not in scene_of.values():
            scene_of[a] = b

    seg_clip = {}
    for a, b in sorted(scene_of.items()):
        members = [(k.lower(), s) for k, s in sg[a]]
        cmembers = [(k.lower(), c) for k, c in cg[b]]
        seg_clip.update(match_members(members, cmembers, seg_ms, clip_ms))

    print("Segments and their clips:")
    for s in segments:
        name = s["filename"].split(".")[0]
        clip, at = seg_clip.get(name, (None, 0))
        if clip:
            print("  %-9s frame %6d %8.1f ms  -> %-7s %8.1f ms%s" % (
                name, s["frame"], s["duration_ms"], clip, clip_ms[clip],
                "  (from %.0f ms)" % at if at else ""))
        else:
            print("  %-9s frame %6d %8.1f ms  -> none" % (name, s["frame"], s["duration_ms"]))

    # Every sequence that seeks: in the segment that starts latest at or just
    # after its start (DirkSimple's starts can be a few frames before a
    # segment, and segments overlap by a few frames); a sequence longer than
    # what is left of its clip plays on into the clip of the next segment on
    # the disc, as the laserdisc does.
    spans = sorted((s["frame"], s["frame"] + s["duration_ms"] * LASERDISC_FPS / 1000.0,
                    s["filename"].split(".")[0]) for s in segments)
    placed, missing = [], []
    for scene, seqs in sorted(game["scenes"].items()):
        for seq, d in sorted(seqs.items()):
            t = d.get("start_time", -1)
            if t is None or t < 0:
                continue
            ld = (t + CONTENT_START_MS) * LASERDISC_FPS / 1000.0
            hit = [sp for sp in spans if sp[0] <= ld + START_SLACK and ld < sp[1]]
            if not hit or hit[-1][2] not in seg_clip:
                missing.append((scene, seq, ld))
                continue
            first, end, seg = hit[-1]
            clip, at = seg_clip[seg]
            ms = max(0.0, at + (ld - first) * 1000.0 / LASERDISC_FPS)
            frame = round(ms * CLIP_FPS / 1000.0)
            when = (d.get("timeout") or {}).get("when", 0) or 0
            need = frame + (1 if d.get("is_single_frame") else round(when * CLIP_FPS / 1000.0))
            then = None
            if need > frames[clip] + OVERRUN_SLACK:
                after = [sp for sp in spans if sp[0] >= end - START_SLACK and sp[2] != seg]
                then = seg_clip.get(after[0][2], (None, 0))[0] if after else None
            placed.append((scene, seq, ld, seg, clip, frame, when, need - frames[clip], then))
    print("\nSequences placed: %d; not placed: %d" % (len(placed), len(missing)))
    for scene, seq, ld, seg, clip, frame, when, over, then in placed:
        note = ""
        if then:
            note = "  then %s (%d frames into it)" % (then, over)
        elif over > OVERRUN_SLACK:
            note = "  RUNS PAST THE CLIP BY %d FRAMES" % over
        print("  %-30s %-26s ld %8.1f  %-9s -> %-7s frame %5d  for %6.0f ms%s" % (
            scene, seq, ld, seg, clip, frame, when, note))
    for scene, seq, ld in missing:
        print("  NOT PLACED %-30s %-26s ld %8.1f" % (scene, seq, ld))

if __name__ == "__main__":
    main()
