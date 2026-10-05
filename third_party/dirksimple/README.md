# DirkSimple's Dragon's Lair data

`game.lua` is DirkSimple's description of Dragon's Lair: its scenes, their sequences (each with
its laserdisc start frame), the move windows, deaths, points and the order of the scenes. It is
DirkSimple's re-creation of the arcade game from the original ROM's data.

- Source: https://github.com/icculus/DirkSimple, `data/games/lair/game.lua`
- Commit: d5d75f97af34690010166a786ee41e15dd142c15 (2026-07-26)
- Author: Ryan C. Gordon
- Licence: zlib, in `LICENSE.txt`

Unchanged. `tools/game/game_to_json.lua` reads it as DirkSimple does; the game's table the
firmware embeds is made from it.
