/* mh2.ini: the settings the launcher edits. Read once at start-up from the
 * current directory (the game folder). Without the file everything keeps the
 * built-in defaults, so development runs from ../disc behave as before. */
#pragma once

enum { CFG_UP, CFG_DOWN, CFG_LEFT, CFG_RIGHT, CFG_START, CFG_SELECT, CFG_CROSS, CFG_CIRCLE,
       CFG_SQUARE, CFG_TRIANGLE, CFG_L1, CFG_R1, CFG_L2, CFG_R2, CFG_L3, CFG_R3,
       CFG_LS_UP, CFG_LS_DOWN, CFG_LS_LEFT, CFG_LS_RIGHT,
       CFG_RS_UP, CFG_RS_DOWN, CFG_RS_LEFT, CFG_RS_RIGHT, CFG_NKEYS };

typedef struct {
    int scale;              /* internal resolution 1..4 (0 = leave the command line's) */
    int window_w, window_h; /* client size; 0 = default */
    int fullscreen;         /* borderless on the chosen monitor */
    int skip_intro;         /* 1 = press Start for the player during the boot movies */
    int widescreen;         /* 1 = 16:9: wider 3D view (camera hook in main.c) and a 16:9 picture */
    int stretch;            /* 1 = fill the window instead of 4:3 */
    int smooth;             /* 1 = linear filter when enlarging, 0 = nearest */
    int frame_limit;        /* 0 = run as fast as possible */
    int vsync;              /* 1 = present in step with a 60/120 Hz display (gs_gl.c choose_vsync) */
    int gamepad;            /* 0 = ignore XInput pads */
    int rumble;             /* 1 = gamepad vibration when the game asks for it */
    int pad2;               /* player 2 (port 2): 0 off, 1 second XInput pad, 2 first XInput pad (keyboard stays player 1) */
    int keys[CFG_NKEYS][2]; /* virtual-key codes, 0 = none */
} bmx2_cfg;

extern bmx2_cfg g_cfg;
void config_load(const char *path);
