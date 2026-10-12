/* transport.h - the connection layer under http.c for this system:
   AcornSSL on RISC OS (transport_riscos.c), OpenSSL elsewhere
   (transport_openssl.c). Part of Tube. MIT licence. */
#ifndef TUBE_TRANSPORT_H
#define TUBE_TRANSPORT_H
#include <stddef.h>
#include "http.h"

extern const http_transport transport_system;

/* 0 if secure connections can be made (RISC OS: the AcornSSL module is
   loaded), else -1 with the reason. */
int transport_check (char *err, size_t errlen);

/* Command-line programs: wait a moment (centiseconds) between polls. */
void transport_wait (int cs);

#endif
