#pragma once
#include <stddef.h>

enum { UPD_IDLE, UPD_CHECKING, UPD_DOWNLOADING, UPD_CURRENT, UPD_READY, UPD_ERROR, UPD_USB, UPD_OFF };

void update_start(void);                     /* background checks (installed copies only) */
void update_check_now(void);
int update_state(char *ver, size_t cap);     /* UPD_*; ver = the ready/downloading version */
int update_version_newer(const char *a, const char *b);
