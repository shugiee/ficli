#pragma once

// Custom color slots — defined via init_color() with the Everforest
// palette. Indices 16-23 sit above the standard 16 ANSI colors so they don't
// clobber terminal defaults.
#define CUST_BG     16
#define CUST_RED    17
#define CUST_GREEN  18
#define CUST_YELLOW 19
#define CUST_BLUE   20
#define CUST_PURPLE 21
#define CUST_AQUA   22
#define CUST_FG     23
#define CUST_SURFACE 24

// Color pair indices — shared by all UI modules.
#define COLOR_NORMAL      1  // CUST_FG on CUST_BG — default text
#define COLOR_HEADER      2  // CUST_BG on CUST_BLUE — header bar
#define COLOR_SELECTED    3  // CUST_BG on CUST_FG — selected item
#define COLOR_STATUS      4  // CUST_BG on CUST_BLUE — status bar
#define COLOR_FORM        5  // CUST_BG on CUST_FG — modal dialogs
#define COLOR_FORM_ACTIVE 6  // CUST_BG on CUST_AQUA — active form field
#define COLOR_EXPENSE     7  // CUST_RED on CUST_BG — expense amounts
#define COLOR_INCOME      8  // CUST_GREEN on CUST_BG — income amounts
#define COLOR_INFO        9  // CUST_AQUA on CUST_BG — info text
#define COLOR_FORM_DROPDOWN 10 // CUST_BG on CUST_SURFACE — dropdown layer
#define COLOR_ERROR       11 // CUST_RED on CUST_BG — error borders/messages
