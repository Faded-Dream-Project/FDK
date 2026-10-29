#ifndef FDK_ATSPI_INTERNAL_H
#define FDK_ATSPI_INTERNAL_H

/* The AT-SPI bridge's pump hooks (src/desktop/atspi.c; called from
 * window.c's scheme tick + shutdown). The env knob ($FDK_ATSPI) and
 * the whole wire protocol live in the module. */

struct fdk_context;

/* The pump's attach tick: engages the bridge ONCE per context when a
 * window exists (throttled internally; degrades silently and
 * permanently when the desktop has no a11y bus). */
void fdk__atspi_tick(struct fdk_context *ctx);

/* Drops the subscription and both bus connections; zeroes the
 * registry. fdk_shutdown's path. */
void fdk__atspi_shutdown(void);

#endif /* FDK_ATSPI_INTERNAL_H */
