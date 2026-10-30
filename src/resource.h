// Resource IDs shared between .rc files and code.
#pragma once

#define IDD_SETTINGS        101
#define IDD_ADVANCED        102
#define IDD_ABOUT           104
#define IDD_SPLASH          105
#define IDD_HELP            106
#define IDI_APPICON         201

// Displayed version, single source for version.rc and the About box.
#define EDGEPOP_VERSION_TXT "1.2.2"

// The two outward-facing URLs: the site and the donate page.
#define EDGEPOP_SITE_URL    "https://kitkarl.com"
#define EDGEPOP_DONATE_URL  "https://kitkarl.com/donate"

#define IDC_SL_DWELL       1001
#define IDC_ED_DWELL       1002
#define IDC_SL_ZONE        1003
#define IDC_ED_ZONE        1004
#define IDC_SL_STOP        1005
#define IDC_ED_STOP        1006
#define IDC_CK_FSGUARD     1007
#define IDC_CK_EDGE_L      1009
#define IDC_CK_EDGE_R      1010
#define IDC_CK_EDGE_T      1011
#define IDC_CK_EDGE_B      1012
#define IDC_RD_SIDE_MAIN   1013
#define IDC_RD_SIDE_NB     1014
#define IDC_RD_CHAIN_NB    1015
#define IDC_RD_CHAIN_DIR   1016
#define IDC_SL_GRACE       1017
#define IDC_ED_GRACE       1018
#define IDC_CK_ICONS       1019
#define IDC_CK_TITLES      1020
#define IDC_CK_THUMBS      1021
#define IDC_RD_VD_CUR      1022
#define IDC_RD_VD_ALL      1023
#define IDC_CK_STARTWIN    1024
#define IDC_CK_DEBUG       1025
#define IDC_SL_CROSS       1028
#define IDC_ED_CROSS       1029
#define IDC_SL_TITLE       1030
#define IDC_ED_TITLE       1031
#define IDC_SL_REDELAY     1034
#define IDC_ED_REDELAY     1035
#define IDC_SL_STAY       1036
#define IDC_ED_STAY       1037
#define IDC_CK_THUMBOVER   1038   // removed; number not reused
#define IDC_ED_EXCLUDE     1039
#define IDC_CK_PEEK        1040
#define IDC_BTN_RESET      1032
#define IDC_ST_APPLYNOTE   1033

// About dialog + the kitkarl.com links (settings footer, About,
// splash). The splash reuses the About title/version/copyright ids.
#define IDC_LINK_SITE      1041
#define IDC_IC_APP         1042
#define IDC_ST_ABOUT_TITLE 1043
#define IDC_ST_ABOUT_VER   1044
#define IDC_ST_ABOUT_DESC  1045
#define IDC_BTN_DONATE     1046
#define IDC_ST_ABOUT_FREE  1047
#define IDC_ST_ABOUT_COPY  1048

// The (?) help card: a small borderless popup next to the (?),
// sized and positioned at runtime.
#define IDC_ST_HELP_TITLE  1051
#define IDC_ST_HELP_BODY   1052

// The settings donate band.
#define IDC_ST_DONATE_PITCH 1053

// Taskbar indicators + the activation master switch.
// The three masters are on/off SWITCH controls ("EdgePopSwitch").
#define IDC_SW_ENABLE      1054
#define IDC_SW_ARROWS      1055
#define IDC_LB_TB_HOVER    1056
#define IDC_RD_TB_TRI      1062
#define IDC_RD_TB_CHEV     1063
#define IDC_RD_TB_ARR      1064   // shape "Arrow" radio
#define IDC_RD_TB_POS_L    1065   // position: item's left edge
#define IDC_RD_TB_POS_C    1066   // position: item's center
#define IDC_RD_TB_POS_R    1067   // position: item's right edge
#define IDC_CK_TB_SHADOW   1079
#define IDC_BTN_TB_COLOR   1068
#define IDC_ST_TBSWATCH    1069
#define IDC_CK_TB_NUM      1070
#define IDC_CK_TB_ARR      1071
#define IDC_LB_TB_SHAPE    1073
#define IDC_LB_TB_SPOT     1074
#define IDC_SW_STARTWIN    1075
#define IDC_CK_TB_PNUM     1076   // persistent: Numbers
#define IDC_CK_TB_PARR     1077   // persistent: Arrows
#define IDC_LB_TB_PERSIST  1078
// Per-section controls: each section owns its size + background.
#define IDC_SL_TB_PSIZE    1080
#define IDC_ED_TB_PSIZE    1081
#define IDC_SL_TB_HSIZE    1082
#define IDC_ED_TB_HSIZE    1083
#define IDC_CK_TB_PBG      1084   // persistent: capsule background
#define IDC_CK_TB_HBG      1085   // hover: capsule background
#define IDC_LB_TB_PSIZE    1086
#define IDC_LB_TB_HSIZE    1087
#define IDC_LB_TB_APPEAR   1088   // "Appearance" row: color + shadow
#define IDC_TABS           1089   // Edge popup / Taskbar tabs
#define IDC_GB_EDGEPOP     1090   // group boxes need ids to show/hide per tab;
                                  // the two outer page boxes ("Edge Popup" /
                                  // "Taskbar screen indicators") were removed
                                  // ids kept: the page lists null-skip them
#define IDC_GB_ACT         1091
#define IDC_GB_TRIGGER     1092
#define IDC_GB_POPUP       1093
#define IDC_GB_WINDOWS     1094
#define IDC_GB_TBAR        1095
#define IDC_GB_PERSIST     1096
#define IDC_GB_HOVER       1097
#define IDC_CK_CLOSEBTN    1098   // per-card close buttons
#define IDC_SL_CARDW       1099   // card width slider
#define IDC_ED_CARDW       1101   // its numeric buddy (1100 is the pill ?)
#define IDC_SL_CARDH       1231   // card max-height slider
#define IDC_ED_CARDH       1232   // its numeric buddy

// (?) help buttons: hover = tooltip, click = explanation window.
// They are owner-drawn circles and are moved
// at runtime to hug the end of the preceding label's text.
#define IDC_QQ_DWELL       1102
#define IDC_QQ_ZONE        1103
#define IDC_QQ_STOP        1104
#define IDC_QQ_FGS         1105
#define IDC_QQ_EDGES       1106
#define IDC_QQ_SIDE        1107
#define IDC_QQ_CHAIN       1108
#define IDC_QQ_GRACE       1109
#define IDC_QQ_TITLE       1110
#define IDC_QQ_SHOW        1111
#define IDC_QQ_VD          1112
#define IDC_QQ_START       1113
#define IDC_QQ_REDELAY     1114
#define IDC_QQ_STAY       1116
#define IDC_QQ_CROSSWIN    1115
#define IDC_QQ_CROSS       1115   // cross-window setting (alias)
#define IDC_QQ_THUMBOVER   1117   // removed; number not reused
#define IDC_QQ_EXCLUDE     1119
#define IDC_QQ_PEEK        1120
// (?) help for the rows (activation master switch, taskbar
// arrow customization, hover chips).
#define IDC_QQ_ENABLE      1121
#define IDC_QQ_TB_ARROWS   1122
#define IDC_QQ_TB_HOVER    1123
#define IDC_QQ_TB_SHAPE    1125
#define IDC_QQ_TB_SPOT     1126
#define IDC_QQ_TBCOLOR     1127
#define IDC_QQ_TB_PERSIST  1128
#define IDC_QQ_TB_PSIZE    1131
#define IDC_QQ_TB_HSIZE    1132
#define IDC_QQ_TB_PBG      1133
#define IDC_QQ_TB_HBG      1134
#define IDC_QQ_CLOSEBTN    1135   // per-card close buttons
#define IDC_QQ_CARDW       1136   // card width
#define IDC_QQ_CARDH       1137   // card max height
// The (?) next to the activation pill: one button, generic help.
#define IDC_QQ_ACT         1100

// Activation: pill toggle inside one fixed-size Activation
// group; the inactive mode's rows are hidden (the dialog never resizes).
#define IDC_PILL_ACT       1221
#define IDC_RD_ED_AUTO     1222
#define IDC_RD_ED_SIDES    1223
#define IDC_ST_ADVDESC     1229
#define IDC_LB_CROSS       1210
#define IDC_LB_DWELL       1211
#define IDC_LB_ZONE        1212
#define IDC_LB_STOP        1213
#define IDC_LB_SIDE        1214
#define IDC_LB_CHAIN       1215
#define IDC_LB_GRACE       1216
#define IDC_LB_TITLE       1217
#define IDC_LB_CARDW       1230   // card width label
#define IDC_LB_CARDH       1233   // card max-height label
#define IDC_LB_REDELAY      1224
#define IDC_LB_STAY       1225
#define IDC_LB_EXCLUDE     1226
#define IDC_LB_VD          1218
#define IDC_LB_EDGES       1219
#define IDC_BTN_ADVANCED   1220
#define IDC_BTN_REBUILD    1202

#ifndef IDC_STATIC
#define IDC_STATIC         -1
#endif
