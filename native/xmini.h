// The few Xlib declarations the host and the test plug-in need, so they build without the libx11-dev package.
// They match <X11/Xlib.h>; link with -l:libX11.so.6.
#pragma once
typedef struct _XDisplay Display;
typedef unsigned long Window, Atom;
typedef struct _XGC *GC;
typedef union {
  int type;
  struct { int type; unsigned long serial; int send_event; Display *display; Window window; Atom message_type; int format; long l[5]; } xclient;
  struct { int type; unsigned long serial; int send_event; Display *display; Window event, window; int x, y, width, height; } xconfigure;
  struct { int type; unsigned long serial; int send_event; Display *display; Window window, root, subwindow; unsigned long time; int x, y, x_root, y_root; unsigned state, button; int same_screen; } xbutton;
  long pad[24];
} XEvent;
enum { ButtonPress = 4, MotionNotify = 6, Expose = 12, ConfigureNotify = 22, ClientMessage = 33 };
enum { ButtonPressMask = 1L << 2, ButtonMotionMask = 1L << 13, ExposureMask = 1L << 15, StructureNotifyMask = 1L << 17 };

int XInitThreads(void);
Display *XOpenDisplay(const char *name);
int XCloseDisplay(Display *d);
int XConnectionNumber(Display *d);
Window XDefaultRootWindow(Display *d);
Window XCreateSimpleWindow(Display *d, Window parent, int x, int y, unsigned w, unsigned h, unsigned border_w, unsigned long border, unsigned long background);
int XDestroyWindow(Display *d, Window w);
int XMapRaised(Display *d, Window w);
int XMapWindow(Display *d, Window w);
int XResizeWindow(Display *d, Window w, unsigned width, unsigned height);
int XStoreName(Display *d, Window w, const char *name);
Atom XInternAtom(Display *d, const char *name, int only_if_exists);
int XSetWMProtocols(Display *d, Window w, Atom *protocols, int count);
int XSelectInput(Display *d, Window w, long mask);
int XPending(Display *d);
int XNextEvent(Display *d, XEvent *e);
int XSendEvent(Display *d, Window w, int propagate, long mask, XEvent *e);
int XFlush(Display *d);
int XSync(Display *d, int discard);
int (*XSetErrorHandler(int (*handler)(Display *, void *)))(Display *, void *);
GC XCreateGC(Display *d, Window w, unsigned long mask, void *values);
int XFreeGC(Display *d, GC gc);
int XSetForeground(Display *d, GC gc, unsigned long colour);
int XFillRectangle(Display *d, Window w, GC gc, int x, int y, unsigned width, unsigned height);
int XDrawString(Display *d, Window w, GC gc, int x, int y, const char *text, int length);
typedef struct { int width, height, xoffset, format; char *data; int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth, bytes_per_line, bits_per_pixel; } XImage;   // leading fields only
XImage *XGetImage(Display *d, Window w, int x, int y, unsigned width, unsigned height, unsigned long plane_mask, int format);
