#ifndef BINARY_FILE_H
#define BINARY_FILE_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* バイナリログを上書きモードで開く。 */
FILE *binary_file_open(const char *path);

/* バイナリデータを指定件数だけ書き込む。 */
int binary_file_write(FILE *fp,
                      const void *data,
                      size_t element_size,
                      size_t element_count,
                      const char *label);

#ifdef __cplusplus
}
#endif

#endif /* BINARY_FILE_H */
