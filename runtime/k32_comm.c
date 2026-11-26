/* kernel32: serial port control (DCB and break), over termios. A COM port is an H_FILE on a tty;
 * anything else answers ERROR_INVALID_HANDLE as Windows does for non-serial handles. Timeouts are
 * accepted without effect: reads on a serial H_FILE block until data arrives. */
#include "aw.h"
#include "k32.h"
#include <errno.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#define ERR_INVALID_HANDLE 6
#define ERR_INVALID_PARAMETER 87

/* DCB, x64 layout as Windows defines it */
typedef struct {
    DWORD DCBlength, BaudRate;
    DWORD flags; /* fBinary bit 0, fParity bit 1, fOutxCtsFlow 2, fOutxDsrFlow 3, fDtrControl 4-5 ... */
    WORD wReserved, XonLim, XoffLim;
    BYTE ByteSize, Parity, StopBits;
    char XonChar, XoffChar, ErrorChar, EofChar, EvtChar;
    WORD wReserved1;
} DCB_L;

typedef struct {
    DWORD ReadIntervalTimeout, ReadTotalTimeoutMultiplier, ReadTotalTimeoutConstant;
    DWORD WriteTotalTimeoutMultiplier, WriteTotalTimeoutConstant;
} COMMTIMEOUTS_L;

#define DCB_FBINARY 0x1u
#define DCB_FPARITY 0x2u

static const struct {
    DWORD baud;
    speed_t speed;
} speeds[] = {
    {110, B110},     {300, B300},     {600, B600},       {1200, B1200},   {2400, B2400},
    {4800, B4800},   {9600, B9600},   {19200, B19200},   {38400, B38400}, {57600, B57600},
    {115200, B115200},
};

static int tty_fd(HANDLE h)
{
    int fd = aw_handle_fd(h);
    if (fd < 0 || !isatty(fd)) {
        aw_set_last_error(ERR_INVALID_HANDLE);
        return -1;
    }
    return fd;
}

API(kernel32, BOOL, GetCommState, (HANDLE h, DCB_L *dcb))
{
    int fd = tty_fd(h);
    if (fd < 0)
        return FALSE;
    if (!dcb || dcb->DCBlength < sizeof *dcb) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        aw_set_last_error(aw_errno_to_win(errno));
        return FALSE;
    }
    DWORD baud = 0;
    speed_t sp = cfgetospeed(&t);
    for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++)
        if (speeds[i].speed == sp)
            baud = speeds[i].baud;
    dcb->BaudRate = baud;
    dcb->flags = DCB_FBINARY | ((t.c_cflag & PARENB) ? DCB_FPARITY : 0);
    dcb->ByteSize = (BYTE)(((t.c_cflag & CSIZE) == CS5 ? 5 : (t.c_cflag & CSIZE) == CS6 ? 6
                            : (t.c_cflag & CSIZE) == CS7 ? 7 : 8));
    dcb->StopBits = (t.c_cflag & CSTOPB) ? 2 : 0;       /* TWOSTOPBITS : ONESTOPBIT */
    dcb->Parity = (BYTE)((t.c_cflag & PARENB) ? ((t.c_cflag & PARODD) ? 1 : 2) : 0);
    dcb->XonChar = 0x11;
    dcb->XoffChar = 0x13;
    return TRUE;
}

API(kernel32, BOOL, SetCommState, (HANDLE h, DCB_L *dcb))
{
    int fd = tty_fd(h);
    if (fd < 0)
        return FALSE;
    if (!dcb || dcb->DCBlength < sizeof *dcb) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    struct termios t;
    if (tcgetattr(fd, &t) != 0) {
        aw_set_last_error(aw_errno_to_win(errno));
        return FALSE;
    }
    if (dcb->flags & DCB_FBINARY)
        cfmakeraw(&t); /* binary mode: no translation or echo, as on Windows */
    if (dcb->BaudRate) {
        int found = 0;
        for (size_t i = 0; i < sizeof speeds / sizeof speeds[0]; i++)
            if (speeds[i].baud == dcb->BaudRate) {
                cfsetispeed(&t, speeds[i].speed);
                cfsetospeed(&t, speeds[i].speed);
                found = 1;
            }
        if (!found) {
            aw_set_last_error(ERR_INVALID_PARAMETER);
            return FALSE;
        }
    }
    tcflag_t csize = dcb->ByteSize == 5 ? CS5 : dcb->ByteSize == 6 ? CS6 : dcb->ByteSize == 7 ? CS7 : CS8;
    t.c_cflag = (t.c_cflag & ~(CSIZE | CSTOPB | PARENB | PARODD)) | csize;
    if (dcb->StopBits == 2)
        t.c_cflag |= CSTOPB;
    if (dcb->flags & DCB_FPARITY) {
        t.c_cflag |= PARENB;
        if (dcb->Parity == 1)
            t.c_cflag |= PARODD;
    }
    if (tcsetattr(fd, TCSANOW, &t) != 0) {
        aw_set_last_error(aw_errno_to_win(errno));
        return FALSE;
    }
    return TRUE;
}

API(kernel32, BOOL, SetCommTimeouts, (HANDLE h, COMMTIMEOUTS_L *to))
{
    if (tty_fd(h) < 0)
        return FALSE;
    if (!to) {
        aw_set_last_error(ERR_INVALID_PARAMETER);
        return FALSE;
    }
    return TRUE;
}

API(kernel32, BOOL, SetCommBreak, (HANDLE h))
{
    int fd = tty_fd(h);
    if (fd < 0 || ioctl(fd, TIOCSBRK, 0) != 0) {
        if (fd >= 0)
            aw_set_last_error(aw_errno_to_win(errno));
        return FALSE;
    }
    return TRUE;
}

API(kernel32, BOOL, ClearCommBreak, (HANDLE h))
{
    int fd = tty_fd(h);
    if (fd < 0 || ioctl(fd, TIOCCBRK, 0) != 0) {
        if (fd >= 0)
            aw_set_last_error(aw_errno_to_win(errno));
        return FALSE;
    }
    return TRUE;
}
