#pragma once

#include <stdbool.h>
#include <stdio.h>
#include "audio_decode_types.h"

#ifdef __cplusplus
extern "C" {
#endif

#include "flacdecoder.h"

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    FLACContext ctx;        // Decoder state from components/flac
    uint8_t *data_buf;      // Sliding read buffer for input frames
    size_t data_buf_size;   // Allocated size for data_buf
    size_t bytes_in_data_buf; // Bytes currently stored in data_buf
    uint8_t *read_ptr;      // Read cursor within data_buf
    bool eof_reached;       // Set once fread() reaches EOF
    long data_start;        // Byte offset where audio frames begin
} flac_instance;

void flac_instance_init(flac_instance *instance);
bool is_flac(FILE *fp, decode_data *output, flac_instance *instance);
DECODE_STATUS decode_flac(FILE *fp, decode_data *pData, flac_instance *pInstance);
void flac_instance_free(flac_instance *instance);

#ifdef __cplusplus
}
#endif
