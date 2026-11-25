/* Private header for menu.c / dialog.c: Win32 GUI structures and constants
 * not present in aw.h, plus the cross-package prototypes used by these files.
 * Only included by menu.c and dialog.c. */
#ifndef AWMD_H
#define AWMD_H
#include "aw.h"
#include "gui.h"

#ifndef MAKELONG
#define MAKELONG(a, b) ((LONG)(((WORD)((a) & 0xffff)) | ((DWORD)((WORD)((b) & 0xffff))) << 16))
#endif
#ifndef LOWORD
#define LOWORD(l) ((WORD)((uintptr_t)(l) & 0xffff))
#define HIWORD(l) ((WORD)(((uintptr_t)(l) >> 16) & 0xffff))
#endif
#define MAKEINTRESOURCEW(i) ((LPCWSTR)(uintptr_t)(WORD)(i))
#define MAKEINTRESOURCEA(i) ((LPCSTR)(uintptr_t)(WORD)(i))

/* ---- messages -------------------------------------------------------- */
#define WM_NULL 0x0000
#define WM_CREATE 0x0001
#define WM_DESTROY 0x0002
#define WM_MOVE 0x0003
#define WM_SIZE 0x0005
#define WM_ACTIVATE 0x0006
#define WM_SETFOCUS 0x0007
#define WM_KILLFOCUS 0x0008
#define WM_ENABLE 0x000A
#define WM_SETTEXT 0x000C
#define WM_GETTEXT 0x000D
#define WM_GETTEXTLENGTH 0x000E
#define WM_PAINT 0x000F
#define WM_CLOSE 0x0010
#define WM_QUIT 0x0012
#define WM_ERASEBKGND 0x0014
#define WM_SYSCOLORCHANGE 0x0015
#define WM_SHOWWINDOW 0x0018
#define WM_SETTINGCHANGE 0x001A
#define WM_ACTIVATEAPP 0x001C
#define WM_SETFONT 0x0030
#define WM_GETFONT 0x0031
#define WM_NCCREATE 0x0081
#define WM_NCDESTROY 0x0082
#define WM_GETDLGCODE 0x0087
#define WM_KEYDOWN 0x0100
#define WM_KEYUP 0x0101
#define WM_CHAR 0x0102
#define WM_DEADCHAR 0x0103
#define WM_SYSKEYDOWN 0x0104
#define WM_SYSKEYUP 0x0105
#define WM_SYSCHAR 0x0106
#define WM_INITDIALOG 0x0110
#define WM_COMMAND 0x0111
#define WM_SYSCOMMAND 0x0112
#define WM_TIMER 0x0113
#define WM_INITMENU 0x0116
#define WM_INITMENUPOPUP 0x0117
#define WM_MEASUREITEM 0x011C
#define WM_MENUSELECT 0x011F
#define WM_MENUCHAR 0x0120
#define WM_ENTERIDLE 0x0121
#define WM_UNINITMENUPOPUP 0x0125
#define WM_DRAWITEM 0x012B
#define WM_CTLCOLORMSGBOX 0x0132
#define WM_CTLCOLOREDIT 0x0133
#define WM_CTLCOLORLISTBOX 0x0134
#define WM_CTLCOLORBTN 0x0135
#define WM_CTLCOLORDLG 0x0136
#define WM_CTLCOLORSCROLLBAR 0x0137
#define WM_CTLCOLORSTATIC 0x0138
#define WM_MOUSEMOVE 0x0200
#define WM_LBUTTONDOWN 0x0201
#define WM_LBUTTONUP 0x0202
#define WM_LBUTTONDBLCLK 0x0203
#define WM_RBUTTONDOWN 0x0204
#define WM_RBUTTONUP 0x0205
#define WM_RBUTTONDBLCLK 0x0206
#define WM_MBUTTONDOWN 0x0207
#define WM_MBUTTONUP 0x0208
#define WM_MOUSEWHEEL 0x020A
#define WM_MOUSEHWHEEL 0x020E
#define WM_ENTERMENULOOP 0x0211
#define WM_EXITMENULOOP 0x0212
#define WM_NEXTMENU 0x0213
#define WM_SYSCOMMAND 0x0112
#define WM_USER 0x0400

/* ---- menu ------------------------------------------------------------ */
#define MF_INSERT 0x00000000
#define MF_CHANGE 0x00000080
#define MF_APPEND 0x00000100
#define MF_DELETE 0x00000200
#define MF_REMOVE 0x00001000
#define MF_BYCOMMAND 0x00000000
#define MF_BYPOSITION 0x00000400
#define MF_SEPARATOR 0x00000800
#define MF_ENABLED 0x00000000
#define MF_GRAYED 0x00000001
#define MF_DISABLED 0x00000002
#define MF_UNCHECKED 0x00000000
#define MF_CHECKED 0x00000008
#define MF_USECHECKBITMAPS 0x00000200
#define MF_STRING 0x00000000
#define MF_BITMAP 0x00000004
#define MF_OWNERDRAW 0x00000100
#define MF_POPUP 0x00000010
#define MF_MENUBARBREAK 0x00000020
#define MF_MENUBREAK 0x00000040
#define MF_UNHILITE 0x00000000
#define MF_HILITE 0x00000080
#define MF_DEFAULT 0x00001000
#define MF_SYSMENU 0x00002000
#define MF_HELP 0x00004000
#define MF_RIGHTJUSTIFY 0x00004000
#define MF_MOUSESELECT 0x00008000
#define MF_END 0x00000080

#define MFT_STRING 0x00000000
#define MFT_BITMAP 0x00000004
#define MFT_MENUBARBREAK 0x00000020
#define MFT_MENUBREAK 0x00000040
#define MFT_OWNERDRAW 0x00000100
#define MFT_RADIOCHECK 0x00000200
#define MFT_SEPARATOR 0x00000800
#define MFT_RIGHTORDER 0x00002000
#define MFT_RIGHTJUSTIFY 0x00004000

#define MFS_GRAYED 0x00000003
#define MFS_DISABLED 0x00000003
#define MFS_CHECKED 0x00000008
#define MFS_HILITE 0x00000080
#define MFS_ENABLED 0x00000000
#define MFS_UNCHECKED 0x00000000
#define MFS_UNHILITE 0x00000000
#define MFS_DEFAULT 0x00001000

#define MIIM_STATE 0x00000001
#define MIIM_ID 0x00000002
#define MIIM_SUBMENU 0x00000004
#define MIIM_CHECKMARKS 0x00000008
#define MIIM_TYPE 0x00000010
#define MIIM_DATA 0x00000020
#define MIIM_STRING 0x00000040
#define MIIM_BITMAP 0x00000080
#define MIIM_FTYPE 0x00000100

#define MIM_MAXHEIGHT 0x00000001
#define MIM_BACKGROUND 0x00000002
#define MIM_HELPID 0x00000004
#define MIM_MENUDATA 0x00000008
#define MIM_STYLE 0x00000010
#define MIM_APPLYTOSUBMENUS 0x80000000

#define MNS_NOCHECK 0x80000000
#define MNS_MODELESS 0x40000000
#define MNS_DRAGDROP 0x20000000
#define MNS_AUTODISMISS 0x10000000
#define MNS_NOTIFYBYPOS 0x08000000
#define MNS_CHECKORBMP 0x04000000

#define TPM_LEFTBUTTON 0x0000
#define TPM_RIGHTBUTTON 0x0002
#define TPM_LEFTALIGN 0x0000
#define TPM_CENTERALIGN 0x0004
#define TPM_RIGHTALIGN 0x0008
#define TPM_TOPALIGN 0x0000
#define TPM_VCENTERALIGN 0x0010
#define TPM_BOTTOMALIGN 0x0020
#define TPM_HORIZONTAL 0x0000
#define TPM_VERTICAL 0x0040
#define TPM_NONOTIFY 0x0080
#define TPM_RETURNCMD 0x0100
#define TPM_RECURSE 0x0001
#define TPM_NOANIMATION 0x4000
#define TPM_LAYOUTRTL 0x8000
#define TPM_WORKAREA 0x10000

#define GMDI_USEDISABLED 0x0001
#define GMDI_GOINTOPOPUPS 0x0002

#define MNC_IGNORE 0
#define MNC_CLOSE 1
#define MNC_EXECUTE 2
#define MNC_SELECT 3

#define HBMMENU_CALLBACK ((HBITMAP)(intptr_t)-1)
#define HBMMENU_SYSTEM ((HBITMAP)(intptr_t)1)
#define HBMMENU_MBAR_RESTORE ((HBITMAP)(intptr_t)2)
#define HBMMENU_MBAR_MINIMIZE ((HBITMAP)(intptr_t)3)
#define HBMMENU_MBAR_CLOSE ((HBITMAP)(intptr_t)5)
#define HBMMENU_MBAR_CLOSE_D ((HBITMAP)(intptr_t)6)
#define HBMMENU_MBAR_MINIMIZE_D ((HBITMAP)(intptr_t)7)
#define HBMMENU_POPUP_CLOSE ((HBITMAP)(intptr_t)8)
#define HBMMENU_POPUP_RESTORE ((HBITMAP)(intptr_t)9)
#define HBMMENU_POPUP_MAXIMIZE ((HBITMAP)(intptr_t)10)
#define HBMMENU_POPUP_MINIMIZE ((HBITMAP)(intptr_t)11)

#define OBJID_WINDOW 0x00000000
#define OBJID_SYSMENU 0xFFFFFFFF
#define OBJID_MENU 0xFFFFFFFD
#define OBJID_CLIENT 0xFFFFFFFC

typedef struct {
    UINT cbSize;
    UINT fMask;
    UINT fType;
    UINT fState;
    UINT wID;
    HMENU hSubMenu;
    HBITMAP hbmpChecked;
    HBITMAP hbmpUnchecked;
    ULONG_PTR dwItemData;
    LPWSTR dwTypeDataW;
    UINT cch;
    HBITMAP hbmpItem;
} AW_MENUITEMINFOW;

typedef struct {
    UINT cbSize;
    UINT fMask;
    UINT fType;
    UINT fState;
    UINT wID;
    HMENU hSubMenu;
    HBITMAP hbmpChecked;
    HBITMAP hbmpUnchecked;
    ULONG_PTR dwItemData;
    LPSTR dwTypeDataA;
    UINT cch;
    HBITMAP hbmpItem;
} AW_MENUITEMINFOA;

typedef struct {
    DWORD cbSize;
    DWORD fMask;
    DWORD dwStyle;
    UINT cyMax;
    HBRUSH hbrBack;
    DWORD dwContextHelpID;
    ULONG_PTR dwMenuData;
} AW_MENUINFO;

typedef struct {
    DWORD cbSize;
    RECT rcBar;
    HMENU hMenu;
    HWND hwndMenu;
    BOOL fBarFocused;
    BOOL fFocused;
} AW_MENUBARINFO;

typedef struct {
    UINT cbSize;
    RECT rcExclude;
} AW_TPMPARAMS;

typedef struct { BYTE fVirt; WORD key; WORD cmd; } AW_ACCEL;
#define FVIRTKEY 0x01
#define FNOINVERT 0x02
#define FSHIFT 0x04
#define FCONTROL 0x08
#define FALT 0x10

/* ---- dialog styles --------------------------------------------------- */
#define DS_ABSALIGN 0x00000001L
#define DS_SYSMODAL 0x00000002L
#define DS_3DLOOK 0x00000004L
#define DS_FIXEDSYS 0x00000008L
#define DS_NOFAILCREATE 0x00000010L
#define DS_LOCALEDIT 0x00000020L
#define DS_SETFONT 0x00000040L
#define DS_MODALFRAME 0x00000080L
#define DS_NOIDLEMSG 0x00000100L
#define DS_SETFOREGROUND 0x00000200L
#define DS_CONTROL 0x00000400L
#define DS_CENTER 0x00000800L
#define DS_CENTERMOUSE 0x00001000L
#define DS_CONTEXTHELP 0x00002000L
#define DS_SHELLFONT (DS_SETFONT | DS_FIXEDSYS)

#define WS_OVERLAPPED 0
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
#define WS_EX_LEFT 0
#define WS_EX_RTLREADING 0x00002000L
#define WS_EX_CONTROLPARENT 0x00010000L
#define WS_EX_APPWINDOW 0x00040000L
#define WS_EX_LAYERED 0x00080000L

#define BS_PUSHBUTTON 0x00000000L
#define BS_DEFPUSHBUTTON 0x00000001L
#define BS_CHECKBOX 0x00000002L
#define BS_AUTOCHECKBOX 0x00000003L
#define BS_RADIOBUTTON 0x00000004L
#define BS_3STATE 0x00000005L
#define BS_AUTO3STATE 0x00000006L
#define BS_GROUPBOX 0x00000007L
#define BS_USERBUTTON 0x00000008L
#define BS_AUTORADIOBUTTON 0x00000009L
#define BS_PUSHBOX 0x0000000AL
#define BS_OWNERDRAW 0x0000000BL
#define BS_TYPEMASK 0x0000000FL
#define BS_LEFTTEXT 0x00000020L
#define BS_TEXT 0x00000000L
#define BS_ICON 0x00000040L
#define BS_BITMAP 0x00000080L
#define BS_LEFT 0x00000100L
#define BS_RIGHT 0x00000200L
#define BS_CENTER 0x00000300L
#define BS_TOP 0x00000400L
#define BS_BOTTOM 0x00000800L
#define BS_VCENTER 0x00000C00L
#define BS_PUSHLIKE 0x00001000L
#define BS_MULTILINE 0x00002000L
#define BS_NOTIFY 0x00004000L
#define BS_FLAT 0x00008000L

#define ES_LEFT 0x0000L
#define ES_CENTER 0x0001L
#define ES_RIGHT 0x0002L
#define ES_MULTILINE 0x0004L
#define ES_UPPERCASE 0x0008L
#define ES_LOWERCASE 0x0010L
#define ES_PASSWORD 0x0020L
#define ES_AUTOVSCROLL 0x0040L
#define ES_AUTOHSCROLL 0x0080L
#define ES_NOHIDESEL 0x0100L
#define ES_OEMCONVERT 0x0400L
#define ES_READONLY 0x0800L
#define ES_WANTRETURN 0x1000L
#define ES_NUMBER 0x2000L

#define BM_GETCHECK 0x00F0
#define BM_SETCHECK 0x00F1
#define BM_GETSTATE 0x00F2
#define BM_SETSTATE 0x00F3
#define BM_SETSTYLE 0x00F4
#define BM_CLICK 0x00F5
#define BM_GETIMAGE 0x00F6
#define BM_SETIMAGE 0x00F7
#define BM_SETDONTCLICK 0x00F8

#define DM_GETDEFID (WM_USER + 0)
#define DM_SETDEFID (WM_USER + 1)
#define DM_REPOSITION (WM_USER + 2)
#define DC_HASDEFID 0x534B
#define DLGWINDOWEXTRA 30
#define DWLP_MSGRESULT 0
#define DWLP_DLGPROC 8
#define DWLP_USER 16
/* our own bytes inside DLGWINDOWEXTRA */
#define AWDLP_DEFID 24
#define AWDLP_BASEX 26
#define AWDLP_BASEY 28

/* ---- dialog templates (raw field offsets are parsed by hand) --------- */
typedef struct {
    DWORD style;
    DWORD dwExtendedStyle;
    WORD cdit;
    short x, y, cx, cy;
} AW_DLGTEMPLATE;
typedef struct {
    WORD dlgVer;
    WORD signature;
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    WORD cDlgItems;
    short x, y, cx, cy;
} AW_DLGTEMPLATEEX;
typedef struct {
    DWORD style;
    DWORD dwExtendedStyle;
    short x, y, cx, cy;
    WORD id;
} AW_DLGITEMTEMPLATE;
typedef struct {
    DWORD helpID;
    DWORD exStyle;
    DWORD style;
    short x, y, cx, cy;
    DWORD id;
} AW_DLGITEMTEMPLATEEX;

/* ---- misc constants ------------------------------------------------- */
#define IDOK 1
#define IDCANCEL 2
#define IDABORT 3
#define IDRETRY 4
#define IDIGNORE 5
#define IDYES 6
#define IDNO 7
#define IDCLOSE 8
#define IDHELP 9
#define IDTRYAGAIN 10
#define IDCONTINUE 11
#define IDTIMEOUT 32000

#define MB_OK 0x00000000L
#define MB_OKCANCEL 0x00000001L
#define MB_ABORTRETRYIGNORE 0x00000002L
#define MB_YESNOCANCEL 0x00000003L
#define MB_YESNO 0x00000004L
#define MB_RETRYCANCEL 0x00000005L
#define MB_CANCELTRYCONTINUE 0x00000006L
#define MB_ICONHAND 0x00000010L
#define MB_ICONQUESTION 0x00000020L
#define MB_ICONEXCLAMATION 0x00000030L
#define MB_ICONASTERISK 0x00000040L
#define MB_USERICON 0x00000080L
#define MB_ICONWARNING MB_ICONEXCLAMATION
#define MB_ICONERROR MB_ICONHAND
#define MB_ICONINFORMATION MB_ICONASTERISK
#define MB_ICONSTOP MB_ICONHAND
#define MB_DEFBUTTON1 0x00000000L
#define MB_DEFBUTTON2 0x00000100L
#define MB_DEFBUTTON3 0x00000200L
#define MB_DEFBUTTON4 0x00000300L
#define MB_APPLMODAL 0x00000000L
#define MB_SYSTEMMODAL 0x00001000L
#define MB_TASKMODAL 0x00002000L
#define MB_HELP 0x00004000L
#define MB_NOFOCUS 0x00008000L
#define MB_SETFOREGROUND 0x00010000L
#define MB_DEFAULT_DESKTOP_ONLY 0x00020000L
#define MB_TOPMOST 0x00040000L
#define MB_RIGHT 0x00080000L
#define MB_RTLREADING 0x00100000L
#define MB_SERVICE_NOTIFICATION 0x00200000L
#define MB_SERVICE_NOTIFICATION_NT3X 0x00040000L
#define MB_TYPEMASK 0x0000000FL
#define MB_ICONMASK 0x000000F0L
#define MB_DEFMASK 0x00000F00L
#define MB_MODEMASK 0x00003000L
#define MB_MISCMASK 0x0000C000L

#define IDI_APPLICATION 32512
#define IDI_HAND 32513
#define IDI_QUESTION 32514
#define IDI_EXCLAMATION 32515
#define IDI_ASTERISK 32516
#define IDI_WINLOGO 32517
#define IDI_WARNING IDI_EXCLAMATION
#define IDI_ERROR IDI_HAND
#define IDI_INFORMATION IDI_ASTERISK

#define SW_HIDE 0
#define SW_SHOWNORMAL 1
#define SW_SHOWMINIMIZED 2
#define SW_SHOWMAXIMIZED 3
#define SW_SHOWNOACTIVATE 4
#define SW_SHOW 5
#define SW_SHOWNA 8

#define SM_CXSCREEN 0
#define SM_CYSCREEN 1
#define SM_CXICON 11
#define SM_CYICON 12
#define SM_CYMENU 15
#define SM_CXDOUBLECLK 36
#define SM_CXSMICON 49
#define SM_CYSMICON 50
#define SM_CXMENUCHECK 71
#define SM_CYMENUCHECK 72

#define COLOR_SCROLLBAR 0
#define COLOR_BACKGROUND 1
#define COLOR_DESKTOP 1
#define COLOR_ACTIVECAPTION 2
#define COLOR_MENU 4
#define COLOR_WINDOW 5
#define COLOR_WINDOWFRAME 6
#define COLOR_MENUTEXT 7
#define COLOR_WINDOWTEXT 8
#define COLOR_GRAYTEXT 17
#define COLOR_HIGHLIGHT 13
#define COLOR_HIGHLIGHTTEXT 14
#define COLOR_3DFACE 15
#define COLOR_BTNFACE 15
#define COLOR_3DSHADOW 16
#define COLOR_3DDKSHADOW 21
#define COLOR_3DLIGHT 22

#define WHITE_BRUSH 0
#define LTGRAY_BRUSH 1
#define GRAY_BRUSH 2
#define DKGRAY_BRUSH 3
#define BLACK_BRUSH 4
#define NULL_BRUSH 5
#define HOLLOW_BRUSH 5
#define WHITE_PEN 6
#define BLACK_PEN 7
#define NULL_PEN 8
#define DC_BRUSH 18
#define DC_PEN 19
#define DEFAULT_GUI_FONT 17

#define DT_TOP 0x0000
#define DT_LEFT 0x0000
#define DT_CENTER 0x0001
#define DT_RIGHT 0x0002
#define DT_VCENTER 0x0004
#define DT_BOTTOM 0x0008
#define DT_WORDBREAK 0x0010
#define DT_SINGLELINE 0x0020
#define DT_EXPANDTABS 0x0040
#define DT_TABSTOP 0x0080
#define DT_NOCLIP 0x0100
#define DT_CALCRECT 0x0400
#define DT_NOPREFIX 0x0800
#define DT_END_ELLIPSIS 0x8000
#define DT_EDITCONTROL 0x2000
#define DT_NOFULLWIDTHCHARBREAK 0x00080000

#define FW_NORMAL 400
#define FW_BOLD 700
#define DEFAULT_CHARSET 1
#define CLEARTYPE_QUALITY 5
#define OUT_TT_PRECIS 4
#define CLIP_DEFAULT_PRECIS 0
#define DEFAULT_PITCH 0
#define FF_DONTCARE 0

#define GWL_WNDPROC (-4)
#define GWL_STYLE (-16)
#define GWL_EXSTYLE (-20)
#define GWLP_WNDPROC (-4)
#define GWLP_HINSTANCE (-6)
#define GWLP_ID (-12)
#define GWLP_USERDATA (-21)
#define GWLP_HWNDPARENT (-8)
#define GW_CHILD 5
#define GW_HWNDNEXT 2
#define GW_HWNDPREV 3
#define GW_OWNER 4
#define GW_HWNDFIRST 0
#define GW_ENABLEDPOPUP 6

#define VK_LBUTTON 0x01
#define VK_RBUTTON 0x02
#define VK_CANCEL 0x03
#define VK_BACK 0x08
#define VK_TAB 0x09
#define VK_RETURN 0x0D
#define VK_SHIFT 0x10
#define VK_CONTROL 0x11
#define VK_MENU 0x12
#define VK_PAUSE 0x13
#define VK_CAPITAL 0x14
#define VK_ESCAPE 0x1B
#define VK_SPACE 0x20
#define VK_PRIOR 0x21
#define VK_NEXT 0x22
#define VK_END 0x23
#define VK_HOME 0x24
#define VK_LEFT 0x25
#define VK_UP 0x26
#define VK_RIGHT 0x27
#define VK_DOWN 0x28
#define VK_INSERT 0x2D
#define VK_DELETE 0x2E
#define VK_F1 0x70
#define VK_F4 0x73
#define VK_F10 0x79
#define VK_F24 0x87

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

#define MSGF_DIALOGBOX 0
#define MSGF_MENU 2

#define RT_MENU 4
#define RT_ACCELERATOR 9

#define SPI_GETNONCLIENTMETRICS 0x0029

/* ---- structures ------------------------------------------------------ */
typedef struct {
    HWND hwnd;
    UINT message;
    WPARAM wParam;
    LPARAM lParam;
    DWORD time;
    POINT pt;
    DWORD lPrivate;
} AW_MSG;

typedef struct {
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
} AW_WNDCLASSEXW;

typedef struct {
    HDC hdc;
    BOOL fErase;
    RECT rcPaint;
    BOOL fRestore;
    BOOL fIncUpdate;
    BYTE rgbReserved[32];
} AW_PAINTSTRUCT;

typedef struct {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    UINT itemWidth;
    UINT itemHeight;
    ULONG_PTR itemData;
} AW_MEASUREITEMSTRUCT;

typedef struct {
    UINT CtlType;
    UINT CtlID;
    UINT itemID;
    UINT itemAction;
    UINT itemState;
    HWND hwndItem;
    HDC hDC;
    RECT rcItem;
    ULONG_PTR itemData;
} AW_DRAWITEMSTRUCT;

#define ODT_MENU 1
#define ODA_SELECT 0x0001
#define ODA_DRAWENTIRE 0x0001
#define ODS_SELECTED 0x0001
#define ODS_GRAYED 0x0002
#define ODS_DISABLED 0x0004
#define ODS_CHECKED 0x0008
#define ODS_FOCUS 0x0010
#define ODS_DEFAULT 0x0020
#define ODS_COMBOBOXEDIT 0x1000
#define ODS_HOTLIGHT 0x0040
#define ODS_INACTIVE 0x0080
#define ODS_NOACCEL 0x0100
#define ODS_NOFOCUSRECT 0x0200

typedef struct {
    UINT cbSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCWSTR lpszText;
    LPCWSTR lpszCaption;
    DWORD dwStyle;
    LPCWSTR lpszIcon;
    DWORD_PTR dwContextHelpId;
    void(AWCALL *lpfnMsgBoxCallback)(void *);
    DWORD dwLanguageId;
} AW_MSGBOXPARAMSW;

typedef struct {
    UINT cbSize;
    HWND hwndOwner;
    HINSTANCE hInstance;
    LPCSTR lpszText;
    LPCSTR lpszCaption;
    DWORD dwStyle;
    LPCSTR lpszIcon;
    DWORD_PTR dwContextHelpId;
    void(AWCALL *lpfnMsgBoxCallback)(void *);
    DWORD dwLanguageId;
} AW_MSGBOXPARAMSA;

/* ---- cross-package prototypes (ms_abi public API) -------------------- */
#define UD(ret, name, args) extern ret WINAPI __aw_user32_##name args
UD(HWND, CreateWindowExW, (DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID));
UD(HWND, CreateWindowExA, (DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID));
UD(BOOL, DestroyWindow, (HWND));
UD(BOOL, ShowWindow, (HWND, int));
UD(BOOL, EnableWindow, (HWND, BOOL));
UD(BOOL, IsWindow, (HWND));
UD(BOOL, IsWindowVisible, (HWND));
UD(BOOL, IsWindowEnabled, (HWND));
UD(BOOL, IsChild, (HWND, HWND));
UD(HWND, GetParent, (HWND));
UD(HWND, GetWindow, (HWND, UINT));
UD(HWND, GetFocus, (void));
UD(HWND, SetFocus, (HWND));
UD(HWND, GetCapture, (void));
UD(HWND, SetCapture, (HWND));
UD(BOOL, ReleaseCapture, (void));
UD(HWND, GetForegroundWindow, (void));
UD(BOOL, SetForegroundWindow, (HWND));
UD(HWND, GetActiveWindow, (void));
UD(HWND, GetDlgItem, (HWND, int));
UD(BOOL, GetClientRect, (HWND, RECT *));
UD(BOOL, GetWindowRect, (HWND, RECT *));
UD(int, MapWindowPoints, (HWND, HWND, POINT *, UINT));
UD(BOOL, ClientToScreen, (HWND, POINT *));
UD(BOOL, ScreenToClient, (HWND, POINT *));
UD(BOOL, GetCursorPos, (POINT *));
UD(LONG_PTR, GetWindowLongPtrW, (HWND, int));
UD(LONG_PTR, SetWindowLongPtrW, (HWND, int, LONG_PTR));
UD(LONG_PTR, GetWindowLongPtrA, (HWND, int));
UD(LONG_PTR, SetWindowLongPtrA, (HWND, int, LONG_PTR));
UD(LONG, GetWindowLongW, (HWND, int));
UD(LONG, SetWindowLongW, (HWND, int, LONG));
UD(BOOL, SetWindowTextW, (HWND, LPCWSTR));
UD(int, GetWindowTextW, (HWND, LPWSTR, int));
UD(int, GetWindowTextLengthW, (HWND));
UD(int, GetClassNameW, (HWND, LPWSTR, int));
UD(LRESULT, SendMessageW, (HWND, UINT, WPARAM, LPARAM));
UD(LRESULT, SendMessageA, (HWND, UINT, WPARAM, LPARAM));
UD(BOOL, PostMessageW, (HWND, UINT, WPARAM, LPARAM));
UD(BOOL, PostMessageA, (HWND, UINT, WPARAM, LPARAM));
UD(BOOL, GetMessageW, (AW_MSG *, HWND, UINT, UINT));
UD(BOOL, PeekMessageW, (AW_MSG *, HWND, UINT, UINT, UINT));
UD(LRESULT, DispatchMessageW, (const AW_MSG *));
UD(BOOL, TranslateMessage, (const AW_MSG *));
UD(LRESULT, DefWindowProcW, (HWND, UINT, WPARAM, LPARAM));
UD(LRESULT, DefWindowProcA, (HWND, UINT, WPARAM, LPARAM));
UD(BOOL, InvalidateRect, (HWND, const RECT *, BOOL));
UD(BOOL, UpdateWindow, (HWND));
UD(BOOL, RedrawWindow, (HWND, const RECT *, void *, UINT));
UD(HDC, BeginPaint, (HWND, AW_PAINTSTRUCT *));
UD(BOOL, EndPaint, (HWND, const AW_PAINTSTRUCT *));
UD(HDC, GetWindowDC, (HWND));
UD(int, ReleaseDC, (HWND, HDC));
UD(ATOM, RegisterClassExW, (const AW_WNDCLASSEXW *));
UD(ATOM, RegisterClassW, (const void *));
UD(int, GetSystemMetrics, (int));
UD(HICON, LoadIconW, (HINSTANCE, LPCWSTR));
UD(HICON, LoadIconA, (HINSTANCE, LPCSTR));
UD(HCURSOR, LoadCursorW, (HINSTANCE, LPCWSTR));
UD(UINT_PTR, SetTimer, (HWND, UINT_PTR, UINT, void *));
UD(BOOL, KillTimer, (HWND, UINT_PTR));
UD(int, LoadStringW, (HINSTANCE, UINT, LPWSTR, int));
UD(int, LoadStringA, (HINSTANCE, UINT, LPSTR, int));
UD(HMENU, GetMenu, (HWND));
UD(BOOL, SetMenu, (HWND, HMENU));
UD(HWND, WindowFromPoint, (POINT));
UD(BOOL, GetWindowPlacement, (HWND, void *));
UD(void, PostQuitMessage, (int));
UD(SHORT, GetKeyState, (int));
UD(BOOL, IsDialogMessageW, (HWND, AW_MSG *));
UD(BOOL, IsDialogMessageA, (HWND, AW_MSG *));
#define RDW_INVALIDATE 0x0001
#define RDW_ERASE 0x0004
#define RDW_FRAME 0x0400
#define RDW_ALLCHILDREN 0x0080
#define RDW_UPDATENOW 0x0100
#define PM_NOREMOVE 0
#define PM_REMOVE 1
#define PM_NOYIELD 2

#define UG(ret, name, args) extern ret WINAPI __aw_gdi32_##name args
UG(HBRUSH, CreateSolidBrush, (COLORREF));
UG(HPEN, CreatePen, (int, int, COLORREF));
UG(BOOL, DeleteObject, (HGDIOBJ));
UG(HGDIOBJ, GetStockObject, (int));
UG(HGDIOBJ, SelectObject, (HDC, HGDIOBJ));
UG(COLORREF, SetTextColor, (HDC, COLORREF));
UG(COLORREF, SetBkColor, (HDC, COLORREF));
UG(int, SetBkMode, (HDC, int));
UG(BOOL, TextOutW, (HDC, int, int, LPCWSTR, int));
UG(BOOL, GetTextExtentPoint32W, (HDC, LPCWSTR, int, SIZE *));
UG(BOOL, GetTextMetricsW, (HDC, void *));
UG(BOOL, Rectangle, (HDC, int, int, int, int));
UG(BOOL, MoveToEx, (HDC, int, int, POINT *));
UG(BOOL, LineTo, (HDC, int, int, int));
UG(BOOL, Polygon, (HDC, const POINT *, int));
UG(int, FillRect, (HDC, const RECT *, HBRUSH));
UG(int, DrawTextW, (HDC, LPCWSTR, int, RECT *, UINT));
UG(int, DrawTextA, (HDC, LPCSTR, int, RECT *, UINT));
UG(BOOL, FrameRect, (HDC, const RECT *, HBRUSH));
#define TRANSPARENT 1
#define OPAQUE 2
#define PS_SOLID 0
#define PS_INSIDEFRAME 6

/* Menu internals shared between menu.c and dialog.c */
HMENU WINAPI aw_menu_load_indirect_w(const void *templ);
int aw_menubar_hit(HWND hwnd, HMENU menu, int x, int y);

#endif
