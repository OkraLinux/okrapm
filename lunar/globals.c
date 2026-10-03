#include "okpm.h"

OkpmContext g_ctx;
int g_lock_fd = -1;
char g_tmpdir[MAX_PATH];
int g_config_loaded = 0;
