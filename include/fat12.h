#ifndef FAT12_H
#define FAT12_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

typedef enum {
    FAT12_OK = 0,
    FAT12_ERR_USAGE = 2,
    FAT12_ERR_UNSUPPORTED = 3,
    FAT12_ERR_IO = 4,
    FAT12_ERR_FORMAT = 5,
    FAT12_ERR_NOT_FOUND = 6,
    FAT12_ERR_NOT_DIR = 7,
    FAT12_ERR_IS_DIR = 8,
    FAT12_ERR_RANGE = 9,
    FAT12_ERR_RECOVERY = 10,
    FAT12_ERR_NOT_IMPLEMENTED = 20
} Fat12Status;

typedef struct {
    FILE *fp;
    uint64_t image_size;
    uint32_t partition_lba;
    uint32_t partition_sectors;
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t fat_count;
    uint16_t root_entry_count;
    uint16_t sectors_per_fat;
    uint32_t total_sectors;
    uint32_t root_dir_sectors;
    uint32_t first_fat_sector;
    uint32_t first_root_sector;
    uint32_t first_data_sector;
    uint32_t data_sectors;
    uint32_t cluster_count;
    char volume_label[12];
} Fat12Fs;

typedef struct {
    char name[13];
    uint8_t raw_name[11];
    uint8_t attributes;
    uint16_t first_cluster;
    uint32_t size;
    bool deleted;
    bool directory;
    uint64_t entry_offset;
} Fat12DirEntry;

Fat12Status fat12_open(Fat12Fs *fs, const char *image_path);
void fat12_close(Fat12Fs *fs);
const char *fat12_status_string(Fat12Status status);

Fat12Status fat12_print_info(const Fat12Fs *fs, FILE *out);
Fat12Status fat12_list(const Fat12Fs *fs, const char *path, FILE *out);
Fat12Status fat12_read_file(const Fat12Fs *fs, const char *path, uint8_t **data, size_t *size);
Fat12Status fat12_extract(const Fat12Fs *fs, const char *path, const char *output_path);
Fat12Status fat12_recover_deleted(const Fat12Fs *fs, const char *path, const char *output_path);

#endif
