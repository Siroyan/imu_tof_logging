#include "csv_file.h"

#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>

FILE *csv_file_open(const char *path, const char *header)
{
  struct stat st;
  int need_header;
  FILE *fp;

  need_header = (stat(path, &st) != 0 || st.st_size == 0);

  fp = fopen(path, "a");
  if (fp == NULL)
    {
      printf("ERROR: Failed to open log file %s. %d\n", path, errno);
      return NULL;
    }

  if (need_header && fprintf(fp, "%s\n", header) < 0)
    {
      printf("ERROR: Failed to write CSV header. %d\n", errno);
      fclose(fp);
      return NULL;
    }

  return fp;
}
