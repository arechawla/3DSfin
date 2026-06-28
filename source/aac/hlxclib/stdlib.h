/* Minimal shim for the Helix hlxclib stdlib wrapper.
   The ESP8266Audio fork routes libc through hlxclib/ to allow Arduino-specific
   allocator overrides. On the 3DS (devkitARM/newlib) the standard library is
   exactly what we want, so forward straight to it. */
#ifndef HLXCLIB_STDLIB_H
#define HLXCLIB_STDLIB_H
#include <stdlib.h>
#endif
