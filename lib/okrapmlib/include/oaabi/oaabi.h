#ifndef OaabiHeader
#define OaabiHeader

#ifdef __cplusplus
extern "C" {
#endif

#define OaabiVersion 1

#include <stdint.h>

struct OaabiPlugin {
	uint32_t Version;
	uint32_t Reserved;
	int (*Init)(void);
	void (*Fini)(void);
};

static inline int OaabiGetVersion(void)
{
	return OaabiVersion;
}

#ifdef __cplusplus
}
#endif

#endif
