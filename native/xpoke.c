// Test helper: sends a synthetic mouse click or a window-close request to an X11 window.
//   xpoke click <window id> <x> <y>      xpoke close <window id>      xpoke shot <window id> <width> <height> <out.ppm>
#include "xmini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
  if (argc < 3) { fprintf(stderr, "usage: xpoke click <window> <x> <y> | close <window>\n"); return 2; }
  Display *d = XOpenDisplay(NULL); if (!d) { fprintf(stderr, "no display\n"); return 1; }
  Window w = strtoul(argv[2], NULL, 0); XEvent e; memset(&e, 0, sizeof e);
  if (!strcmp(argv[1], "click") && argc >= 5) {
    e.type = ButtonPress; e.xbutton.display = d; e.xbutton.window = w; e.xbutton.x = atoi(argv[3]); e.xbutton.y = atoi(argv[4]); e.xbutton.button = 1; e.xbutton.same_screen = 1;
    XSendEvent(d, w, 0, ButtonPressMask, &e);
  } else if (!strcmp(argv[1], "close")) {
    e.type = ClientMessage; e.xclient.display = d; e.xclient.window = w; e.xclient.message_type = XInternAtom(d, "WM_PROTOCOLS", 0); e.xclient.format = 32; e.xclient.l[0] = (long)XInternAtom(d, "WM_DELETE_WINDOW", 0);
    XSendEvent(d, w, 0, 0, &e);
  } else if (!strcmp(argv[1], "shot") && argc >= 6) {
    int wd = atoi(argv[3]), ht = atoi(argv[4]); XImage *im = XGetImage(d, w, 0, 0, (unsigned)wd, (unsigned)ht, ~0UL, 2);
    if (!im) { fprintf(stderr, "could not read the window\n"); return 1; }
    FILE *f = fopen(argv[5], "wb"); fprintf(f, "P6 %d %d 255\n", wd, ht);
    for (int y = 0; y < ht; y++) for (int x = 0; x < wd; x++) { const unsigned char *p = (const unsigned char *)im->data + y * im->bytes_per_line + x * (im->bits_per_pixel / 8); fputc(p[2], f); fputc(p[1], f); fputc(p[0], f); }
    fclose(f);
  } else return 2;
  XSync(d, 0); XCloseDisplay(d); return 0;
}
