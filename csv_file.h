#ifndef CSV_FILE_H
#define CSV_FILE_H

#include <stdio.h>

/* CSV を追記モードで開き、必要ならヘッダーを出力する。 */
FILE *csv_file_open(const char *path, const char *header);

#endif /* CSV_FILE_H */
