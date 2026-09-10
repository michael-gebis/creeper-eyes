// The tab icon, as an inline data: URI.
//
// A URI rather than a /favicon.ico route: no second handler, no second
// request against a server that manages one client at a time, and the whole
// thing costs about four hundred bytes of flash.  SVG rather than ICO
// because it is legible in source and scales to whatever the browser asks
// for.
//
// Only '#' is percent-encoded, because it would otherwise start a fragment
// and throw away the rest of the colour.  The SVG's own attributes use
// single quotes so the double quote that delimits the href stays safe, and
// '<' and '>' are literal characters inside an attribute value, so they are
// left alone and the artwork stays readable.  Each icon must be one line
// with no newlines -- adjacent string literals concatenate without any, so
// the wrapping below is free.
//
// Designed on a 32-unit grid with nothing thinner than about three units:
// a favicon is seen at 16 pixels and almost never larger, so anything finer
// than that is detail nobody will ever resolve.

#ifndef FAVICON_H
#define FAVICON_H

#include "config.h"

// Frank himself: flat crown, heavy fringe, a bolt through either side of the
// neck.  The fringe is a dark slate green rather than the near-black it
// started as -- black on the icon's own near-black ground meant the flat top,
// which is the whole silhouette, simply was not there.
#define FAVICON_URI_FRANK                                                     \
  "data:image/svg+xml,"                                                       \
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"              \
  "<rect width='32' height='32' rx='6' fill='%231F2621'/>"                    \
  "<rect x='1.5' y='17' width='4.5' height='6' rx='1.4' fill='%239AA3AA'/>"   \
  "<rect x='26' y='17' width='4.5' height='6' rx='1.4' fill='%239AA3AA'/>"    \
  "<rect x='6' y='6' width='20' height='22' rx='2.5' fill='%237FA05C'/>"      \
  "<rect x='6' y='6' width='20' height='6.5' rx='2.5' fill='%2331473F'/>"     \
  "<rect x='6' y='10' width='20' height='2.5' fill='%2331473F'/>"             \
  "<rect x='9.5' y='17' width='4.5' height='3.5' rx='1' fill='%23141814'/>"   \
  "<rect x='18' y='17' width='4.5' height='3.5' rx='1' fill='%23141814'/>"    \
  "</svg>"

// Just the eyes, glowing in the dark, pupils cut to one side.  For a build
// that is not going into a Frankenstein -- the renderer does not care what
// the head around it looks like, and neither should the tab.
#define FAVICON_URI_EYES                                                      \
  "data:image/svg+xml,"                                                       \
  "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'>"              \
  "<rect width='32' height='32' rx='6' fill='%23101418'/>"                    \
  "<circle cx='10' cy='16' r='6.4' fill='%236BA3D6'/>"                        \
  "<circle cx='22' cy='16' r='6.4' fill='%236BA3D6'/>"                        \
  "<circle cx='11.8' cy='16' r='2.9' fill='%230A0D10'/>"                      \
  "<circle cx='23.8' cy='16' r='2.9' fill='%230A0D10'/>"                      \
  "</svg>"

// Only the selected one is compiled in; the other costs nothing.
#if FAVICON == FAVICON_EYES
#define FAVICON_URI FAVICON_URI_EYES
#else
#define FAVICON_URI FAVICON_URI_FRANK
#endif

#endif // FAVICON_H
