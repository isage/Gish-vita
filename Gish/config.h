#ifndef GISH_CONFIG_H
#define GISH_CONFIG_H

#include <stdio.h>
#define TO_DEBUG_LOG(...) fprintf(stderr, __VA_ARGS__)

typedef enum
{
  FALSE = 0,
  TRUE  = 1
} bool;
extern char *gishDataPath;

#endif /* GISH_CONFIG_H */
