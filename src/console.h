// The command dispatcher's public surface.
//
// Implemented in main.cpp, alongside the state the commands act on.  It is
// published here so the web server can feed it the same way the serial
// console does, rather than growing a parallel implementation.

#ifndef CONSOLE_H
#define CONSOLE_H

#include <Print.h>

// Parse and run one command line, writing any output to `out`.  The line is
// consumed in place by strtok, so the caller must own a writable copy.
void handleCommand(char *line, Print &out);

// One line describing everything worth knowing.
void cmdStatus(Print &out);

#endif // CONSOLE_H
