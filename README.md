<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**SwanStation** is a PlayStation (PS1) emulator focused on accuracy, modern rendering features, and reliable performance across a wide range of titles.

This fork runs SwanStation standalone on the Nintendo Switch for the Tico frontend, driving the emulator directly rather than through libretro, and adds a small set of practical features while preserving the behavior and compatibility of the original emulator.

----------

## Summary

This fork focuses on making SwanStation more usable in practice without changing its core design.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   A choice of renderer in Settings: Vulkan, OpenGL, Zink (OpenGL on Vulkan) or Software
-   Disc changes from the menu for multi-disc games (`.m3u` or the disc's folder)
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters (Vulkan and Software)
-   Built-in save and load state support, with a picture of each slot
-   Cheats in RetroArch's `.cht` format, run by SwanStation's own cheat engine
-   Integrated RetroAchievements with custom alerts
-   A game library when started without a game

----------

## Credits

This fork is built on top of SwanStation, itself derived from DuckStation.
All core emulation work belongs to their authors and contributors.

- **SwanStation repository** — https://github.com/libretro/swanstation
- **DuckStation repository** — https://github.com/stenzek/duckstation

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.