#include "binary_file.h"

#include <errno.h>
#include <stdio.h>

/*
 * バイナリログを上書きモードで開く。
 * CSV の追記とは異なり、各収録で1つの独立したバイナリファイルを作る。
 */
FILE *binary_file_open(const char *path)
{
  FILE *fp = fopen(path, "wb");

  if (fp == NULL)
    {
      printf("ERROR: Failed to open binary log file %s. %d\n", path, errno);
      return NULL;
    }

  return fp;
}

/*
 * fwrite() の戻り値を確認し、短い書き込みをエラーとして扱う。
 * label はどのデータの書き込みで失敗したかをログへ出すために使う。
 */
int binary_file_write(FILE *fp,
                      const void *data,
                      size_t element_size,
                      size_t element_count,
                      const char *label)
{
  if (element_count == 0)
    {
      return 0;
    }

  if (fwrite(data, element_size, element_count, fp) != element_count)
    {
      printf("ERROR: Failed to write %s. %d\n", label, errno);
      return 1;
    }

  return 0;
}
