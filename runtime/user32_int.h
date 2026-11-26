/* user32 core - internal shared definitions (S5).
 *
 * Split across: user32_class.c (atoms, window classes, class data, registered messages),
 * user32_wnd.c (window table, CreateWindowEx/DestroyWindow, text, window data, hierarchy, lookup, init),
 * user32_geom.c (origins, clipping, visibility, ShowWindow, SetWindowPos, rects, AdjustWindowRect),
 * user32_msg.c (queues, send/post, GetMessage loop, dispatch, timers, DefWindowProc),
 * user32_paint.c (update regions, BeginPaint/EndPaint, DCs, WM_PAINT generation, back-buffer flush),
 * user32_nc.c (non-client insets, hit testing, frame painting, weak menu/dialog hooks),
 * user32_input.c (X events to messages, activation/focus, capture, cursor, keys, hover),
 * user32_icon.c (icons/cursors: system, RT_GROUP_ICON/RT_ICON, .ico), user32_sys.c (metrics, colours, DPI).
 */
#ifndef AW_USER32_INT_H
#define AW_USER32_INT_H
#include "gui.h"
#include <pthread.h>

/* ------------------------------------------------------------------ messages */
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETREDRAW 0x000B
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUERYENDSESSION 0x0011
#define WM_QUIT 0x0012
#define WM_QUERYOPEN 0x0013
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_ENDSESSION 0x0016
#define WM_SHOWWINDOW 0x0018
#define WM_SETTINGCHANGE 0x001A
#define WM_DEVMODECHANGE 0x001B
#define WM_ACTIVATEAPP 0x001C
#define WM_FONTCHANGE 0x001D
#define WM_TIMECHANGE 0x001E
#define WM_CANCELMODE 0x001F
#define WM_SETCURSOR 0x0020
#define WM_MOUSEACTIVATE 0x0021
#define WM_CHILDACTIVATE 0x0022
#define WM_QUEUESYNC 0x0023
#define WM_GETMINMAXINFO 0x0024
#define WM_PAINTICON 0x0026
#define WM_ICONERASEBKGND 0x0027
#define WM_NEXTDLGCTL 0x0028
#define WM_DRAWITEM 0x002B
#define WM_MEASUREITEM 0x002C
#define WM_DELETEITEM 0x002D
#define WM_VKEYTOITEM 0x002E
#define WM_CHARTOITEM 0x002F
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_SETHOTKEY 0x0032
#define WM_GETHOTKEY 0x0033
#define WM_QUERYDRAGICON 0x0037
#define WM_COMPAREITEM 0x0039
#define WM_GETOBJECT 0x003D
#define WM_COMPACTING 0x0041
#define WM_COMMNOTIFY 0x0044
#define WM_WINDOWPOSCHANGING 0x0046
#define WM_WINDOWPOSCHANGED 0x0047
#define WM_POWER 0x0048
#define WM_COPYDATA 0x004A
#define WM_CANCELJOURNAL 0x004B
#define WM_NOTIFY 0x004E
#define WM_INPUTLANGCHANGEREQUEST 0x0050
#define WM_INPUTLANGCHANGE 0x0051
#define WM_TCARD 0x0052
#define WM_HELP 0x0053
#define WM_USERCHANGED 0x0054
#define WM_NOTIFYFORMAT 0x0055
#define WM_CONTEXTMENU 0x007B
#define WM_STYLECHANGING 0x007C
#define WM_STYLECHANGED 0x007D
#define WM_DISPLAYCHANGE 0x007E
#define WM_GETICON 0x007F
#define WM_SETICON 0x0080
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_NCCALCSIZE 0x0083
#define WM_NCHITTEST 0x0084
#define WM_NCPAINT 0x0085
#define WM_NCACTIVATE 0x0086
#define WM_GETDLGCODE 0x0087
#define WM_SYNCPAINT 0x0088
#define WM_NCMOUSEMOVE 0x00A0
#define WM_NCLBUTTONDOWN 0x00A1
#define WM_NCLBUTTONUP 0x00A2
#define WM_NCLBUTTONDBLCLK 0x00A3
#define WM_NCRBUTTONDOWN 0x00A4
#define WM_NCRBUTTONUP 0x00A5
#define WM_NCRBUTTONDBLCLK 0x00A6
#define WM_NCMBUTTONDOWN 0x00A7
#define WM_NCMBUTTONUP 0x00A8
#define WM_NCMBUTTONDBLCLK 0x00A9
#define WM_NCXBUTTONDOWN 0x00AB
#define WM_NCXBUTTONUP 0x00AC
#define WM_NCXBUTTONDBLCLK 0x00AD
#define WM_INPUT_DEVICE_CHANGE 0x00FE
#define WM_INPUT 0x00FF
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_SYSDEADCHAR 0x0107
#define WM_UNICHAR 0x0109
#define WM_KEYLAST 0x0109
#define WM_IME_STARTCOMPOSITION 0x010D
#define WM_IME_ENDCOMPOSITION 0x010E
#define WM_IME_COMPOSITION 0x010F
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_HSCROLL 0x0114
#define WM_VSCROLL 0x0115
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_GESTURE 0x0119
#define WM_MENUSELECT 0x011F
#define WM_MENUCHAR 0x0120
#define WM_ENTERIDLE 0x0121
#define WM_MENURBUTTONUP 0x0122
#define WM_MENUDRAG 0x0123
#define WM_MENUGETOBJECT 0x0124
#define WM_UNINITMENUPOPUP 0x0125
#define WM_MENUCOMMAND 0x0126
#define WM_CHANGEUISTATE 0x0127
#define WM_UPDATEUISTATE 0x0128
#define WM_QUERYUISTATE 0x0129
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEFIRST 0x0200
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MBUTTONDBLCLK 0x0209
#define WM_MOUSEWHEEL 0x020A
#define WM_XBUTTONDOWN 0x020B
#define WM_XBUTTONUP 0x020C
#define WM_XBUTTONDBLCLK 0x020D
#define WM_MOUSEHWHEEL 0x020E
#define WM_MOUSELAST 0x020E
#define WM_PARENTNOTIFY 0x0210
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_NEXTMENU 0x0213
#define WM_SIZING 0x0214
#define WM_CAPTURECHANGED 0x0215
#define WM_MOVING 0x0216
#define WM_POWERBROADCAST 0x0218
#define WM_DEVICECHANGE 0x0219
#define WM_MDICREATE 0x0220
#define WM_MDIDESTROY 0x0221
#define WM_MDIACTIVATE 0x0222
#define WM_MDIRESTORE 0x0223
#define WM_MDINEXT 0x0224
#define WM_MDIMAXIMIZE 0x0225
#define WM_MDITILE 0x0226
#define WM_MDICASCADE 0x0227
#define WM_MDIICONARRANGE 0x0228
#define WM_MDIGETACTIVE 0x0229
#define WM_MDISETMENU 0x0230
#define WM_ENTERSIZEMOVE 0x0231
#define WM_EXITSIZEMOVE 0x0232
#define WM_DROPFILES 0x0233
#define WM_MDIREFRESHMENU 0x0234
#define WM_DPICHANGED 0x02E0
#define WM_GETDPISCALEDSIZE 0x02E4
#define WM_CUT 0x0300
#define WM_COPY 0x0301
#define WM_PASTE 0x0302
#define WM_CLEAR 0x0303
#define WM_UNDO 0x0304
#define WM_RENDERFORMAT 0x0305
#define WM_RENDERALLFORMATS 0x0306
#define WM_DESTROYCLIPBOARD 0x0307
#define WM_DRAWCLIPBOARD 0x0308
#define WM_PAINTCLIPBOARD 0x0309
#define WM_VSCROLLCLIPBOARD 0x030A
#define WM_SIZECLIPBOARD 0x030B
#define WM_ASKCBFORMATNAME 0x030C
#define WM_CHANGECBCHAIN 0x030D
#define WM_HSCROLLCLIPBOARD 0x030E
#define WM_QUERYNEWPALETTE 0x030F
#define WM_PALETTEISCHANGING 0x0310
#define WM_PALETTECHANGED 0x0311
#define WM_HOTKEY 0x0312
#define WM_PRINT 0x0317
#define WM_PRINTCLIENT 0x0318
#define WM_APPCOMMAND 0x0319
#define WM_THEMECHANGED 0x031A
#define WM_CLIPBOARDUPDATE 0x031D
#define WM_DWMCOMPOSITIONCHANGED 0x031E
#define WM_DWMNCRENDERINGCHANGED 0x031F
#define WM_DWMCOLORIZATIONCOLORCHANGED 0x0320
#define WM_DWMWINDOWMAXIMIZEDCHANGE 0x0321
#define WM_GETTITLEBARINFOEX 0x033F
#define WM_HANDHELDFIRST 0x0358
#define WM_HANDHELDLAST 0x035F
#define WM_APP 0x8000
#define WM_USER 0x0400

#define EM_GETSEL 0x00B0
#define EM_SETSEL 0x00B1
#define EM_GETRECT 0x00B2
#define EM_SETRECT 0x00B3
#define EM_SETRECTNP 0x00B4
#define EM_SCROLL 0x00B5
#define EM_LINESCROLL 0x00B6
#define EM_SCROLLCARET 0x00B7
#define EM_GETMODIFY 0x00B8
#define EM_SETMODIFY 0x00B9
#define EM_GETLINECOUNT 0x00BA
#define EM_LINEINDEX 0x00BB
#define EM_SETHANDLE 0x00BC
#define EM_GETHANDLE 0x00BD
#define EM_GETTHUMB 0x00BE
#define EM_LINELENGTH 0x00C1
#define EM_REPLACESEL 0x00C2
#define EM_GETLINE 0x00C4
#define EM_LIMITTEXT 0x00C5
#define EM_CANUNDO 0x00C6
#define EM_UNDO 0x00C7
#define EM_FMTLINES 0x00C8
#define EM_LINEFROMCHAR 0x00C9
#define EM_SETTABSTOPS 0x00CB
#define EM_SETPASSWORDCHAR 0x00CC
#define EM_EMPTYUNDOBUFFER 0x00CD
#define EM_GETFIRSTVISIBLELINE 0x00CE
#define EM_SETREADONLY 0x00CF
#define EM_SETWORDBREAKPROC 0x00D0
#define EM_GETWORDBREAKPROC 0x00D1
#define EM_GETPASSWORDCHAR 0x00D2
#define EM_SETMARGINS 0x00D3
#define EM_GETMARGINS 0x00D4
#define EM_SETLIMITTEXT EM_LIMITTEXT
#define EM_GETLIMITTEXT 0x00D5
#define EM_POSFROMCHAR 0x00D6
#define EM_CHARFROMPOS 0x00D7
#define EM_SETIMESTATUS 0x00D8
#define EM_GETIMESTATUS 0x00D9
#define EM_SETCUEBANNER 0x1501
#define EM_GETCUEBANNER 0x1502
#define EM_SETCARETINDEX 0x00D9
#define LB_ADDSTRING 0x0180
#define LB_INSERTSTRING 0x0181
#define LB_DELETESTRING 0x0182
#define LB_SELITEMRANGEEX 0x0183
#define LB_RESETCONTENT 0x0184
#define LB_SETSEL 0x0185
#define LB_SETCURSEL 0x0186
#define LB_GETSEL 0x0187
#define LB_GETCURSEL 0x0188
#define LB_GETTEXT 0x0189
#define LB_GETTEXTLEN 0x018A
#define LB_GETCOUNT 0x018B
#define LB_SELECTSTRING 0x018C
#define LB_DIR 0x018D
#define LB_GETTOPINDEX 0x018E
#define LB_FINDSTRING 0x018F
#define LB_GETSELCOUNT 0x0190
#define LB_GETSELITEMS 0x0191
#define LB_SETTABSTOPS 0x0192
#define LB_GETHORIZONTALEXTENT 0x0193
#define LB_SETHORIZONTALEXTENT 0x0194
#define LB_SETCOLUMNWIDTH 0x0195
#define LB_ADDFILE 0x0196
#define LB_SETTOPINDEX 0x0197
#define LB_GETITEMRECT 0x0198
#define LB_GETITEMDATA 0x0199
#define LB_SETITEMDATA 0x019A
#define LB_SELITEMRANGE 0x019B
#define LB_SETANCHORINDEX 0x019C
#define LB_GETANCHORINDEX 0x019D
#define LB_SETCARETINDEX 0x019E
#define LB_GETCARETINDEX 0x019F
#define LB_SETITEMHEIGHT 0x01A0
#define LB_GETITEMHEIGHT 0x01A1
#define LB_FINDSTRINGEXACT 0x01A2
#define LB_SETLOCALE 0x01A5
#define LB_GETLOCALE 0x01A6
#define LB_SETCOUNT 0x01A7
#define LB_INITSTORAGE 0x01A8
#define LB_ITEMFROMPOINT 0x01A9
#define LB_GETLISTBOXINFO 0x01B2
#define CB_GETEDITSEL 0x0140
#define CB_LIMITTEXT 0x0141
#define CB_SETEDITSEL 0x0142
#define CB_ADDSTRING 0x0143
#define CB_DELETESTRING 0x0144
#define CB_DIR 0x0145
#define CB_GETCOUNT 0x0146
#define CB_GETCURSEL 0x0147
#define CB_GETLBTEXT 0x0148
#define CB_GETLBTEXTLEN 0x0149
#define CB_INSERTSTRING 0x014A
#define CB_RESETCONTENT 0x014B
#define CB_FINDSTRING 0x014C
#define CB_SELECTSTRING 0x014D
#define CB_SETCURSEL 0x014E
#define CB_CHOICE 0x014F
#define CB_GETITEMDATA 0x0150
#define CB_SETITEMDATA 0x0151
#define CB_GETDROPPEDCONTROLRECT 0x0152
#define CB_SETITEMHEIGHT 0x0153
#define CB_GETITEMHEIGHT 0x0154
#define CB_SETEXTENDEDUI 0x0155
#define CB_GETEXTENDEDUI 0x0156
#define CB_GETDROPPEDSTATE 0x0157
#define CB_FINDSTRINGEXACT 0x0158
#define CB_SETLOCALE 0x0159
#define CB_GETLOCALE 0x015A
#define CB_GETTOPINDEX 0x015B
#define CB_SETTOPINDEX 0x015C
#define CB_GETHORIZONTALEXTENT 0x015D
#define CB_SETHORIZONTALEXTENT 0x015E
#define CB_GETDROPPEDWIDTH 0x015F
#define CB_SETDROPPEDWIDTH 0x0160
#define CB_INITSTORAGE 0x0161
#define CB_MULTIPLEADDSTRING 0x0163
#define CB_GETCOMBOBOXINFO 0x0164
#define CB_SETCUEBANNER 0x1703
#define CB_GETCUEBANNER 0x1704
#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3
#define BM_SETSTYLE 0x00F4
#define BM_CLICK 0x00F5
#define BM_GETIMAGE 0x00F6
#define BM_SETIMAGE 0x00F7
#define BM_SETDONTCLICK 0x00F8
#define STM_SETICON 0x0170
#define STM_GETICON 0x0171
#define STM_SETIMAGE 0x0172
#define STM_GETIMAGE 0x0173

/* ------------------------------------------------------------------ styles */
#define WS_OVERLAPPED 0x00000000L
#define WS_POPUP 0x80000000L
#define WS_CHILD 0x40000000L
#define WS_MINIMIZE 0x20000000L
#define WS_VISIBLE 0x10000000L
#define WS_DISABLED 0x08000000L
#define WS_CLIPSIBLINGS 0x04000000L
#define WS_CLIPCHILDREN 0x02000000L
#define WS_MAXIMIZE 0x01000000L
#define WS_CAPTION 0x00C00000L
#define WS_BORDER 0x00800000L
#define WS_DLGFRAME 0x00400000L
#define WS_VSCROLL 0x00200000L
#define WS_HSCROLL 0x00100000L
#define WS_SYSMENU 0x00080000L
#define WS_THICKFRAME 0x00040000L
#define WS_GROUP 0x00020000L
#define WS_TABSTOP 0x00010000L
#define WS_MINIMIZEBOX 0x00020000L
#define WS_MAXIMIZEBOX 0x00010000L
#define WS_TILED WS_OVERLAPPED
#define WS_ICONIC WS_MINIMIZE
#define WS_SIZEBOX WS_THICKFRAME
#define WS_OVERLAPPEDWINDOW (WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX)
#define WS_POPUPWINDOW (WS_POPUP | WS_BORDER | WS_SYSMENU)
#define WS_CHILDWINDOW WS_CHILD
#define WS_EX_DLGMODALFRAME 0x00000001L
#define WS_EX_NOPARENTNOTIFY 0x00000004L
#define WS_EX_TOPMOST 0x00000008L
#define WS_EX_ACCEPTFILES 0x00000010L
#define WS_EX_TRANSPARENT 0x00000020L
#define WS_EX_MDICHILD 0x00000040L
#define WS_EX_TOOLWINDOW 0x00000080L
#define WS_EX_WINDOWEDGE 0x00000100L
#define WS_EX_CLIENTEDGE 0x00000200L
#define WS_EX_CONTEXTHELP 0x00000400L
#define WS_EX_RIGHT 0x00001000L
#define WS_EX_LEFT 0x00000000L
#define WS_EX_RTLREADING 0x00002000L
#define WS_EX_LEFTSCROLLBAR 0x00004000L
#define WS_EX_CONTROLPARENT 0x00010000L
#define WS_EX_STATICEDGE 0x00020000L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_LAYERED 0x00080000L
#define WS_EX_NOINHERITLAYOUT 0x00100000L
#define WS_EX_NOREDIRECTIONBITMAP 0x00200000L
#define WS_EX_LAYOUTRTL 0x00400000L
#define WS_EX_COMPOSITED 0x02000000L
#define WS_EX_NOACTIVATE 0x08000000L
#define CS_VREDRAW 0x0001
#define CS_HREDRAW 0x0002
#define CS_DBLCLKS 0x0008
#define CS_OWNDC 0x0020
#define CS_CLASSDC 0x0040
#define CS_PARENTDC 0x0080
#define CS_NOCLOSE 0x0200
#define CS_SAVEBITS 0x0800
#define CS_BYTEALIGNCLIENT 0x1000
#define CS_BYTEALIGNWINDOW 0x2000
#define CS_GLOBALCLASS 0x4000
#define CW_USEDEFAULT ((INT)0x80000000)
#define HWND_TOP ((HWND)0)
#define HWND_BOTTOM ((HWND)1)
#define HWND_TOPMOST ((HWND)(intptr_t)-1)
#define HWND_NOTOPMOST ((HWND)(intptr_t)-2)
#define HWND_MESSAGE ((HWND)(intptr_t)-3)
#define HWND_BROADCAST ((HWND)(intptr_t)0xFFFF)
#define HWND_DESKTOP ((HWND)0)
#define SWP_NOSIZE 0x0001
#define SWP_NOMOVE 0x0002
#define SWP_NOZORDER 0x0004
#define SWP_NOREDRAW 0x0008
#define SWP_NOACTIVATE 0x0010
#define SWP_FRAMECHANGED 0x0020
#define SWP_SHOWWINDOW 0x0040
#define SWP_HIDEWINDOW 0x0080
#define SWP_NOCOPYBITS 0x0100
#define SWP_NOOWNERZORDER 0x0200
#define SWP_NOSENDCHANGING 0x0400
#define SWP_DRAWFRAME SWP_FRAMECHANGED
#define SWP_NOREPOSITION SWP_NOOWNERZORDER
#define SWP_DEFERERASE 0x2000
#define SWP_ASYNCWINDOWPOS 0x4000
#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_NORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_MAXIMIZE 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_MINIMIZE 6
#define SW_SHOWMINNOACTIVE 7
#define SW_SHOWNA 8
#define SW_RESTORE 9
#define SW_SHOWDEFAULT 10
#define SW_FORCEMINIMIZE 11
#define RDW_INVALIDATE 0x0001
#define RDW_INTERNALPAINT 0x0002
#define RDW_ERASE 0x0004
#define RDW_VALIDATE 0x0008
#define RDW_NOINTERNALPAINT 0x0010
#define RDW_NOERASE 0x0020
#define RDW_NOCHILDREN 0x0040
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define RDW_ERASENOW 0x0200
#define RDW_FRAME 0x0400
#define RDW_NOFRAME 0x0800
#define GWL_WNDPROC (-4)
#define GWL_HINSTANCE (-6)
#define GWL_HWNDPARENT (-8)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWL_USERDATA (-21)
#define GWL_ID (-12)
#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_HWNDPARENT (-8)
#define GWLP_STYLE (-16)
#define GWLP_EXSTYLE (-20)
#define GWLP_USERDATA (-21)
#define GWLP_ID (-12)
#define GW_HWNDFIRST 0
#define GW_HWNDLAST 1
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_CHILD 5
#define GW_ENABLEDPOPUP 6
#define GW_MAX 6
#define GA_PARENT 1
#define GA_ROOT 2
#define GA_ROOTOWNER 3
#define GW_OWNER 4
#define GW_CHILD 5
#define GW_ENABLEDPOPUP 6
#define CWP_ALL 0x0000
#define CWP_SKIPINVISIBLE 0x0001
#define CWP_SKIPDISABLED 0x0002
#define CWP_SKIPTRANSPARENT 0x0004
#define HTNOWHERE 0
#define HTCLIENT 1
#define HTCAPTION 2
#define HTSYSMENU 3
#define HTGROWBOX 4
#define HTSIZE HTGROWBOX
#define HTMENU 5
#define HTHSCROLL 6
#define HTVSCROLL 7
#define HTMINBUTTON 8
#define HTMAXBUTTON 9
#define HTLEFT 10
#define HTRIGHT 11
#define HTTOP 12
#define HTTOPLEFT 13
#define HTTOPRIGHT 14
#define HTBOTTOM 15
#define HTBOTTOMLEFT 16
#define HTBOTTOMRIGHT 17
#define HTBORDER 18
#define HTREDUCE HTMINBUTTON
#define HTZOOM HTMAXBUTTON
#define HTSIZEFIRST HTLEFT
#define HTSIZELAST HTBOTTOMRIGHT
#define HTOBJECT 19
#define HTCLOSE 20
#define HTHELP 21
#define HTTRANSPARENT (-1)
#define HTERROR (-2)
#define SIZE_RESTORED 0
#define SIZE_MINIMIZED 1
#define SIZE_MAXIMIZED 2
#define SIZE_MAXSHOW 3
#define SIZE_MAXHIDE 4
#define WA_INACTIVE 0
#define WA_ACTIVE 1
#define WA_CLICKACTIVE 2
#define MA_ACTIVATE 1
#define MA_ACTIVATEANDEAT 2
#define MA_NOACTIVATE 3
#define MA_NOACTIVATEANDEAT 4
#define SW_SCROLLCHILDREN 0x0001
#define SW_INVALIDATE 0x0002
#define SW_ERASE 0x0004
#define SW_SMOOTHSCROLL 0x0010
#define SB_HORZ 0
#define SB_VERT 1
#define SB_CTL 2
#define SB_BOTH 3
#define SB_LINEUP 0
#define SB_LINELEFT 0
#define SB_LINEDOWN 1
#define SB_LINERIGHT 1
#define SB_PAGEUP 2
#define SB_PAGELEFT 2
#define SB_PAGEDOWN 3
#define SB_PAGERIGHT 3
#define SB_THUMBPOSITION 4
#define SB_THUMBTRACK 5
#define SB_TOP 6
#define SB_LEFT 6
#define SB_BOTTOM 7
#define SB_RIGHT 7
#define SB_ENDSCROLL 8
#define SIF_RANGE 0x0001
#define SIF_PAGE 0x0002
#define SIF_POS 0x0004
#define SIF_DISABLENOSCROLL 0x0008
#define SIF_TRACKPOS 0x0010
#define SIF_ALL (SIF_RANGE | SIF_PAGE | SIF_POS | SIF_TRACKPOS)
#define ESB_ENABLE_BOTH 0x0000
#define ESB_DISABLE_BOTH 0x0003
#define ESB_DISABLE_LEFT 0x0001
#define ESB_DISABLE_RIGHT 0x0002
#define ESB_DISABLE_UP 0x0001
#define ESB_DISABLE_DOWN 0x0002
#define ESB_DISABLE_LTUP ESB_DISABLE_LEFT
#define ESB_DISABLE_RTDN ESB_DISABLE_RIGHT
#define OBJID_CLIENT ((LONG)0xFFFFFFFC)
#define SC_SIZE 0xF000
#define SC_MOVE 0xF010
#define SC_MINIMIZE 0xF020
#define SC_MAXIMIZE 0xF030
#define SC_NEXTWINDOW 0xF040
#define SC_PREVWINDOW 0xF050
#define SC_CLOSE 0xF060
#define SC_VSCROLL 0xF070
#define SC_HSCROLL 0xF080
#define SC_MOUSEMENU 0xF090
#define SC_KEYMENU 0xF100
#define SC_ARRANGE 0xF110
#define SC_RESTORE 0xF120
#define SC_TASKLIST 0xF130
#define SC_SCREENSAVE 0xF140
#define SC_HOTKEY 0xF150
#define SC_DEFAULT 0xF160
#define SC_MONITORPOWER 0xF170
#define SC_CONTEXTHELP 0xF180
#define SC_SEPARATOR 0xF00F
#define MF_BYCOMMAND 0x00000000L
#define MF_BYPOSITION 0x00000400L
#define MF_ENABLED 0x00000000L
#define MF_GRAYED 0x00000001L
#define MF_DISABLED 0x00000002L
#define MF_CHECKED 0x00000008L
#define MF_POPUP 0x00000010L
#define MF_SEPARATOR 0x00000400L

/* ------------------------------------------------------------------ misc constants */
#define IDC_ARROW 32512
#define IDC_IBEAM 32513
#define IDC_WAIT 32514
#define IDC_CROSS 32515
#define IDC_UPARROW 32516
#define IDC_SIZENWSE 32642
#define IDC_SIZENESW 32643
#define IDC_SIZEWE 32644
#define IDC_SIZENS 32645
#define IDC_SIZEALL 32646
#define IDC_NO 32648
#define IDC_HAND 32649
#define IDC_APPSTARTING 32650
#define IDC_HELP 32651
#define IDC_PIN 32671
#define IDC_PERSON 32672
#define IDI_APPLICATION 32512
#define IDI_ERROR 32513
#define IDI_QUESTION 32514
#define IDI_WARNING 32515
#define IDI_INFORMATION 32516
#define IDI_WINLOGO 32517
#define IDI_SHIELD 32518
#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_DESKTOP 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_INACTIVECAPTION 3
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_CAPTIONTEXT 9
#define COLOR_ACTIVEBORDER 10
#define COLOR_INACTIVEBORDER 11
#define COLOR_APPWORKSPACE 12
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_BTNFACE 15
#define COLOR_3DFACE 15
#define COLOR_BTNSHADOW 16
#define COLOR_3DSHADOW 16
#define COLOR_GRAYTEXT 17
#define COLOR_BTNTEXT 18
#define COLOR_INACTIVECAPTIONTEXT 19
#define COLOR_BTNHIGHLIGHT 20
#define COLOR_3DHIGHLIGHT 20
#define COLOR_3DHILIGHT 20
#define COLOR_3DDKSHADOW 21
#define COLOR_3DLIGHT 22
#define COLOR_INFOTEXT 23
#define COLOR_INFOBK 24
#define COLOR_HOTLIGHT 26
#define COLOR_GRADIENTACTIVECAPTION 27
#define COLOR_GRADIENTINACTIVECAPTION 28
#define COLOR_MENUHILIGHT 29
#define COLOR_MENUBAR 30
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDCLOSE 8
#define IDHELP 9
#define BST_UNCHECKED 0
#define BST_CHECKED 1
#define BST_INDETERMINATE 2
#define BM_SETCHECK_DEF 0x00F1

/* clipboard formats */
#define CF_TEXT 1
#define CF_BITMAP 2
#define CF_METAFILEPICT 3
#define CF_SYLK 4
#define CF_DIF 5
#define CF_TIFF 6
#define CF_OEMTEXT 7
#define CF_DIB 8
#define CF_PALETTE 9
#define CF_PENDATA 10
#define CF_RIFF 11
#define CF_WAVE 12
#define CF_UNICODETEXT 13
#define CF_ENHMETAFILE 14
#define CF_HDROP 15
#define CF_LOCALE 16
#define CF_DIBV5 17
#define CF_OWNERDISPLAY 0x0080
#define CF_DSPTEXT 0x0081
#define CF_DSPBITMAP 0x0082
#define CF_DSPMETAFILEPICT 0x0083
#define CF_DSPENHMETAFILE 0x008E
#define CF_PRIVATEFIRST 0x0200
#define CF_PRIVATELAST 0x02FF
#define CF_GDIOBJFIRST 0x0300
#define CF_GDIOBJLAST 0x03FF
#define GMEM_MOVEABLE 0x0002
#define GMEM_ZEROINIT 0x0040
#define GHND (GMEM_MOVEABLE | GMEM_ZEROINIT)

/* ------------------------------------------------------------------ structs */
typedef struct tagWNDCLASSA {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
} WNDCLASSA;
typedef struct tagWNDCLASSW {
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
} WNDCLASSW;
typedef struct tagWNDCLASSEXA {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCSTR lpszMenuName;
    LPCSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXA;
typedef struct tagWNDCLASSEXW {
    UINT cbSize;
    UINT style;
    WNDPROC lpfnWndProc;
    int cbClsExtra;
    int cbWndExtra;
    HINSTANCE hInstance;
    HICON hIcon;
    HCURSOR hCursor;
    HBRUSH hbrBackground;
    LPCWSTR lpszMenuName;
    LPCWSTR lpszClassName;
    HICON hIconSm;
} WNDCLASSEXW;

typedef struct tagCREATESTRUCTA {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCSTR lpszName;
    LPCSTR lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTA;
typedef struct tagCREATESTRUCTW {
    LPVOID lpCreateParams;
    HINSTANCE hInstance;
    HMENU hMenu;
    HWND hwndParent;
    int cy, cx, y, x;
    LONG style;
    LPCWSTR lpszName;
    LPCWSTR lpszClass;
    DWORD dwExStyle;
} CREATESTRUCTW;

typedef struct { HWND hwnd; UINT message; WPARAM wParam; LPARAM lParam; DWORD time; POINT pt; DWORD lPrivate; } MSG;
typedef struct tagPAINTSTRUCT {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} PAINTSTRUCT;
typedef struct tagWINDOWPOS {
    HWND hwnd, hwndInsertAfter;
    int x, y, cx, cy;
    UINT flags;
} WINDOWPOS;
typedef struct tagNCCALCSIZE_PARAMS {
    RECT rgrc[3];
    WINDOWPOS *lppos;
} NCCALCSIZE_PARAMS;
typedef struct tagMINMAXINFO {
    POINT ptReserved, ptMaxSize, ptMaxPosition, ptMinTrackSize, ptMaxTrackSize;
} MINMAXINFO;
typedef struct tagSTYLESTRUCT { DWORD styleOld, styleNew; } STYLESTRUCT;
typedef struct tagSCROLLINFO {
    UINT cbSize, fMask;
    int nMin, nMax;
    UINT nPage;
    int nPos, nTrackPos;
} SCROLLINFO;
typedef struct tagWINDOWPLACEMENT {
    UINT length, flags, showCmd;
    POINT ptMinPosition, ptMaxPosition;
    RECT rcNormalPosition;
} WINDOWPLACEMENT;
typedef struct tagWINDOWINFO {
    DWORD cbSize;
    RECT rcWindow, rcClient;
    DWORD dwStyle, dwExStyle, dwWindowStatus;
    UINT cxWindowBorders, cyWindowBorders;
    ATOM atomWindowType;
    WORD wCreatorVersion;
} WINDOWINFO;
typedef struct tagMONITORINFO { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; } MONITORINFO;
typedef struct tagMONITORINFOEXA { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; char szDevice[32]; } MONITORINFOEXA;
typedef struct tagMONITORINFOEXW { DWORD cbSize; RECT rcMonitor, rcWork; DWORD dwFlags; WCHAR szDevice[32]; } MONITORINFOEXW;
typedef struct tagGUITHREADINFO {
    DWORD cbSize, flags;
    HWND hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret;
    RECT rcCaret;
} GUITHREADINFO;
typedef struct tagCURSORINFO { DWORD cbSize, flags; HCURSOR hCursor; POINT ptScreenPos; } CURSORINFO;
typedef struct tagICONINFO { BOOL fIcon; DWORD xHotspot, yHotspot; HBITMAP hbmMask, hbmColor; } ICONINFO;
typedef struct tagTRACKMOUSEEVENT { DWORD cbSize, dwFlags; HWND hwndTrack; DWORD dwHoverTime; } TRACKMOUSEEVENT;
typedef struct tagLOGFONTA {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    char lfFaceName[32];
} LOGFONTA;
typedef struct tagLOGFONTW {
    LONG lfHeight, lfWidth, lfEscapement, lfOrientation, lfWeight;
    BYTE lfItalic, lfUnderline, lfStrikeOut, lfCharSet, lfOutPrecision, lfClipPrecision, lfQuality, lfPitchAndFamily;
    WCHAR lfFaceName[32];
} LOGFONTW;
typedef struct tagNONCLIENTMETRICSA {
    UINT cbSize;
    int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight;
    LOGFONTA lfCaptionFont;
    int iSmCaptionWidth, iSmCaptionHeight;
    LOGFONTA lfSmCaptionFont;
    int iMenuWidth, iMenuHeight;
    LOGFONTA lfMenuFont, lfStatusFont, lfMessageFont;
    int iPaddedBorderWidth;
} NONCLIENTMETRICSA;
typedef struct tagNONCLIENTMETRICSW {
    UINT cbSize;
    int iBorderWidth, iScrollWidth, iScrollHeight, iCaptionWidth, iCaptionHeight;
    LOGFONTW lfCaptionFont;
    int iSmCaptionWidth, iSmCaptionHeight;
    LOGFONTW lfSmCaptionFont;
    int iMenuWidth, iMenuHeight;
    LOGFONTW lfMenuFont, lfStatusFont, lfMessageFont;
    int iPaddedBorderWidth;
} NONCLIENTMETRICSW;
typedef struct tagICONMETRICSA {
    UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap, iLogFont;
    LOGFONTA lfFont;
} ICONMETRICSA;
typedef struct tagICONMETRICSW {
    UINT cbSize; int iHorzSpacing, iVertSpacing, iTitleWrap, iLogFont;
    LOGFONTW lfFont;
} ICONMETRICSW;
typedef struct tagMINIMIZEDMETRICS { UINT cbSize; int iWidth, iHorzGap, iVertGap, iArrange; } MINIMIZEDMETRICS;
typedef struct tagHIGHCONTRASTA { UINT cbSize; DWORD dwFlags; LPSTR lpszDefaultScheme; } HIGHCONTRASTA;
typedef struct tagHIGHCONTRASTW { UINT cbSize; DWORD dwFlags; LPWSTR lpszDefaultScheme; } HIGHCONTRASTW;
typedef struct tagDEVMODEA { BYTE dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields;
    int dmPositionX, dmPositionY; DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate; BYTE dmFormName[32];
    WORD dmLogPixels; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEA;
typedef struct tagDEVMODEW { WCHAR dmDeviceName[32]; WORD dmSpecVersion, dmDriverVersion, dmSize, dmDriverExtra; DWORD dmFields;
    int dmPositionX, dmPositionY; DWORD dmDisplayOrientation, dmDisplayFixedOutput;
    short dmColor, dmDuplex, dmYResolution, dmTTOption, dmCollate; WCHAR dmFormName[32];
    WORD dmLogPixels; DWORD dmBitsPerPel, dmPelsWidth, dmPelsHeight, dmDisplayFlags, dmDisplayFrequency;
    DWORD dmICMMethod, dmICMIntent, dmMediaType, dmDitherType, dmReserved1, dmReserved2, dmPanningWidth, dmPanningHeight;
} DEVMODEW;
typedef struct tagDISPLAY_DEVICEA { DWORD cb; char DeviceName[32], DeviceString[128]; DWORD StateFlags; char DeviceID[128], DeviceKey[128]; } DISPLAY_DEVICEA;
typedef struct tagDISPLAY_DEVICEW { DWORD cb; WCHAR DeviceName[32], DeviceString[128]; DWORD StateFlags; WCHAR DeviceID[128], DeviceKey[128]; } DISPLAY_DEVICEW;
typedef struct { UINT cbSize; DWORD dwFlags; HWND hwndActive, hwndFocus, hwndCapture, hwndMenuOwner, hwndMoveSize, hwndCaret; RECT rcCaret; } aw_gti_compat;
typedef struct tagPROPENUMSTRUCT { DWORD cbSize; WNDPROC lpfnWndProc; } dummy_propenum_t;
typedef BOOL(AWCALL *PROPENUMPROC)(HWND, LPCWSTR, HANDLE, ULONG_PTR);
typedef BOOL(AWCALL *PROPENUMPROCA)(HWND, LPCSTR, HANDLE, ULONG_PTR);
typedef BOOL(AWCALL *ENUMWINDOWSPROC)(HWND, LPARAM);


/* ------------------------------------------------------------------ winuser constants (from the mingw winuser.h values) */
#define RT_MANIFEST 24
#define SW_MAX 11
#define SW_PARENTCLOSING 1
#define SW_OTHERZOOM 2
#define SW_PARENTOPENING 3
#define SW_OTHERUNZOOM 4
#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_MBUTTON 0x04
#define VK_XBUTTON1 0x05
#define VK_XBUTTON2 0x06
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_CLEAR 0x0C
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_KANA 0x15
#define VK_HANGEUL 0x15
#define VK_HANGUL 0x15
#define VK_IME_ON 0x16
#define VK_JUNJA 0x17
#define VK_FINAL 0x18
#define VK_HANJA 0x19
#define VK_KANJI 0x19
#define VK_IME_OFF 0x1A
#define VK_ESCAPE 0x1B
#define VK_CONVERT 0x1C
#define VK_NONCONVERT 0x1D
#define VK_ACCEPT 0x1E
#define VK_MODECHANGE 0x1F
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_SELECT 0x29
#define VK_PRINT 0x2A
#define VK_EXECUTE 0x2B
#define VK_SNAPSHOT 0x2C
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_HELP 0x2F
#define VK_LWIN 0x5B
#define VK_RWIN 0x5C
#define VK_APPS 0x5D
#define VK_SLEEP 0x5F
#define VK_NUMPAD0 0x60
#define VK_NUMPAD1 0x61
#define VK_NUMPAD2 0x62
#define VK_NUMPAD3 0x63
#define VK_NUMPAD4 0x64
#define VK_NUMPAD5 0x65
#define VK_NUMPAD6 0x66
#define VK_NUMPAD7 0x67
#define VK_NUMPAD8 0x68
#define VK_NUMPAD9 0x69
#define VK_MULTIPLY 0x6A
#define VK_ADD 0x6B
#define VK_SEPARATOR 0x6C
#define VK_SUBTRACT 0x6D
#define VK_DECIMAL 0x6E
#define VK_DIVIDE 0x6F
#define VK_F1 0x70
#define VK_F2 0x71
#define VK_F3 0x72
#define VK_F4 0x73
#define VK_F5 0x74
#define VK_F6 0x75
#define VK_F7 0x76
#define VK_F8 0x77
#define VK_F9 0x78
#define VK_F10 0x79
#define VK_F11 0x7A
#define VK_F12 0x7B
#define VK_F13 0x7C
#define VK_F14 0x7D
#define VK_F15 0x7E
#define VK_F16 0x7F
#define VK_F17 0x80
#define VK_F18 0x81
#define VK_F19 0x82
#define VK_F20 0x83
#define VK_F21 0x84
#define VK_F22 0x85
#define VK_F23 0x86
#define VK_F24 0x87
#define VK_NAVIGATION_VIEW 0x88
#define VK_NAVIGATION_MENU 0x89
#define VK_NAVIGATION_UP 0x8A
#define VK_NAVIGATION_DOWN 0x8B
#define VK_NAVIGATION_LEFT 0x8C
#define VK_NAVIGATION_RIGHT 0x8D
#define VK_NAVIGATION_ACCEPT 0x8E
#define VK_NAVIGATION_CANCEL 0x8F
#define VK_NUMLOCK 0x90
#define VK_SCROLL 0x91
#define VK_OEM_NEC_EQUAL 0x92
#define VK_OEM_FJ_JISHO 0x92
#define VK_OEM_FJ_MASSHOU 0x93
#define VK_OEM_FJ_TOUROKU 0x94
#define VK_OEM_FJ_LOYA 0x95
#define VK_OEM_FJ_ROYA 0x96
#define VK_LSHIFT 0xA0
#define VK_RSHIFT 0xA1
#define VK_LCONTROL 0xA2
#define VK_RCONTROL 0xA3
#define VK_LMENU 0xA4
#define VK_RMENU 0xA5
#define VK_BROWSER_BACK 0xA6
#define VK_BROWSER_FORWARD 0xA7
#define VK_BROWSER_REFRESH 0xA8
#define VK_BROWSER_STOP 0xA9
#define VK_BROWSER_SEARCH 0xAA
#define VK_BROWSER_FAVORITES 0xAB
#define VK_BROWSER_HOME 0xAC
#define VK_VOLUME_MUTE 0xAD
#define VK_VOLUME_DOWN 0xAE
#define VK_VOLUME_UP 0xAF
#define VK_MEDIA_NEXT_TRACK 0xB0
#define VK_MEDIA_PREV_TRACK 0xB1
#define VK_MEDIA_STOP 0xB2
#define VK_MEDIA_PLAY_PAUSE 0xB3
#define VK_LAUNCH_MAIL 0xB4
#define VK_LAUNCH_MEDIA_SELECT 0xB5
#define VK_LAUNCH_APP1 0xB6
#define VK_LAUNCH_APP2 0xB7
#define VK_OEM_1 0xBA
#define VK_OEM_PLUS 0xBB
#define VK_OEM_COMMA 0xBC
#define VK_OEM_MINUS 0xBD
#define VK_OEM_PERIOD 0xBE
#define VK_OEM_2 0xBF
#define VK_OEM_3 0xC0
#define VK_GAMEPAD_A 0xC3
#define VK_GAMEPAD_B 0xC4
#define VK_GAMEPAD_X 0xC5
#define VK_GAMEPAD_Y 0xC6
#define VK_GAMEPAD_RIGHT_SHOULDER 0xC7
#define VK_GAMEPAD_LEFT_SHOULDER 0xC8
#define VK_GAMEPAD_LEFT_TRIGGER 0xC9
#define VK_GAMEPAD_RIGHT_TRIGGER 0xCA
#define VK_GAMEPAD_DPAD_UP 0xCB
#define VK_GAMEPAD_DPAD_DOWN 0xCC
#define VK_GAMEPAD_DPAD_LEFT 0xCD
#define VK_GAMEPAD_DPAD_RIGHT 0xCE
#define VK_GAMEPAD_MENU 0xCF
#define VK_GAMEPAD_VIEW 0xD0
#define VK_GAMEPAD_LEFT_THUMBSTICK_BUTTON 0xD1
#define VK_GAMEPAD_RIGHT_THUMBSTICK_BUTTON 0xD2
#define VK_GAMEPAD_LEFT_THUMBSTICK_UP 0xD3
#define VK_GAMEPAD_LEFT_THUMBSTICK_DOWN 0xD4
#define VK_GAMEPAD_LEFT_THUMBSTICK_RIGHT 0xD5
#define VK_GAMEPAD_LEFT_THUMBSTICK_LEFT 0xD6
#define VK_GAMEPAD_RIGHT_THUMBSTICK_UP 0xD7
#define VK_GAMEPAD_RIGHT_THUMBSTICK_DOWN 0xD8
#define VK_GAMEPAD_RIGHT_THUMBSTICK_RIGHT 0xD9
#define VK_GAMEPAD_RIGHT_THUMBSTICK_LEFT 0xDA
#define VK_OEM_4 0xDB
#define VK_OEM_5 0xDC
#define VK_OEM_6 0xDD
#define VK_OEM_7 0xDE
#define VK_OEM_8 0xDF
#define VK_OEM_AX 0xE1
#define VK_OEM_102 0xE2
#define VK_ICO_HELP 0xE3
#define VK_ICO_00 0xE4
#define VK_PROCESSKEY 0xE5
#define VK_ICO_CLEAR 0xE6
#define VK_PACKET 0xE7
#define VK_OEM_RESET 0xE9
#define VK_OEM_JUMP 0xEA
#define VK_OEM_PA1 0xEB
#define VK_OEM_PA2 0xEC
#define VK_OEM_PA3 0xED
#define VK_OEM_WSCTRL 0xEE
#define VK_OEM_CUSEL 0xEF
#define VK_OEM_ATTN 0xF0
#define VK_OEM_FINISH 0xF1
#define VK_OEM_COPY 0xF2
#define VK_OEM_AUTO 0xF3
#define VK_OEM_ENLW 0xF4
#define VK_OEM_BACKTAB 0xF5
#define VK_ATTN 0xF6
#define VK_CRSEL 0xF7
#define VK_EXSEL 0xF8
#define VK_EREOF 0xF9
#define VK_PLAY 0xFA
#define VK_ZOOM 0xFB
#define VK_NONAME 0xFC
#define VK_PA1 0xFD
#define VK_OEM_CLEAR 0xFE
#define WM_MOUSEHOVER 0x02A1
#define WM_MOUSELEAVE 0x02A3
#define ICON_SMALL 0
#define ICON_BIG 1
#define ICON_SMALL2 2
#define WVR_ALIGNTOP 0x0010
#define WVR_ALIGNLEFT 0x0020
#define WVR_ALIGNBOTTOM 0x0040
#define WVR_ALIGNRIGHT 0x0080
#define WVR_HREDRAW 0x0100
#define WVR_VREDRAW 0x0200
#define WVR_VALIDRECTS 0x0400
#define MK_LBUTTON 0x0001
#define MK_RBUTTON 0x0002
#define MK_SHIFT 0x0004
#define MK_CONTROL 0x0008
#define MK_MBUTTON 0x0010
#define MK_XBUTTON1 0x0020
#define MK_XBUTTON2 0x0040
#define TME_HOVER 0x00000001
#define TME_LEAVE 0x00000002
#define TME_NONCLIENT 0x00000010
#define TME_QUERY 0x40000000
#define TME_CANCEL 0x80000000
#define HOVER_DEFAULT 0xFFFFFFFF
#define CS_IME 0x00010000
#define CS_DROPSHADOW 0x00020000
#define PM_NOREMOVE 0x0000
#define PM_REMOVE 0x0001
#define PM_NOYIELD 0x0002
#define KEYEVENTF_EXTENDEDKEY 0x0001
#define KEYEVENTF_KEYUP 0x0002
#define KEYEVENTF_UNICODE 0x0004
#define KEYEVENTF_SCANCODE 0x0008
#define MOUSEEVENTF_MOVE 0x0001
#define MOUSEEVENTF_LEFTDOWN 0x0002
#define MOUSEEVENTF_LEFTUP 0x0004
#define MOUSEEVENTF_RIGHTDOWN 0x0008
#define MOUSEEVENTF_RIGHTUP 0x0010
#define MOUSEEVENTF_MIDDLEDOWN 0x0020
#define MOUSEEVENTF_MIDDLEUP 0x0040
#define MOUSEEVENTF_XDOWN 0x0080
#define MOUSEEVENTF_XUP 0x0100
#define MOUSEEVENTF_WHEEL 0x0800
#define MOUSEEVENTF_HWHEEL 0x01000
#define MOUSEEVENTF_MOVE_NOCOALESCE 0x2000
#define MOUSEEVENTF_VIRTUALDESK 0x4000
#define MOUSEEVENTF_ABSOLUTE 0x8000
#define QS_KEY 0x0001
#define QS_MOUSEMOVE 0x0002
#define QS_MOUSEBUTTON 0x0004
#define QS_POSTMESSAGE 0x0008
#define QS_TIMER 0x0010
#define QS_PAINT 0x0020
#define QS_SENDMESSAGE 0x0040
#define QS_HOTKEY 0x0080
#define QS_ALLPOSTMESSAGE 0x0100
#define QS_RAWINPUT 0x0400
#define QS_TOUCH 0x0800
#define QS_POINTER 0x1000
#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXVSCROLL 2
#define SM_CYHSCROLL 3
#define SM_CYCAPTION 4
#define SM_CXBORDER 5
#define SM_CYBORDER 6
#define SM_CXDLGFRAME 7
#define SM_CYDLGFRAME 8
#define SM_CYVTHUMB 9
#define SM_CXHTHUMB 10
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CXCURSOR 13
#define SM_CYCURSOR 14
#define SM_CYMENU 15
#define SM_CXFULLSCREEN 16
#define SM_CYFULLSCREEN 17
#define SM_CYKANJIWINDOW 18
#define SM_MOUSEPRESENT 19
#define SM_CYVSCROLL 20
#define SM_CXHSCROLL 21
#define SM_DEBUG 22
#define SM_SWAPBUTTON 23
#define SM_RESERVED1 24
#define SM_RESERVED2 25
#define SM_RESERVED3 26
#define SM_RESERVED4 27
#define SM_CXMIN 28
#define SM_CYMIN 29
#define SM_CXSIZE 30
#define SM_CYSIZE 31
#define SM_CXFRAME 32
#define SM_CYFRAME 33
#define SM_CXMINTRACK 34
#define SM_CYMINTRACK 35
#define SM_CXDOUBLECLK 36
#define SM_CYDOUBLECLK 37
#define SM_CXICONSPACING 38
#define SM_CYICONSPACING 39
#define SM_MENUDROPALIGNMENT 40
#define SM_PENWINDOWS 41
#define SM_DBCSENABLED 42
#define SM_CMOUSEBUTTONS 43
#define SM_SECURE 44
#define SM_CXEDGE 45
#define SM_CYEDGE 46
#define SM_CXMINSPACING 47
#define SM_CYMINSPACING 48
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CYSMCAPTION 51
#define SM_CXSMSIZE 52
#define SM_CYSMSIZE 53
#define SM_CXMENUSIZE 54
#define SM_CYMENUSIZE 55
#define SM_ARRANGE 56
#define SM_CXMINIMIZED 57
#define SM_CYMINIMIZED 58
#define SM_CXMAXTRACK 59
#define SM_CYMAXTRACK 60
#define SM_CXMAXIMIZED 61
#define SM_CYMAXIMIZED 62
#define SM_NETWORK 63
#define SM_CLEANBOOT 67
#define SM_CXDRAG 68
#define SM_CYDRAG 69
#define SM_SHOWSOUNDS 70
#define SM_CXMENUCHECK 71
#define SM_CYMENUCHECK 72
#define SM_SLOWMACHINE 73
#define SM_MIDEASTENABLED 74
#define SM_MOUSEWHEELPRESENT 75
#define SM_XVIRTUALSCREEN 76
#define SM_YVIRTUALSCREEN 77
#define SM_CXVIRTUALSCREEN 78
#define SM_CYVIRTUALSCREEN 79
#define SM_CMONITORS 80
#define SM_SAMEDISPLAYFORMAT 81
#define SM_IMMENABLED 82
#define SM_CXFOCUSBORDER 83
#define SM_CYFOCUSBORDER 84
#define SM_TABLETPC 86
#define SM_MEDIACENTER 87
#define SM_STARTER 88
#define SM_SERVERR2 89
#define SM_MOUSEHORIZONTALWHEELPRESENT 91
#define SM_CXPADDEDBORDER 92
#define SM_DIGITIZER 94
#define SM_MAXIMUMTOUCHES 95
#define SM_CMETRICS 91
#define SM_REMOTESESSION 0x1000
#define SM_SHUTTINGDOWN 0x2000
#define SM_REMOTECONTROL 0x2001
#define SM_CARETBLINKINGENABLED 0x2002
#define SM_CONVERTIBLESLATEMODE 0x2003
#define SM_SYSTEMDOCKED 0x2004
#define IMAGE_BITMAP 0
#define IMAGE_ICON 1
#define IMAGE_CURSOR 2
#define IMAGE_ENHMETAFILE 3
#define LR_DEFAULTCOLOR 0x0000
#define LR_MONOCHROME 0x0001
#define LR_COLOR 0x0002
#define LR_COPYRETURNORG 0x0004
#define LR_COPYDELETEORG 0x0008
#define LR_LOADFROMFILE 0x0010
#define LR_LOADTRANSPARENT 0x0020
#define LR_DEFAULTSIZE 0x0040
#define LR_VGACOLOR 0x0080
#define LR_LOADMAP3DCOLORS 0x1000
#define LR_CREATEDIBSECTION 0x2000
#define LR_COPYFROMRESOURCE 0x4000
#define LR_SHARED 0x8000
#define DI_MASK 0x0001
#define DI_IMAGE 0x0002
#define DI_NORMAL 0x0003
#define DI_COMPAT 0x0004
#define DI_DEFAULTSIZE 0x0008
#define DI_NOMIRROR 0x0010
#define IDI_HAND 32513
#define IDI_EXCLAMATION 32515
#define IDI_ASTERISK 32516
#define IDTRYAGAIN 10
#define IDCONTINUE 11
#define DWLP_MSGRESULT 0
#define DLGC_WANTARROWS 0x0001
#define DLGC_WANTTAB 0x0002
#define DLGC_WANTALLKEYS 0x0004
#define DLGC_WANTMESSAGE 0x0004
#define DLGC_HASSETSEL 0x0008
#define DLGC_DEFPUSHBUTTON 0x0010
#define DLGC_UNDEFPUSHBUTTON 0x0020
#define DLGC_RADIOBUTTON 0x0040
#define DLGC_WANTCHARS 0x0080
#define DLGC_STATIC 0x0100
#define DLGC_BUTTON 0x2000

/* ------------------------------------------------------------------ S5 hand-written constants */
typedef DWORD *LPDWORD;
typedef DWORD_PTR *PDWORD_PTR;
static const WCHAR aw_wempty[1] = {0}; /* the empty UTF-16 string (L"" is 32-bit on Linux) */
typedef int32_t *LPLONG;
#ifndef MAKELONG
#define MAKELONG(a, b) ((LONG)(((WORD)((uintptr_t)(a) & 0xFFFF)) | ((DWORD)((WORD)((uintptr_t)(b) & 0xFFFF))) << 16))
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xFFFF))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xFFFF))
#define MAKEWPARAM(a, b) ((WPARAM)(MAKELONG(a, b)))
#define MAKELPARAM(a, b) ((LPARAM)(MAKELONG(a, b)))
#define GET_X_LPARAM(l) ((int)(int16_t)LOWORD(l))
#define GET_Y_LPARAM(l) ((int)(int16_t)HIWORD(l))
#define GET_WHEEL_DELTA_WPARAM(w) ((int)(int16_t)HIWORD(w))
#define GET_KEYSTATE_WPARAM(w) (LOWORD(w))
#define GET_XBUTTON_WPARAM(w) (HIWORD(w))
#endif
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(WORD)(i))
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(WORD)(i))

#ifndef RT_CURSOR
#define RT_CURSOR ((LPCSTR)(uintptr_t)1)
#define RT_BITMAP ((LPCSTR)(uintptr_t)2)
#define RT_ICON ((LPCSTR)(uintptr_t)3)
#define RT_GROUP_CURSOR ((LPCSTR)(uintptr_t)12)
#define RT_GROUP_ICON ((LPCSTR)(uintptr_t)14)
#endif
#define AW_SM_MAX 100
#define AW_CMD_MAX 1024

/* ================================================================== S5 internal model
 * user32_*.c share these definitions.  Windows are kept in a fixed table; HWNDs are
 * (gen << 16) | (slot << 2) | 4 so they are never 0 and stale handles are rejected. */
#define AW_MAX_WINDOWS 4096
#define AW_MAX_CLASSES 1024
#define AW_MAX_ATOMS 4096
#define AW_ATOM_BASE 0xC000
#define AW_TIMER_MIN_MS 10
#define AW_DBLCLK_PX 4

/* COLORREF is 0x00BBGGRR; surface pixels are 0x00RRGGBB. */
static inline uint32_t aw_px(COLORREF c)
{
    return ((c & 0xFFu) << 16) | (c & 0xFF00u) | ((c >> 16) & 0xFFu);
}
#define AW_RGB(r, g, b) ((COLORREF)((uint32_t)(r) | ((uint32_t)(g) << 8) | ((uint32_t)(b) << 16)))

typedef struct aw_atom_entry {
    WCHAR *name;
    int used, global;
} aw_atom_entry;

typedef struct aw_wndclass {
    ATOM atom;
    UINT style;
    WNDPROC proc;
    int unicode;
    int cb_cls, cb_wnd;
    HINSTANCE hinst;
    HICON icon, icon_sm;
    HCURSOR cursor;
    HBRUSH brush;
    WCHAR *menu_name;      /* NULL or malloc'd string */
    uintptr_t menu_id;     /* integer resource id when menu_name is NULL and menu_id != 0 */
    WCHAR name[128];
    BYTE *cls_extra;
    int used, builtin;
} aw_wndclass;

struct aw_prop {
    struct aw_prop *next;
    WCHAR *name;
    HANDLE data;
};

typedef struct aw_win {
    HWND hwnd;
    uint16_t gen;
    uint8_t used, is_desktop, is_message;
    DWORD tid;
    aw_wndclass *cls;
    WNDPROC proc;
    int unicode;
    UINT style, exstyle;
    WCHAR *text;
    int text_len, text_cap;
    HMENU menu;
    HINSTANCE hinst;
    INT_PTR id;
    HWND parent, owner;
    HWND first_child, last_child, next_sib, prev_sib;
    RECT rect;             /* top-level: screen coords; child: client coords of its parent */
    RECT client;           /* client area relative to the window's own top-left */
    LONG_PTR userdata;
    int extra_size;
    BYTE *extra;
    struct aw_prop *props;
    aw_region update;      /* invalid area, client coords */
    aw_region clip;        /* clip of the paint DC currently handed out (surface coords) */
    int clip_active;
    HFONT font;
    HICON icon_big, icon_small;
    HCURSOR cursor;
    HWND last_focus;       /* top-level: child that had the focus when the window was last active */
    unsigned visible : 1, enabled : 1, minimized : 1, maximized : 1, active : 1;
    unsigned destroyed : 1, erase_pending : 1, nc_invalid : 1, tracking_leave : 1, tracking_hover : 1;
    unsigned in_paint : 1, shown_once : 1, topmost : 1, has_menu : 1, in_create : 1;
    uint32_t xwin;         /* top-level: X window (0 for children and message windows) */
    aw_surface *surf;      /* top-level: back buffer covering the whole window rect */
    int cur_dpi;
    void *sb;              /* scroll bar state (user32_scroll.c); NULL until first use */
} aw_win;

typedef struct aw_timer {
    struct aw_timer *next;
    HWND hwnd;
    UINT_PTR id;
    UINT elapse;
    DWORD due;
    TIMERPROC proc;
} aw_timer;

typedef struct aw_sendreq {
    struct aw_sendreq *next;
    HWND hwnd;
    UINT msg;
    WPARAM w;
    LPARAM l;
    int unicode;
    LRESULT result;
    int done;
} aw_sendreq;

typedef struct aw_msg {
    struct aw_msg *next;
    HWND hwnd;
    UINT msg;
    WPARAM w;
    LPARAM l;
    DWORD time;
    POINT pt;
    void *owned;           /* string buffer converted to the target charset; freed after GetMessage */
    char key_text[8];      /* KEYDOWN/SYSKEYDOWN: UTF-8 text for TranslateMessage */
} aw_msg;

typedef struct aw_queue {
    struct aw_queue *next;
    DWORD tid;
    aw_msg *head, *tail;
    aw_sendreq *sent;      /* cross-thread SendMessage requests waiting for this thread */
    aw_timer *timers;
    HWND focus, capture, active, hover;
    int hover_x, hover_y;
    DWORD hover_tick;
    MSG last;
    int last_valid;
    char key_text[8];
    int key_sys;
    DWORD last_time;
    POINT last_pt;
    HWND click_hwnd;
    int click_btn, click_x, click_y;
    DWORD click_time;
    UINT_PTR next_timer_id;
    void *owned_last;
    int in_send;
} aw_queue;

/* ------------------------------------------------------------------ cross-file internals */
extern pthread_mutex_t aw_wnd_lock;     /* short data sections only; never held across callbacks */
extern pthread_cond_t aw_send_cond;     /* cross-thread SendMessage completion */
extern int aw_display;                  /* plat_init() succeeded */
extern HWND aw_desktop_hwnd;            /* pseudo window covering the screen */

/* user32_class.c */
ATOM aw_atom_add(const WCHAR *name, int global);
ATOM aw_atom_find(const WCHAR *name);
const WCHAR *aw_atom_name(ATOM a);
aw_wndclass *aw_class_find(const WCHAR *name, ATOM atom);
ATOM aw_class_add(const aw_wndclass *tmpl, const WCHAR *name);
int aw_wcmp_ci(const WCHAR *a, const WCHAR *b);
int aw_wlen(const WCHAR *s);

/* user32_wnd.c */
aw_win *aw_win_slot(int i);
aw_win *aw_win_from_hwnd(HWND h);
aw_win *aw_win_from_xwin(uint32_t xwin);
int aw_hwnd_valid(HWND h);
aw_win *aw_top_ancestor(aw_win *w);
aw_win *aw_win_alloc(void);
void aw_win_free(aw_win *w);
int aw_window_kind(const aw_win *w);
void aw_link_child(aw_win *parent, aw_win *w, HWND after);
void aw_unlink_child(aw_win *w);
void aw_win_set_text(aw_win *w, const WCHAR *s, int len);
void aw_update_x_title(aw_win *w);
void aw_destroy_window_internal(aw_win *w);
void aw_destroy_owned(HWND owner);
void aw_user_init(void);

/* user32_geom.c */
void aw_win_origin(const aw_win *w, int *x, int *y);
void aw_win_client_origin(const aw_win *w, int *x, int *y);
void aw_win_surf_rect(const aw_win *w, RECT *r);
void aw_win_client_surf_rect(const aw_win *w, RECT *r);
void aw_clip_chain(const aw_win *w, aw_region *g);
void aw_visible_region(const aw_win *w, aw_region *out);
int aw_is_visible(const aw_win *w);
BOOL aw_show_window(aw_win *w, int cmd);
void aw_set_pos(aw_win *w, int x, int y, int cx, int cy, UINT flags);
void aw_on_configure(aw_win *w, int x, int y, int cx, int cy);

/* user32_nc.c */
void aw_nc_calc(aw_win *w);
int aw_menu_height(aw_win *w);
int aw_nc_hit(aw_win *w, int x, int y);
void aw_nc_paint(aw_win *w);

/* user32_msg.c */
aw_queue *aw_queue_current(void);
aw_queue *aw_queue_for_tid(DWORD tid);
LRESULT aw_send_msg(HWND h, UINT msg, WPARAM w, LPARAM l, int unicode);
LRESULT aw_call_proc(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, int src_unicode);
BOOL aw_post_input_text(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, DWORD time, int x, int y, const char *text);
#define aw_post_input(w, msg, wp, lp, time, x, y) aw_post_input_text((w), (msg), (wp), (lp), (time), (x), (y), NULL)
LRESULT aw_defwndproc(aw_win *w, UINT msg, WPARAM wp, LPARAM lp, int unicode);
int aw_get_message(aw_queue *q, MSG *m, HWND filter, UINT min, UINT max, int remove, int block);
void aw_timers_kill_for(HWND h);
void aw_wake(void);
int aw_post_thread_msg_q(aw_queue *q, UINT msg, WPARAM wp, LPARAM lp);

/* user32_paint.c */
void aw_invalidate_rgn(aw_win *w, const aw_region *r, int erase);
void aw_invalidate(aw_win *w, const RECT *r, int erase);
int aw_gen_paint(HWND *out);
void aw_paint_now(aw_win *w);
void aw_flush_all(void);
HDC aw_dc_for(aw_win *w, const aw_region *vis, int window_dc);
void aw_erase_bkgnd(aw_win *w, aw_win *top, const aw_region *clip, HBRUSH brush);

/* user32_input.c */
void aw_input_dispatch(aw_plat_event *ev);
void aw_activate_top(aw_win *top, int active);
void aw_capture_set(HWND h);
void aw_hover_idle(void);
void aw_apply_cursor(aw_win *w);
void aw_key_set(int vk, int down, int toggled);

/* user32_icon.c */
aw_icon *aw_icon_alloc(int w, int h, int cursor);
void aw_icon_free(aw_icon *ic);
aw_icon *aw_icon_system(int id, int cursor);
aw_icon *aw_icon_from_resource(HINSTANCE h, const void *name, int cursor, int cx, int cy);
aw_icon *aw_icon_from_file(const char *path, int cursor, int cx, int cy);
aw_icon *aw_icon_from_dib(const uint8_t *d, uint32_t size, int cursor, int wantw, int wanth, int from_group);

/* user32_sys.c */
int aw_system_metric(int idx);

/* cross-package (ms_abi) */
extern COLORREF WINAPI __aw_gdi32_SetTextColor(HDC, COLORREF);
extern COLORREF WINAPI __aw_gdi32_SetBkColor(HDC, COLORREF);
extern HBRUSH WINAPI __aw_gdi32_CreateSolidBrush(COLORREF);
extern HBRUSH WINAPI __aw_user32_GetSysColorBrush(int);
extern HGDIOBJ WINAPI __aw_gdi32_GetStockObject(int);
extern int WINAPI __aw_user32_FillRect(HDC, const RECT *, HBRUSH);

/* S5 helpers shared between user32_*.c */
void aw_style_insets(DWORD style, DWORD exstyle, int menu_h, int *l, int *t, int *r, int *b);
void aw_screen_pos(const aw_win *w, int *x, int *y);
void aw_screen_client_pos(const aw_win *w, int *x, int *y);
int aw_is_alt_down(void);
void aw_atexit_gui(void);
extern HDC WINAPI __aw_user32_BeginPaint(HWND, PAINTSTRUCT *);
extern BOOL WINAPI __aw_user32_EndPaint(HWND, const PAINTSTRUCT *);
extern HDC WINAPI __aw_user32_GetDCEx(HWND, HRGN, DWORD);
extern LRESULT WINAPI __aw_user32_DispatchMessageA(const MSG *);
extern BOOL WINAPI __aw_user32_TranslateMessage(const MSG *);
extern BOOL WINAPI __aw_user32_SetWindowPos(HWND, HWND, int, int, int, int, UINT);
extern LRESULT WINAPI __aw_user32_DefWindowProcA(HWND, UINT, WPARAM, LPARAM);
extern LRESULT WINAPI __aw_user32_DefWindowProcW(HWND, UINT, WPARAM, LPARAM);
extern BOOL WINAPI __aw_user32_DrawIconEx(HDC, int, int, HICON, int, int, UINT, HBRUSH, UINT);

/* ------------------------------------------------------------------ hooks owned by other packages
 * (weak fallbacks live in user32_nc.c; menu.c / dialog.c / controls override them).
 * Tray (S8): `int aw_tray_handle_event(aw_plat_event *ev)` is weak in user32_input.c; return nonzero
 * when the tray consumed the X event, and user32 skips it. */
int aw_menubar_height(HWND hwnd, HMENU menu, int width);
void aw_menubar_paint(HWND hwnd, HMENU menu, HDC window_dc, const RECT *bar);
void aw_menubar_track(HWND hwnd, HMENU menu, int x, int y, int vk);
int aw_menu_is_valid(HMENU menu);
void aw_dialogs_register(void);
void aw_menus_register(void);

/* user32_scroll.c: standard scroll bars (WS_VSCROLL/WS_HSCROLL) */
void aw_scroll_extra(aw_win *w, int *r, int *b);      /* adds the bar thickness to the right/bottom insets */
int aw_scroll_hit(aw_win *w, int x, int y);           /* window coords -> HTHSCROLL/HTVSCROLL or 0 */
void aw_scroll_nc_paint(aw_win *w, const aw_region *vis);
void aw_scroll_track(aw_win *w, int hit, int sx, int sy); /* WM_NCLBUTTONDOWN on a bar: modal tracking */
void aw_scroll_free(aw_win *w);

#endif
