#include <stdarg.h>
#include <stdio.h>

int rockbox_snprintf (char *buf, int size, const char *fmt, ...);
int rockbox_vsnprintf (char *buf, int size, const char *fmt, va_list ap);
int rockbox_fprintf (int fd, const char *fmt, ...);

#define snprintf rockbox_snprintf
#define vsnprintf rockbox_vsnprintf
#define fprintf rockbox_fprintf
