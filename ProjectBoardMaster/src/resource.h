#pragma once

// dialogs
#define IDD_MAIN                100
#define IDD_SETTINGS            101
#define IDD_IO                  102
#define IDD_LOG                 103

#define IDR_MAINMENU            200

// main dialog
#define IDC_TAB                 1000
#define IDC_STATUSTEXT          1001

// settings page - configuration file
#define IDC_CFG_PATH            1100
#define IDC_CFG_BROWSE          1101
#define IDC_CFG_LOAD            1102
#define IDC_CFG_SAVE            1103
#define IDC_CFG_DEFAULT         1104
#define IDC_CFG_NOTE            1105

// settings page - machine configuration directory
#define IDC_PROFILE_DIR         1110
#define IDC_PROFILE_BROWSE      1111
#define IDC_PROFILE_RELOAD      1112
#define IDC_PROFILE_INFO        1113

// settings page - boards
#define IDC_BOARDS_NUM          1120
#define IDC_BOARDS_SPIN         1121
#define IDC_BOARDS_LIST         1122
#define IDC_TRANSPORT           1123
#define IDC_PORT                1124
#define IDC_HOST                1125
#define IDC_TCPPORT             1126
#define IDC_BOARD_APPLY         1127
#define IDC_CONNECT_ALL         1128
#define IDC_DISCONNECT_ALL      1129
#define IDC_IDENTIFY            1130

// settings page - containers and right-column labels, named so the resize
// logic can anchor them
#define IDC_GRP_CFG             1140
#define IDC_GRP_PROFILE         1141
#define IDC_GRP_BOARDS          1142
#define IDC_LBL_TRANSPORT       1143
#define IDC_LBL_PORT            1144
#define IDC_LBL_HOST            1145
#define IDC_LBL_TCPPORT         1146

// I/O page
#define IDC_IO_BOARD            1200
#define IDC_IO_ARM              1201
#define IDC_IO_NOTE             1202

#define IDC_AI_CONN             1210
#define IDC_AI_PIN              1211
#define IDC_AI_REG              1212
#define IDC_AI_ACT              1213
#define IDC_AI_VALUE            1214
#define IDC_AI_EXTRA            1215
#define IDC_AI_STATUS           1216

#define IDC_DI_CONN             1220
#define IDC_DI_PIN              1221
#define IDC_DI_REG              1222
#define IDC_DI_ACT              1223
#define IDC_DI_VALUE            1224
#define IDC_DI_EXTRA            1225
#define IDC_DI_STATUS           1226

#define IDC_AO_CONN             1230
#define IDC_AO_PIN              1231
#define IDC_AO_REG              1232
#define IDC_AO_ACT              1233
#define IDC_AO_VALUE            1234
#define IDC_AO_EXTRA            1235
#define IDC_AO_STATUS           1236

#define IDC_DO_CONN             1240
#define IDC_DO_PIN              1241
#define IDC_DO_REG              1242
#define IDC_DO_ACT              1243
#define IDC_DO_VALUE            1244
#define IDC_DO_EXTRA            1245
#define IDC_DO_STATUS           1246

// log page
#define IDC_LOG_TEXT            1300
#define IDC_LOG_CLEAR           1301
#define IDC_LOG_SAVE            1302
#define IDC_LOG_INFO            1303

// menu commands
#define IDM_FILE_LOAD           40001
#define IDM_FILE_SAVE           40002
#define IDM_FILE_SAVEAS         40003
#define IDM_FILE_EXIT           40004
#define IDM_BOARD_CONNECT       40010
#define IDM_BOARD_DISCONNECT    40011
#define IDM_BOARD_IDENTIFY      40012
#define IDM_HELP_ABOUT          40020
