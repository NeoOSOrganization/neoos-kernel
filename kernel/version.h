#ifndef NEOOS_VERSION_H
#define NEOOS_VERSION_H

#define NEOOS_VERSION "0.1"

// -D'd by the Makefile: `git describe --always --dirty --tags`.
#ifndef NEOOS_GITREV
#define NEOOS_GITREV "unknown"
#endif

#endif
