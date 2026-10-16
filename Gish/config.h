#ifndef GISH_CONFIG_H
#define GISH_CONFIG_H

#include <stdio.h>
#define TO_DEBUG_LOG(...) fprintf(stderr, __VA_ARGS__)

#include <stdbool.h>
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
extern char *gishDataPath;

#endif /* GISH_CONFIG_H */
