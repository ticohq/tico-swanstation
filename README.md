<picture>  
<source media="(prefers-color-scheme: dark)" srcset="https://i.imgur.com/8qsV6MH.png">  
<source media="(prefers-color-scheme: light)" srcset="https://i.imgur.com/4cpzGnB.png">  
<img src="https://i.imgur.com/8qsV6MH.png" width="200">  
</picture>  

*Part of the Tico ecosystem* — https://www.ticoverse.com

**SwanStation** is a PlayStation (PS1) emulator focused on accuracy, modern rendering features, and reliable performance across a wide range of titles.

This fork adapts SwanStation to work with the Tico frontend and provides a standalone build for the Nintendo Switch, adding a small set of practical features while preserving the behavior and compatibility of the original emulator.

----------

## Summary

This fork focuses on making SwanStation more usable in practice without changing its core design.

It adds:

-   Custom overlay matching Tico design, including time, date, user avatar, and game title
-   Explicit control over display (integer scaling and aspect ratios)
-   Runtime-selectable rendering filters  
-   Built-in save and load state support
-   Integrated RetroAchievements with custom alerts

----------

## Notes

-   Some VIXL classes and enums were renamed with a `swanstation` prefix to avoid symbol conflicts during static linking of multiple cores

----------

## A Note

A lot of work in this scene disappears over time — not because it lacked value, but because it was never shared.

If you are building something, consider releasing it. Even small contributions can help others move forward.