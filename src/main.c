#include "fat12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(FILE *out) {
    fprintf(out,
        "Uso:\n"
        "  fat12tool info IMAGE\n"
        "  fat12tool ls IMAGE RUTA\n"
        "  fat12tool cat IMAGE RUTA\n"
        "  fat12tool extract IMAGE RUTA SALIDA\n"
        "  fat12tool recover IMAGE RUTA_BORRADA SALIDA\n");
}

static int fail(Fat12Status status) {
    fprintf(stderr, "error: %s\n", fat12_status_string(status));
    return (int)status;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        usage(stderr);
        return FAT12_ERR_USAGE;
    }
    const char *command = argv[1];
    const char *image = argv[2];
    Fat12Fs fs;
    Fat12Status status = fat12_open(&fs, image);
    if (status != FAT12_OK) {
        return fail(status);
    }
    int result = 0;
    if (strcmp(command, "info") == 0 && argc == 3) {
        status = fat12_print_info(&fs, stdout);
    } else if (strcmp(command, "ls") == 0 && argc == 4) {
        status = fat12_list(&fs, argv[3], stdout);
    } else if (strcmp(command, "cat") == 0 && argc == 4) {
        uint8_t *data = NULL;
        size_t size = 0;
        status = fat12_read_file(&fs, argv[3], &data, &size);
        if (status == FAT12_OK) {
            if (fwrite(data, 1, size, stdout) != size) {
                status = FAT12_ERR_IO;
            }
            free(data);
        }
    } else if (strcmp(command, "extract") == 0 && argc == 5) {
        status = fat12_extract(&fs, argv[3], argv[4]);
    } else if (strcmp(command, "recover") == 0 && argc == 5) {
        status = fat12_recover_deleted(&fs, argv[3], argv[4]);
    } else {
        usage(stderr);
        status = FAT12_ERR_USAGE;
    }
    if (status != FAT12_OK) {
        result = fail(status);
    }
    fat12_close(&fs);
    return result;
}
