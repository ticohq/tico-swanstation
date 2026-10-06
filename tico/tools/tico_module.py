#!/usr/bin/env python3
"""Settings definition and translations for the tico module of SwanStation.

Run after merging upstream. It builds tools/dump_core_options.c against the
core's own libretro_core_options.h, so tico/module/settings.json lists exactly
the options the core reads (swanstation_* keys, values and defaults), laid out
in tabs, together with the overlay's own display and controls options. Labels
are translation keys; the strings go into tico/lang/*.json. The core ships no
translations, so options tico-nx already labelled for its PlayStation core
borrow tico-nx's translations, and the rest are English (the overlay falls
back to English). Choice labels stay English in settings.json; the overlay
translates them through settings_swanstation_value_* keys.

    python3 tico/tools/tico_module.py
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TICO = ROOT / "tico"
SETTINGS = TICO / "module/settings.json"
LANG_DIR = TICO / "lang"
LANGUAGES = ("en", "de", "es", "fr", "ja", "pt", "ru", "zh")
PREFIX = "settings_swanstation_"
# tico-nx's own strings label the options it already knew; optional
TICO_NX = Path(os.environ.get("TICO_NX_DIR", Path.home() / "tico-nx"))

# Core option -> tico-nx label whose translations it takes.
BORROWED = {
    "swanstation_Console_Region": "console_region",
    "swanstation_BIOS_PatchFastBoot": "fast_boot",
    "swanstation_CDROM_RegionCheck": "region_check",
    "swanstation_CDROM_ReadThread": "async_read_thread",
    "swanstation_CDROM_LoadImagePatches": "apply_image_patches",
    "swanstation_CDROM_LoadImageToRAM": "load_image_to_ram",
    "swanstation_CDROM_MuteCDAudio": "mute_cd_audio",
    "swanstation_CDROM_ReadSpeedup": "read_speedup",
    "swanstation_CDROM_SeekSpeedup": "seek_speedup",
    "swanstation_CDROM_ReadaheadSectors": "readahead_sectors",
    "swanstation_CPU_ExecutionMode": "cpu_execution_mode",
    "swanstation_CPU_Overclock": "cpu_overclock_percent",
    "swanstation_CPU_RecompilerICache": "recompiler_icache",
    "swanstation_CPU_RecompilerBlockLinking": "recompiler_block_linking",
    "swanstation_CPU_FastmemMode": "cpu_fastmem_mode",
    "swanstation_Console_Enable8MBRAM": "8mb_ram",
    "swanstation_Main_ApplyGameSettings": "apply_compatibility_settings",
    "swanstation_GPU_Renderer": "renderer",
    "swanstation_GPU_ResolutionScale": "internal_resolution",
    "swanstation_GPU_MSAA": "msaa",
    "swanstation_GPU_TrueColor": "true_color",
    "swanstation_GPU_ScaledDithering": "scaled_dithering",
    "swanstation_GPU_DisableInterlacing": "disable_interlacing",
    "swanstation_GPU_ForceNTSCTimings": "force_ntsc_timings",
    "swanstation_Display_Force4_3For24Bit": "force_4_3_24bit",
    "swanstation_GPU_ChromaSmoothing24Bit": "chroma_smoothing_24bit",
    "swanstation_GPU_TextureFilter": "texture_filter",
    "swanstation_GPU_WidescreenHack": "widescreen_hack",
    "swanstation_GPU_DownsampleMode": "downsample_mode",
    "swanstation_GPU_UseSoftwareRendererForReadbacks": "software_readbacks",
    "swanstation_GPU_PGXPEnable": "geometry_correction",
    "swanstation_GPU_PGXPCulling": "pgxp_culling",
    "swanstation_GPU_PGXPTextureCorrection": "perspective_textures",
    "swanstation_GPU_PGXPColorCorrection": "perspective_colors",
    "swanstation_GPU_PGXPDepthBuffer": "pgxp_depth_buffer",
    "swanstation_GPU_PGXPVertexCache": "pgxp_vertex_cache",
    "swanstation_GPU_PGXPCPU": "pgxp_cpu_mode",
    "swanstation_GPU_PGXPPreserveProjFP": "pgxp_preserve_projection",
    "swanstation_GPU_PGXPTolerance": "pgxp_tolerance",
    "swanstation_GPU_PGXPDepthClearThreshold": "pgxp_depth_clear_threshold",
    "swanstation_Display_AspectRatio": "aspect_ratio",
    "swanstation_Display_CropMode": "crop_mode",
    "swanstation_Display_ShowOSDMessages": "show_osd_messages",
    "swanstation_Display_ActiveStartOffset": "active_start_offset",
    "swanstation_Display_ActiveEndOffset": "active_end_offset",
    "swanstation_Display_LineStartOffset": "line_start_offset",
    "swanstation_Display_LineEndOffset": "line_end_offset",
    "swanstation_MemoryCards_Card1Type": "memory_card_1_type",
    "swanstation_MemoryCards_Card2Type": "memory_card_2_type",
    "swanstation_MemoryCards_UsePlaylistTitle": "memory_cards_use_playlist_title",
    "swanstation_Controller_EnableRumble": "enable_rumble",
    "swanstation_Controller_AnalogCombo": "analog_toggle_combo",
    "swanstation_Controller1_ForceAnalog": "force_analog_mode",
    "swanstation_Controller1_AnalogDPadInDigitalMode": "analog_d_pad_digital",
    "swanstation_Controller1_VibrationBias": "vibration_bias",
    "swanstation_Controller2_ForceAnalog": "force_analog_mode",
    "swanstation_Controller2_AnalogDPadInDigitalMode": "analog_d_pad_digital",
    "swanstation_Controller2_VibrationBias": "vibration_bias",
    # tabs and sections
    "tab_system": "tab_system",
    "section_system": "section_system",
    "section_cd_rom": "section_cd_rom",
    "tab_graphics": "tab_graphics",
    "section_pgxp": "section_pgxp_enhancements",
    "tab_input": "tab_input",
    "section_controller_1": "section_controller_1",
    "section_controller_2": "section_controller_2",
    "tab_memory_cards": "tab_memory_cards",
    "section_memory_cards": "section_memory_cards",
    "section_cpu": "section_cpu",
    "section_core_display": "section_display",
}

# Core options the frontend has no use for: the BIOS is found by its contents
# in the system folder, only two pads are plugged in (no multitap), lightguns
# and the NeGcon need input the Switch does not have, and the log goes nowhere.
EXCLUDED = {
    "swanstation_BIOS_PathNTSCJ", "swanstation_BIOS_PathNTSCU", "swanstation_BIOS_PathPAL",
    "swanstation_ControllerPorts_MultitapMode", "swanstation_Controller_ShowCrosshair",
    "swanstation_Logging_LogLevel",
} | {f"swanstation_Controller{n}_{name}" for n in range(1, 9)
     for name in ("XScale", "YScale", "SteeringDeadzone", "TwistResponse")} \
  | {f"swanstation_Controller{n}_{name}" for n in range(3, 9)
     for name in ("ForceAnalog", "AnalogDPadInDigitalMode", "AxisScale", "VibrationBias")}

# Choices the Switch cannot keep up with, or that are not here.
TRIMMED = {
    "swanstation_GPU_ResolutionScale": {str(n) for n in range(7, 17)},
    "swanstation_GPU_MSAA": {"16", "32", "8-ssaa", "16-ssaa", "32-ssaa"},
}

# Choice labels that say what the choice is here. (option, value) -> label
RELABELLED = {
    ("swanstation_MemoryCards_Card1Type", "Libretro"): "Game's own card (Default)",
}

# The renderers, picked before the core starts (TicoCore/SwanRuntime): the
# core's Vulkan device shared with the frontend, OpenGL on nvc0 or on Zink
# (OpenGL over Vulkan), or software rendering.
RENDERER = {"key": "swanstation_GPU_Renderer", "type": "enum", "default": "Vulkan", "restart": True,
            "choices": [("Vulkan", "Vulkan"), ("OpenGL", "OpenGL"), ("Zink", "Zink (OpenGL on Vulkan)"),
                        ("Software", "Software")]}

# The pad each port has; player 2's is plugged in while a second controller is.
PAD_TYPES = [("DualShock", "DualShock"), ("Digital", "Digital Controller"), ("None", "None")]

# Listed only while another option has a value, like the core's own menus.
PGXP = "swanstation_GPU_PGXPEnable"
DEPENDS_ON: dict[str, tuple[str, str]] = {
    **{key: (PGXP, "true") for key in (
        "swanstation_GPU_PGXPCulling", "swanstation_GPU_PGXPTextureCorrection",
        "swanstation_GPU_PGXPColorCorrection", "swanstation_GPU_PGXPDepthBuffer",
        "swanstation_GPU_PGXPVertexCache", "swanstation_GPU_PGXPCPU",
        "swanstation_GPU_PGXPPreserveProjFP", "swanstation_GPU_PGXPTolerance",
        "swanstation_GPU_PGXPDepthClearThreshold")},
    "swanstation_Display_CustomAspectRatioNumerator": ("swanstation_Display_AspectRatio", "Custom"),
    "swanstation_Display_CustomAspectRatioDenominator": ("swanstation_Display_AspectRatio", "Custom"),
}

# Switch buttons a PlayStation button (or the fast-forward hotkey) can sit on.
SWITCH_BUTTONS = [("A", "A"), ("B", "B"), ("X", "X"), ("Y", "Y"), ("L", "L"), ("R", "R"),
                  ("ZL", "ZL"), ("ZR", "ZR"), ("Plus", "Plus"), ("Minus", "Minus"),
                  ("StickL", "Left stick"), ("StickR", "Right stick"), ("Up", "Up"),
                  ("Down", "Down"), ("Left", "Left"), ("Right", "Right"), ("None", "Disabled")]

POSITIONS = [("hidden", "Hidden"), ("top_left", "Top left"), ("top_right", "Top right"),
             ("bottom_left", "Bottom left"), ("bottom_right", "Bottom right")]


def frontend(key: str, default: str, choices, label: str | None = None) -> dict:
    option = {"key": key, "label": PREFIX + (label or key), "type": "enum", "default": default,
              "choices": choices}
    return option


# A layout entry is a core option key or a frontend option (a dict).
LAYOUT = [
    ("tab_system", [
        ("section_system", [
            # tico's check before launching waives the BIOS while this is on
            # (module.json's unless_option), and the core boots OpenBIOS
            {"key": "bios_openbios", "label": PREFIX + "bios_openbios", "type": "bool",
             "default": "false"},
            "swanstation_Console_Region", "swanstation_BIOS_PatchFastBoot",
                            "swanstation_Main_ApplyGameSettings", "swanstation_Console_Enable8MBRAM"]),
        ("section_cd_rom", ["swanstation_CDROM_ReadSpeedup", "swanstation_CDROM_SeekSpeedup",
                            "swanstation_CDROM_ReadThread", "swanstation_CDROM_ReadaheadSectors",
                            "swanstation_CDROM_LoadImageToRAM", "swanstation_CDROM_PreCacheCHD",
                            "swanstation_CDROM_RegionCheck", "swanstation_CDROM_LoadImagePatches",
                            "swanstation_CDROM_MuteCDAudio"]),
    ]),
    ("tab_graphics", [
        ("section_rendering", [RENDERER, "swanstation_GPU_ResolutionScale", "swanstation_GPU_MSAA",
                               "swanstation_GPU_TextureFilter", "swanstation_GPU_TrueColor",
                               "swanstation_GPU_ScaledDithering", "swanstation_GPU_DisableInterlacing",
                               "swanstation_GPU_WidescreenHack", "swanstation_GPU_ForceNTSCTimings",
                               "swanstation_Display_Force4_3For24Bit",
                               "swanstation_GPU_ChromaSmoothing24Bit", "swanstation_GPU_DownsampleMode",
                               "swanstation_GPU_UseThread",
                               "swanstation_GPU_UseSoftwareRendererForReadbacks",
                               "swanstation_GPU_ShaderPrecompile"]),
        ("section_pgxp", [PGXP, "swanstation_GPU_PGXPCulling", "swanstation_GPU_PGXPTextureCorrection",
                          "swanstation_GPU_PGXPColorCorrection", "swanstation_GPU_PGXPDepthBuffer",
                          "swanstation_GPU_PGXPVertexCache", "swanstation_GPU_PGXPCPU",
                          "swanstation_GPU_PGXPPreserveProjFP", "swanstation_GPU_PGXPTolerance",
                          "swanstation_GPU_PGXPDepthClearThreshold"]),
    ]),
    # the overlay's own: how the game is scaled, fast forward and the HUD.
    # Shaders are picked in game (Settings > Shaders).
    ("tab_display", [
        ("section_screen", [
            frontend("display_mode", "Display", [("Integer", "Integer"), ("Display", "Display")]),
            frontend("display_size", "4:3", [("Stretch", "Stretch"), ("4:3", "4:3"), ("16:9", "16:9"),
                                              ("Original", "Original"), ("1x", "1x"), ("2x", "2x"),
                                              ("Auto", "Auto")]),
        ]),
        ("section_core_display", ["swanstation_Display_AspectRatio",
                                  "swanstation_Display_CustomAspectRatioNumerator",
                                  "swanstation_Display_CustomAspectRatioDenominator",
                                  "swanstation_Display_CropMode", "swanstation_Display_ShowOSDMessages",
                                  "swanstation_Display_ActiveStartOffset",
                                  "swanstation_Display_ActiveEndOffset",
                                  "swanstation_Display_LineStartOffset",
                                  "swanstation_Display_LineEndOffset"]),
        ("section_fast_forward", [
            frontend("fast_forward_speed", "200", [("150", "150%"), ("200", "200%"), ("300", "300%"),
                                                   ("400", "400%"), ("unlimited", "Unlimited")]),
            frontend("fast_forward_mode", "hold", [("hold", "Hold"), ("toggle", "Toggle")]),
            # every Switch button is a PlayStation one by default
            frontend("fast_forward_hotkey", "None", SWITCH_BUTTONS),
        ]),
        ("section_hud", [
            frontend("fps_counter_position", "hidden", POSITIONS, "fps_counter"),
            frontend("rendered_ir_position", "hidden", POSITIONS, "rendered_resolution"),
        ]),
    ]),
    ("tab_performance", [
        ("section_cpu", ["swanstation_CPU_ExecutionMode", "swanstation_CPU_Overclock",
                         "swanstation_CPU_RecompilerICache", "swanstation_CPU_RecompilerBlockLinking",
                         "swanstation_CPU_FastmemMode", "swanstation_CPU_FastmemRewrite",
                         "swanstation_Main_RunaheadFrameCount", "swanstation_Hacks_OldMDECRoutines",
                         "swanstation_Audio_FastHook"]),
        ("section_texture_replacements", [
            "swanstation_TextureReplacements_EnableVRAMWriteReplacements",
            "swanstation_TextureReplacements_PreloadTextures"]),
    ]),
    ("tab_memory_cards", [
        ("section_memory_cards", ["swanstation_MemoryCards_Card1Type",
                                  "swanstation_MemoryCards_Card2Type",
                                  "swanstation_MemoryCards_UsePlaylistTitle"]),
    ]),
    ("tab_input", [
        ("section_input", [
            {**frontend("pad1_type", "DualShock", PAD_TYPES), "restart": True},
            {**frontend("pad2_type", "DualShock", PAD_TYPES), "restart": True},
            "swanstation_Controller_EnableRumble", "swanstation_Controller_AnalogCombo",
        ]),
        ("section_controller_1", [f"swanstation_Controller1_{name}" for name in (
            "ForceAnalog", "AnalogDPadInDigitalMode", "AxisScale", "VibrationBias")]),
        ("section_controller_2", [f"swanstation_Controller2_{name}" for name in (
            "ForceAnalog", "AnalogDPadInDigitalMode", "AxisScale", "VibrationBias")]),
    ]),
    # the overlay's button mapping: PlayStation button -> Switch button, the
    # face buttons by position (SwanRuntime.cpp's kButtonMappings)
    ("tab_controls", [
        ("section_button_mapping", [
            frontend(key, default, SWITCH_BUTTONS) for key, default in [
                ("map_cross", "B"), ("map_circle", "A"), ("map_square", "Y"), ("map_triangle", "X"),
                ("map_l1", "L"), ("map_r1", "R"), ("map_l2", "ZL"), ("map_r2", "ZR"),
                ("map_l3", "StickL"), ("map_r3", "StickR"),
                ("map_start", "Plus"), ("map_select", "Minus"),
                ("map_up", "Up"), ("map_down", "Down"), ("map_left", "Left"), ("map_right", "Right"),
            ]
        ] + [
            {"key": "analog_dpad", "label": PREFIX + "analog_dpad", "type": "bool", "default": "false"},
        ]),
    ]),
]

# Labels neither the core nor tico-nx has. suffix -> (en, de, es, fr, ja, pt, ru, zh)
LABELS = {
    "bios_openbios": ("Use OpenBIOS when no BIOS is found", "OpenBIOS verwenden, wenn kein BIOS gefunden wird",
                      "Usar OpenBIOS si no se encuentra una BIOS", "Utiliser OpenBIOS si aucun BIOS n'est trouvé",
                      "BIOSがない場合はOpenBIOSを使う", "Usar OpenBIOS quando nenhuma BIOS for encontrada",
                      "Использовать OpenBIOS, если BIOS не найден", "未找到 BIOS 时使用 OpenBIOS"),
    "section_rendering": ("Rendering", "Rendering", "Renderizado", "Rendu", "レンダリング", "Renderização",
                          "Рендеринг", "渲染"),
    "tab_display": ("Display", "Anzeige", "Pantalla", "Affichage", "表示", "Tela", "Экран", "显示"),
    "section_screen": ("Screen", "Bild", "Imagen", "Image", "画面", "Imagem", "Изображение", "画面"),
    "section_hud": ("On-screen info", "Bildschirmanzeige", "Información en pantalla",
                    "Affichage à l'écran", "画面表示", "Informações na tela", "Экранная информация",
                    "屏幕信息"),
    "display_mode": ("Display Mode", "Anzeigemodus", "Modo de pantalla", "Mode d'affichage", "表示モード",
                     "Modo de exibição", "Режим отображения", "显示模式"),
    "display_size": ("Size", "Größe", "Tamaño", "Taille", "サイズ", "Tamanho", "Размер", "尺寸"),
    "fps_counter": ("FPS counter", "FPS-Zähler", "Contador de FPS", "Compteur de FPS", "FPSカウンター",
                    "Contador de FPS", "Счётчик FPS", "帧率计数器"),
    "rendered_resolution": ("Rendered resolution", "Gerenderte Auflösung", "Resolución renderizada",
                            "Résolution de rendu", "描画解像度", "Resolução renderizada",
                            "Разрешение рендеринга", "渲染分辨率"),
    "section_fast_forward": ("Fast Forward", "Vorspulen", "Avance rápido", "Avance rapide", "早送り",
                             "Avanço rápido", "Перемотка", "快进"),
    "fast_forward_speed": ("Fast forward speed", "Vorspul-Geschwindigkeit", "Velocidad de avance rápido",
                           "Vitesse d'avance rapide", "早送りの速度", "Velocidade do avanço rápido",
                           "Скорость перемотки", "快进速度"),
    "fast_forward_mode": ("Fast forward mode", "Vorspul-Modus", "Modo de avance rápido",
                          "Mode d'avance rapide", "早送りモード", "Modo do avanço rápido",
                          "Режим перемотки", "快进模式"),
    "fast_forward_hotkey": ("Fast forward button", "Vorspul-Taste", "Botón de avance rápido",
                            "Bouton d'avance rapide", "早送りボタン", "Botão do avanço rápido",
                            "Кнопка перемотки", "快进按键"),
    "tab_performance": ("Performance", "Leistung", "Rendimiento", "Performances", "パフォーマンス",
                        "Desempenho", "Производительность", "性能"),
    "section_texture_replacements": ("Texture replacements", "Texturersetzung", "Reemplazo de texturas",
                                     "Remplacement de textures", "テクスチャ置き換え",
                                     "Substituição de texturas", "Замена текстур", "纹理替换"),
    "section_input": ("Controllers", "Controller", "Mandos", "Manettes", "コントローラー", "Controles",
                      "Контроллеры", "控制器"),
    "pad1_type": ("Player 1 controller", "Controller Spieler 1", "Mando del jugador 1",
                  "Manette du joueur 1", "プレイヤー1のコントローラー", "Controle do jogador 1",
                  "Контроллер игрока 1", "玩家1控制器"),
    "pad2_type": ("Player 2 controller", "Controller Spieler 2", "Mando del jugador 2",
                  "Manette du joueur 2", "プレイヤー2のコントローラー", "Controle do jogador 2",
                  "Контроллер игрока 2", "玩家2控制器"),
    "tab_controls": ("Controls", "Steuerung", "Controles", "Commandes", "操作", "Controles", "Управление",
                     "控制"),
    "section_button_mapping": ("Button mapping", "Tastenbelegung", "Asignación de botones",
                               "Attribution des boutons", "ボタン割り当て", "Mapeamento de botões",
                               "Назначение кнопок", "按键映射"),
    "map_cross": ("Cross", "Kreuz", "Equis", "Croix", "バツ", "Xis", "Крест", "叉"),
    "map_circle": ("Circle", "Kreis", "Círculo", "Rond", "マル", "Círculo", "Круг", "圆圈"),
    "map_square": ("Square", "Quadrat", "Cuadrado", "Carré", "シカク", "Quadrado", "Квадрат", "方块"),
    "map_triangle": ("Triangle", "Dreieck", "Triángulo", "Triangle", "サンカク", "Triângulo", "Треугольник",
                     "三角"),
    "map_l1": ("L1", "L1", "L1", "L1", "L1", "L1", "L1", "L1"),
    "map_r1": ("R1", "R1", "R1", "R1", "R1", "R1", "R1", "R1"),
    "map_l2": ("L2", "L2", "L2", "L2", "L2", "L2", "L2", "L2"),
    "map_r2": ("R2", "R2", "R2", "R2", "R2", "R2", "R2", "R2"),
    "map_l3": ("L3", "L3", "L3", "L3", "L3", "L3", "L3", "L3"),
    "map_r3": ("R3", "R3", "R3", "R3", "R3", "R3", "R3", "R3"),
    "map_start": ("Start", "Start", "Start", "Start", "スタート", "Start", "Start", "开始"),
    "map_select": ("Select", "Select", "Select", "Select", "セレクト", "Select", "Select", "选择"),
    "map_up": ("D-Pad Up", "Steuerkreuz oben", "Cruceta arriba", "Croix haut", "十字キー上",
               "Direcional para cima", "Крестовина вверх", "方向键上"),
    "map_down": ("D-Pad Down", "Steuerkreuz unten", "Cruceta abajo", "Croix bas", "十字キー下",
                 "Direcional para baixo", "Крестовина вниз", "方向键下"),
    "map_left": ("D-Pad Left", "Steuerkreuz links", "Cruceta izquierda", "Croix gauche", "十字キー左",
                 "Direcional para a esquerda", "Крестовина влево", "方向键左"),
    "map_right": ("D-Pad Right", "Steuerkreuz rechts", "Cruceta derecha", "Croix droite", "十字キー右",
                  "Direcional para a direita", "Крестовина вправо", "方向键右"),
    "analog_dpad": ("Left stick as D-Pad", "Linker Stick als Steuerkreuz", "Stick izquierdo como cruceta",
                    "Stick gauche comme croix directionnelle", "左スティックを十字キーとして使う",
                    "Analógico esquerdo como direcional", "Левый стик как крестовина", "左摇杆作为方向键"),
}

# Choice labels translated here. English -> (de, es, fr, ja, pt, ru, zh)
CHOICES = {
    "Disabled": ("Deaktiviert", "Desactivado", "Désactivé", "無効", "Desativado", "Выключено", "禁用"),
    "Enabled": ("Aktiviert", "Activado", "Activé", "有効", "Ativado", "Включено", "启用"),
    "None": ("Keine", "Ninguno", "Aucun", "なし", "Nenhum", "Нет", "无"),
    "Integer": ("Ganzzahlig", "Entero", "Entier", "整数倍", "Inteiro", "Целочисленный", "整数"),
    "Display": ("Anzeige", "Pantalla", "Écran", "画面", "Tela", "Экран", "屏幕"),
    "Stretch": ("Strecken", "Estirar", "Étirer", "引き伸ばし", "Esticar", "Растянуть", "拉伸"),
    "Original": ("Original", "Original", "Original", "オリジナル", "Original", "Оригинал", "原始"),
    "Auto": ("Auto", "Auto", "Auto", "自動", "Auto", "Авто", "自动"),
    "Unlimited": ("Unbegrenzt", "Ilimitado", "Illimité", "無制限", "Ilimitado", "Без ограничений", "无限制"),
    "Hold": ("Halten", "Mantener", "Maintenir", "長押し", "Segurar", "Удерживать", "按住"),
    "Toggle": ("Umschalten", "Alternar", "Basculer", "切り替え", "Alternar", "Переключать", "切换"),
    "Right stick": ("Rechter Stick", "Stick derecho", "Stick droit", "右スティック", "Analógico direito",
                    "Правый стик", "右摇杆"),
    "Left stick": ("Linker Stick", "Stick izquierdo", "Stick gauche", "左スティック", "Analógico esquerdo",
                   "Левый стик", "左摇杆"),
    "Plus": ("Plus", "Más", "Plus", "プラス", "Mais", "Плюс", "加号"),
    "Minus": ("Minus", "Menos", "Moins", "マイナス", "Menos", "Минус", "减号"),
    "Up": ("Oben", "Arriba", "Haut", "上", "Cima", "Вверх", "上"),
    "Down": ("Unten", "Abajo", "Bas", "下", "Baixo", "Вниз", "下"),
    "Left": ("Links", "Izquierda", "Gauche", "左", "Esquerda", "Влево", "左"),
    "Right": ("Rechts", "Derecha", "Droite", "右", "Direita", "Вправо", "右"),
    "Hidden": ("Ausgeblendet", "Oculto", "Masqué", "非表示", "Oculto", "Скрыто", "隐藏"),
    "Top left": ("Oben links", "Arriba a la izquierda", "En haut à gauche", "左上", "Superior esquerdo",
                 "Сверху слева", "左上"),
    "Top right": ("Oben rechts", "Arriba a la derecha", "En haut à droite", "右上", "Superior direito",
                  "Сверху справа", "右上"),
    "Bottom left": ("Unten links", "Abajo a la izquierda", "En bas à gauche", "左下", "Inferior esquerdo",
                    "Снизу слева", "左下"),
    "Bottom right": ("Unten rechts", "Abajo a la derecha", "En bas à droite", "右下", "Inferior direito",
                     "Снизу справа", "右下"),
    "Digital Controller": ("Digital-Controller", "Mando digital", "Manette numérique", "デジタルコントローラー",
                           "Controle digital", "Цифровой контроллер", "数字控制器"),
    "Zink (OpenGL on Vulkan)": ("Zink (OpenGL über Vulkan)", "Zink (OpenGL sobre Vulkan)",
                                "Zink (OpenGL sur Vulkan)", "Zink（Vulkan上のOpenGL）",
                                "Zink (OpenGL sobre Vulkan)", "Zink (OpenGL поверх Vulkan)",
                                "Zink（基于 Vulkan 的 OpenGL）"),
    "Software": ("Software", "Software", "Logiciel", "ソフトウェア", "Software", "Программный", "软件"),
}

RESTART = re.compile(r"\s*\((Restart|Restart Required|Reload Core)\)")
CONTROLLER = re.compile(r"^Controller [12] ")


def value_key(label: str) -> str:
    """tico_config.cpp's ValueKey: settings_swanstation_value_ + label as a slug."""
    return PREFIX + "value_" + "_".join(re.findall(r"[a-z0-9]+", label.lower()))


def clean_label(text: str) -> str:
    # a controller's options sit in that controller's section
    return CONTROLLER.sub("", RESTART.sub("", text)).strip()


def label_key(key: str) -> str:
    """The label of a core option, or of a tab or section by its suffix."""
    borrowed = BORROWED.get(key)  # both controllers' copies share one
    if borrowed:
        return PREFIX + borrowed
    return PREFIX + re.sub(r"(?<=[a-z0-9])(?=[A-Z])", "_", key.removeprefix("swanstation_")).lower()


def dump_options() -> dict:
    with tempfile.TemporaryDirectory() as tmp:
        exe = Path(tmp) / "dump_core_options"
        subprocess.run(["cc", "-std=gnu11", "-w", "-DCPU_AARCH64",
                        "-I", str(ROOT / "src/libretro"),
                        "-I", str(ROOT / "dep/libretro-common/include"),
                        "-o", str(exe), str(Path(__file__).with_name("dump_core_options.c"))],
                       check=True)
        return json.loads(subprocess.run([str(exe)], check=True, capture_output=True,
                                         text=True).stdout)


def core_option(source: dict) -> dict:
    key = source["key"]
    values = [(v, RELABELLED.get((key, v), l)) for v, l in source["values"]
              if v not in TRIMMED.get(key, ())]
    option = {"key": key, "label": label_key(key)}
    if sorted(v for v, _ in values) == ["false", "true"]:
        option.update(type="bool", default=source["default"])
    else:
        option.update(type="enum", default=source["default"],
                      choices=[{"label": l, "value": v} for v, l in values])
    if RESTART.search(source["desc"]):
        option["restart"] = True
    if key in DEPENDS_ON:
        on, value = DEPENDS_ON[key]
        option["depends_on"] = {"key": on, "value": value}
    return option


def frontend_option(definition: dict) -> dict:
    option = dict(definition)
    if "label" not in option:
        option["label"] = label_key(option["key"])
    if "choices" in option:
        option["choices"] = [{"label": l, "value": v} for v, l in option["choices"]]
    return option


def build_settings(dump: dict) -> dict:
    core = {o["key"]: o for o in dump["options"]}
    placed = {entry if isinstance(entry, str) else entry["key"]
              for _, sections in LAYOUT for _, entries in sections for entry in entries}
    missing = sorted(set(core) - placed - EXCLUDED)
    if missing:
        raise SystemExit(f"core options not placed in LAYOUT: {missing}")
    tabs = []
    for tab, sections in LAYOUT:
        tabs.append({"name": label_key(tab), "sections": [
            {"title": label_key(title), "options": [
                core_option(core[entry]) if isinstance(entry, str) else frontend_option(entry)
                for entry in entries]}
            for title, entries in sections]})
    return {
        "core_id": "swanstation",
        "display_name": "SwanStation",
        "config_file": "swanstation.jsonc",
        "slugs": ["psx"],
        "bool_true_value": "true",
        "bool_false_value": "false",
        "tabs": tabs,
    }


def tico_nx_strings() -> dict[str, dict[str, str]]:
    out = {}
    for lang in LANGUAGES:
        path = TICO_NX / "assets/lang" / f"{lang}.json"
        out[lang] = json.loads(path.read_text()) if path.exists() else {}
    return out


def build_strings(dump: dict, settings: dict, existing: dict[str, dict[str, str]]):
    strings: dict[str, dict[str, str]] = {lang: {} for lang in LANGUAGES}
    nx = tico_nx_strings()
    have_nx = any(nx.values())
    english = strings["en"]
    for option in dump["options"]:
        english.setdefault(label_key(option["key"]), clean_label(option["desc"]))
    for index, lang in enumerate(LANGUAGES):
        out = strings[lang]
        for suffix, texts in LABELS.items():
            out[PREFIX + suffix] = texts[index]
        # tico-nx's translations of the options it knew (or, without a
        # tico-nx checkout, as the language files already have them)
        for borrowed in set(BORROWED.values()):
            source = nx[lang].get("settings_duckstation_" + borrowed) if have_nx \
                else existing[lang].get(PREFIX + borrowed)
            if source:
                out[PREFIX + borrowed] = source
        if lang != "en":
            for choice, translations in CHOICES.items():
                out[value_key(choice)] = translations[index - 1]
    used = set()

    def walk(node):
        if isinstance(node, dict):
            for field in ("label", "name", "title"):
                if isinstance(node.get(field), str) and node[field].startswith("settings_"):
                    used.add(node[field])
            for child in node.values():
                walk(child)
        elif isinstance(node, list):
            for child in node:
                walk(child)

    walk(settings)
    unlabelled = sorted(used - set(english))
    if unlabelled:
        raise SystemExit(f"labels without English text: {unlabelled}")
    # only what settings.json uses, and the choice translations
    for lang in LANGUAGES:
        strings[lang] = {k: v for k, v in strings[lang].items()
                         if k in used or k.startswith(PREFIX + "value_")}
    return strings


def main() -> None:
    dump = dump_options()
    settings = build_settings(dump)
    SETTINGS.write_text(json.dumps(settings, indent=2, ensure_ascii=False) + "\n")
    current_files = {lang: json.loads((LANG_DIR / f"{lang}.json").read_text())
                     if (LANG_DIR / f"{lang}.json").exists() else {} for lang in LANGUAGES}
    for lang, strings in build_strings(dump, settings, current_files).items():
        path = LANG_DIR / f"{lang}.json"
        current = {k: v for k, v in current_files[lang].items() if not k.startswith(PREFIX)}
        current.update(dict(sorted(strings.items())))
        path.write_text(json.dumps(current, indent=4, ensure_ascii=False) + "\n")
    print(f"wrote {SETTINGS.relative_to(ROOT)} and {len(LANGUAGES)} language files")


if __name__ == "__main__":
    main()
