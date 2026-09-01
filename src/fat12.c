#define _POSIX_C_SOURCE 200809L
#include "fat12.h"

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

#define MBR_SIGNATURE_OFFSET 510u
#define MBR_PARTITION_TABLE_OFFSET 446u
#define DIR_ENTRY_SIZE 32u
#define ATTR_DIRECTORY 0x10u
#define ATTR_VOLUME_ID 0x08u
#define ATTR_LONG_NAME 0x0Fu
#define FAT12_EOC_MIN 0x0FF8u
#define FAT12_BAD_CLUSTER 0x0FF7u
#define MAX_PATH_COMPONENTS 64u
#define ROOT_ENTRY 32u

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static bool read_exact(FILE *fp, uint64_t offset, void *buffer, size_t size)
{
    if (offset > (uint64_t)INT64_MAX)
    {
        return false;
    }
    if (fseeko(fp, (off_t)offset, SEEK_SET) != 0)
    {
        return false;
    }
    return fread(buffer, 1, size, fp) == size;
}

static uint64_t sector_offset(const Fat12Fs *fs, uint32_t absolute_sector)
{
    return (uint64_t)absolute_sector * fs->bytes_per_sector;
}

static Fat12Status cluster_offset(const Fat12Fs *fs, uint16_t cluster, uint64_t *offset)
{
    if (cluster < 2 || (uint32_t)cluster >= fs->cluster_count + 2u)
    {
        return FAT12_ERR_RANGE;
    }
    uint32_t sector = fs->first_data_sector + ((uint32_t)cluster - 2u) * fs->sectors_per_cluster;
    uint64_t byte_offset = sector_offset(fs, sector);
    uint64_t cluster_size = (uint64_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    if (byte_offset > fs->image_size || cluster_size > fs->image_size - byte_offset)
    {
        return FAT12_ERR_RANGE;
    }
    *offset = byte_offset;
    return FAT12_OK;
}

static Fat12Status read_fat_entry(const Fat12Fs *fs, uint16_t cluster, uint16_t *value)
{
    /* TODO 2: calcular el desplazamiento de una entrada FAT12 y extraer sus 12 bits.
     * Debe distinguir clusters pares e impares, validar limites y leer desde la primera FAT.
     */
    (void)fs;
    (void)cluster;
    (void)value;
    return FAT12_ERR_NOT_IMPLEMENTED;
}

static bool is_eoc(uint16_t value)
{
    return value >= FAT12_EOC_MIN && value <= 0x0FFFu;
}

static bool valid_short_char(unsigned char c)
{
    if (isalnum(c))
    {
        return true;
    }
    const char *extra = "_$~!#%&-{}()@'`";
    return strchr(extra, (int)c) != NULL;
}

static Fat12Status path_component_to_raw(const char *component, uint8_t raw[11])
{
    size_t len = strlen(component);
    if (len == 0 || len > 12)
    {
        return FAT12_ERR_FORMAT;
    }
    const char *dot = strrchr(component, '.');
    size_t stem_len = dot ? (size_t)(dot - component) : len;
    size_t ext_len = dot ? len - stem_len - 1u : 0u;
    if (stem_len < 1 || stem_len > 8 || ext_len > 3)
    {
        return FAT12_ERR_FORMAT;
    }
    memset(raw, ' ', 11);
    for (size_t i = 0; i < stem_len; ++i)
    {
        unsigned char c = (unsigned char)component[i];
        if (!valid_short_char(c))
        {
            return FAT12_ERR_FORMAT;
        }
        raw[i] = (uint8_t)toupper(c);
    }
    for (size_t i = 0; i < ext_len; ++i)
    {
        unsigned char c = (unsigned char)dot[1 + i];
        if (!valid_short_char(c))
        {
            return FAT12_ERR_FORMAT;
        }
        raw[8 + i] = (uint8_t)toupper(c);
    }
    return FAT12_OK;
}

static void raw_to_display_name(const uint8_t raw[11], bool deleted, char output[13])
{
    char stem[9];
    char ext[4];
    memcpy(stem, raw, 8);
    memcpy(ext, raw + 8, 3);
    stem[8] = '\0';
    ext[3] = '\0';
    for (int i = 7; i >= 0 && stem[i] == ' '; --i)
    {
        stem[i] = '\0';
    }
    for (int i = 2; i >= 0 && ext[i] == ' '; --i)
    {
        ext[i] = '\0';
    }
    if (deleted && stem[0] != '\0')
    {
        stem[0] = '?';
    }
    if (ext[0] != '\0')
    {
        (void)snprintf(output, 13, "%s.%s", stem, ext);
    }
    else
    {
        (void)snprintf(output, 13, "%s", stem);
    }
}

static bool is_dot_entry(const uint8_t raw[11])
{
    return raw[0] == '.' && (raw[1] == ' ' || raw[1] == '.');
}

static Fat12Status parse_dir_entry(const uint8_t raw[32], uint64_t offset, Fat12DirEntry *entry)
{
    if (raw[0] == 0x00u)
    {
        return FAT12_ERR_NOT_FOUND;
    }
    uint8_t attr = raw[11];
    if (attr == ATTR_LONG_NAME || (attr & ATTR_VOLUME_ID) != 0u)
    {
        return FAT12_ERR_FORMAT;
    }
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->raw_name, raw, 11);
    entry->attributes = attr;
    entry->first_cluster = read_le16(raw + 26);
    entry->size = read_le32(raw + 28);
    entry->deleted = raw[0] == 0xE5u;
    entry->directory = (attr & ATTR_DIRECTORY) != 0u;
    entry->entry_offset = offset;
    raw_to_display_name(entry->raw_name, entry->deleted, entry->name);
    return FAT12_OK;
}

typedef Fat12Status (*DirVisitor)(const Fat12DirEntry *entry, void *ctx, bool *stop);

static Fat12Status visit_directory(const Fat12Fs *fs, bool root, uint16_t first_cluster,
                                   DirVisitor visitor, void *ctx)
{
    if (root)
    {
        uint64_t base = sector_offset(fs, fs->first_root_sector);
        for (uint32_t i = 0; i < fs->root_entry_count; ++i)
        {
            uint8_t raw[DIR_ENTRY_SIZE];
            uint64_t offset = base + (uint64_t)i * DIR_ENTRY_SIZE;
            if (!read_exact(fs->fp, offset, raw, sizeof(raw)))
            {
                return FAT12_ERR_IO;
            }
            if (raw[0] == 0x00u)
            {
                return FAT12_OK;
            }
            if (raw[11] == ATTR_LONG_NAME || (raw[11] & ATTR_VOLUME_ID) != 0u || is_dot_entry(raw))
            {
                continue;
            }
            Fat12DirEntry entry;
            if (parse_dir_entry(raw, offset, &entry) != FAT12_OK)
            {
                continue;
            }
            bool stop = false;
            Fat12Status status = visitor(&entry, ctx, &stop);
            if (status != FAT12_OK || stop)
            {
                return status;
            }
        }
        return FAT12_OK;
    }

    uint16_t cluster = first_cluster;
    uint8_t *seen = calloc(fs->cluster_count + 2u, 1);
    if (!seen)
    {
        return FAT12_ERR_IO;
    }
    uint32_t cluster_size = (uint32_t)fs->bytes_per_sector * fs->sectors_per_cluster;
    uint8_t *buffer = malloc(cluster_size);
    if (!buffer)
    {
        free(seen);
        return FAT12_ERR_IO;
    }
    Fat12Status result = FAT12_OK;
    while (true)
    {
        if (cluster < 2 || (uint32_t)cluster >= fs->cluster_count + 2u || seen[cluster])
        {
            result = FAT12_ERR_FORMAT;
            break;
        }
        seen[cluster] = 1;
        uint64_t offset;
        result = cluster_offset(fs, cluster, &offset);
        if (result != FAT12_OK || !read_exact(fs->fp, offset, buffer, cluster_size))
        {
            result = result == FAT12_OK ? FAT12_ERR_IO : result;
            break;
        }
        for (uint32_t pos = 0; pos + DIR_ENTRY_SIZE <= cluster_size; pos += DIR_ENTRY_SIZE)
        {
            const uint8_t *raw = buffer + pos;
            if (raw[0] == 0x00u)
            {
                goto done;
            }
            if (raw[11] == ATTR_LONG_NAME || (raw[11] & ATTR_VOLUME_ID) != 0u || is_dot_entry(raw))
            {
                continue;
            }
            Fat12DirEntry entry;
            if (parse_dir_entry(raw, offset + pos, &entry) != FAT12_OK)
            {
                continue;
            }
            bool stop = false;
            result = visitor(&entry, ctx, &stop);
            if (result != FAT12_OK || stop)
            {
                goto done;
            }
        }
        uint16_t next;
        result = read_fat_entry(fs, cluster, &next);
        if (result != FAT12_OK)
        {
            break;
        }
        if (is_eoc(next))
        {
            break;
        }
        if (next == 0u || next == FAT12_BAD_CLUSTER || (next >= 0x0FF0u && next < FAT12_EOC_MIN))
        {
            result = FAT12_ERR_FORMAT;
            break;
        }
        cluster = next;
    }

done:
    free(buffer);
    free(seen);
    return result;
}

typedef struct
{
    uint8_t target[11];
    bool allow_deleted;
    Fat12DirEntry found;
    bool matched;
} FindContext;

static Fat12Status find_visitor(const Fat12DirEntry *entry, void *ctx_ptr, bool *stop)
{
    /* TODO 3: comparar nombres cortos 8.3.
     * Para entradas activas deben coincidir los 11 bytes.
     * Para una entrada borrada, el primer byte se perdio (0xE5): compare los 10 restantes.
     */
    (void)entry;
    (void)ctx_ptr;
    (void)stop;
    return FAT12_OK;
}

static Fat12Status find_in_directory(const Fat12Fs *fs, bool root, uint16_t cluster,
                                     const char *component, bool allow_deleted,
                                     Fat12DirEntry *entry)
{
    FindContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    Fat12Status status = path_component_to_raw(component, ctx.target);
    if (status != FAT12_OK)
    {
        return status;
    }
    ctx.allow_deleted = allow_deleted;
    status = visit_directory(fs, root, cluster, find_visitor, &ctx);
    if (status != FAT12_OK)
    {
        return status;
    }
    if (!ctx.matched)
    {
        return FAT12_ERR_NOT_FOUND;
    }
    *entry = ctx.found;
    return FAT12_OK;
}

static Fat12Status split_path(const char *path, char ***components, size_t *count)
{
    *components = NULL;
    *count = 0;
    if (!path || path[0] != '/')
    {
        return FAT12_ERR_FORMAT;
    }
    char *copy = strdup(path);
    if (!copy)
    {
        return FAT12_ERR_IO;
    }
    char **parts = calloc(MAX_PATH_COMPONENTS, sizeof(*parts));
    if (!parts)
    {
        free(copy);
        return FAT12_ERR_IO;
    }
    char *save = NULL;
    char *token = strtok_r(copy, "/", &save);
    while (token)
    {
        if (*count >= MAX_PATH_COMPONENTS)
        {
            for (size_t i = 0; i < *count; ++i)
            {
                free(parts[i]);
            }
            free(parts);
            free(copy);
            return FAT12_ERR_FORMAT;
        }
        parts[*count] = strdup(token);
        if (!parts[*count])
        {
            for (size_t i = 0; i < *count; ++i)
            {
                free(parts[i]);
            }
            free(parts);
            free(copy);
            return FAT12_ERR_IO;
        }
        (*count)++;
        token = strtok_r(NULL, "/", &save);
    }
    free(copy);
    *components = parts;
    return FAT12_OK;
}

static void free_path_components(char **components, size_t count)
{
    if (!components)
    {
        return;
    }
    for (size_t i = 0; i < count; ++i)
    {
        free(components[i]);
    }
    free(components);
}

static Fat12Status resolve_path(const Fat12Fs *fs, const char *path, bool allow_deleted_final,
                                Fat12DirEntry *entry, bool *is_root)
{
    char **parts = NULL;
    size_t count = 0;
    Fat12Status status = split_path(path, &parts, &count);
    if (status != FAT12_OK)
    {
        return status;
    }
    if (count == 0)
    {
        *is_root = true;
        memset(entry, 0, sizeof(*entry));
        free_path_components(parts, count);
        return FAT12_OK;
    }
    bool root = true;
    uint16_t cluster = 0;
    Fat12DirEntry current;
    for (size_t i = 0; i < count; ++i)
    {
        bool allow_deleted = allow_deleted_final && i + 1u == count;
        status = find_in_directory(fs, root, cluster, parts[i], allow_deleted, &current);
        if (status != FAT12_OK)
        {
            free_path_components(parts, count);
            return status;
        }
        if (i + 1u < count)
        {
            if (current.deleted)
            {
                free_path_components(parts, count);
                return FAT12_ERR_NOT_FOUND;
            }
            if (!current.directory)
            {
                free_path_components(parts, count);
                return FAT12_ERR_NOT_DIR;
            }
            root = false;
            cluster = current.first_cluster;
        }
    }
    *entry = current;
    *is_root = false;
    free_path_components(parts, count);
    return FAT12_OK;
}

Fat12Status set_fat12_image_size(Fat12Fs *fs)
{
    if (fseeko(fs->fp, 0, SEEK_END) != 0)
    {
        return FAT12_ERR_IO;
    }

    fs->image_size = (uint64_t)ftello(fs->fp);
    printf("El archivo pesa %ld bytes\n", fs->image_size);

    if (fseeko(fs->fp, 0, SEEK_SET) != 0)
    {
        return FAT12_ERR_IO;
    }
    return FAT12_OK;
}

Fat12Status check_fat12_mbr_signature(Fat12Fs *fs)
{
    uint8_t signature[2];

    if (!read_exact(fs->fp, MBR_SIGNATURE_OFFSET,
                    signature, sizeof(signature)))
    {
        return FAT12_ERR_IO;
    }
    if (signature[0] != 0x55u || signature[1] != 0xAAu)
    {
        return FAT12_ERR_FORMAT;
    }

    return FAT12_OK;
}

Fat12Status read_partition(Fat12Fs *fs)
{
    uint8_t initial_lba[4];

    if (!read_exact(fs->fp, MBR_PARTITION_TABLE_OFFSET + 8u,
                    initial_lba, sizeof(initial_lba)))
    {
        return FAT12_ERR_IO;
    }
    fs->partition_lba = read_le32(initial_lba);

    uint8_t sector_count[4];

    if (!read_exact(fs->fp, MBR_PARTITION_TABLE_OFFSET + 12u,
                    sector_count, sizeof(sector_count)))
    {
        return FAT12_ERR_IO;
    }

    fs->partition_sectors = read_le32(sector_count);

    printf("La Direccion logica de LBA %u sectores y sectores de particion %u sectores\n", fs->partition_lba, fs->partition_sectors);

    return FAT12_OK;
}

Fat12Status read_BPB(Fat12Fs *fs)
{
    // 512 son los bytes del sector
    uint64_t bpb_offset = fs->partition_lba * 512u;

    if (bpb_offset > fs->image_size)
    {
        return FAT12_ERR_RANGE;
    }

    uint8_t bpb[512];
    if (!read_exact(fs->fp, bpb_offset,
                    bpb, sizeof(bpb)))
    {
        return FAT12_ERR_IO;
    }

    if (bpb[510] != 0x55u || bpb[511] != 0xAAu)
    {
        return FAT12_ERR_FORMAT;
    }

    fs->bytes_per_sector = read_le16(bpb + 0x0B);
    fs->sectors_per_cluster = bpb[0x0D];
    fs->reserved_sectors = read_le16(bpb + 0x0E);
    fs->fat_count = bpb[0x10];
    fs->root_entry_count = read_le16(bpb + 0x11);
    fs->sectors_per_fat = read_le16(bpb + 0x16);
    uint16_t total_sectors_16 = read_le16(bpb + 0x13);

    if (total_sectors_16 != 0)
    {
        fs->total_sectors = total_sectors_16;
    }
    else
    { // Esto se hace generalmente cuando tenemos Fat16
        fs->total_sectors = read_le32(bpb + 0x20);
    }

    if (fs->bytes_per_sector == 0 ||
        fs->sectors_per_cluster == 0 ||
        fs->reserved_sectors == 0 ||
        fs->fat_count == 0 ||
        fs->sectors_per_fat == 0 ||
        fs->total_sectors == 0)
    {
        return FAT12_ERR_FORMAT;
    }
    // Nombre del volumen
    memcpy(fs->volume_label, bpb + 0x2B, 11);
    fs->volume_label[11] = '\0';

    return FAT12_OK;
}

Fat12Status calculate_count_sectors(Fat12Fs *fs)
{
    fs->root_dir_sectors = (fs->root_entry_count * ROOT_ENTRY + fs->bytes_per_sector - 1u) / fs->bytes_per_sector;

    uint32_t fat_sectors = fs->fat_count * fs->sectors_per_fat;
    uint32_t non_data_sectors = fs->reserved_sectors + fat_sectors + fs->root_dir_sectors;
    if (fs->total_sectors < non_data_sectors)
    {
        return FAT12_ERR_FORMAT;
    }

    fs->data_sectors = fs->total_sectors - non_data_sectors;

    fs->cluster_count = fs->data_sectors / fs->sectors_per_cluster;
}

Fat12Status fat12_open(Fat12Fs *fs, const char *image_path)
{
    /* TODO 1: abrir la imagen en modo solo lectura y reconstruir el layout.
     * Pasos minimos:
     *  + obtener el tamano de la imagen ;
     *  + validar firma del MBR;
     *  + leer LBA inicial y cantidad de sectores de la primera particion;
     *  + leer y validar el Boot Sector de la particion;
     *  + interpretar el BPB con funciones little-endian;
     *  + calcular root_dir_sectors, data_sectors y cluster_count;
     *  - aceptar solo FAT12 (cluster_count < 4085);
     *  - calcular first_fat_sector, first_root_sector y first_data_sector;
     *  - validar que ninguna region quede fuera del archivo.
     */
    if (!fs || !image_path)
    {
        return FAT12_ERR_USAGE;
    }
    memset(fs, 0, sizeof(*fs));
    fs->fp = fopen(image_path, "rb");
    if (!fs->fp)
    {
        return FAT12_ERR_IO;
    }
    Fat12Status status = set_fat12_image_size(fs);
    if (status != FAT12_OK)
    {
        fat12_close(fs);
        return status;
    }

    status = check_fat12_mbr_signature(fs);
    if (status != FAT12_OK)
    {
        fat12_close(fs);
        return status;
    }

    status = read_partition(fs);
    if (status != FAT12_OK)
    {
        fat12_close(fs);
        return status;
    }

    status = read_BPB(fs);
    if (status != FAT12_OK)
    {
        fat12_close(fs);
        return status;
    }

    status = calculate_count_sectors(fs);
    if (status != FAT12_OK)
    {
        fat12_close(fs);
        return status;
    }

    return FAT12_ERR_NOT_IMPLEMENTED;
}

void fat12_close(Fat12Fs *fs)
{
    if (fs && fs->fp)
    {
        fclose(fs->fp);
        fs->fp = NULL;
    }
}

const char *fat12_status_string(Fat12Status status)
{
    switch (status)
    {
    case FAT12_OK:
        return "ok";
    case FAT12_ERR_USAGE:
        return "uso invalido";
    case FAT12_ERR_UNSUPPORTED:
        return "tipo FAT no soportado; se requiere FAT12";
    case FAT12_ERR_IO:
        return "error de entrada/salida";
    case FAT12_ERR_FORMAT:
        return "imagen o ruta con formato invalido";
    case FAT12_ERR_NOT_FOUND:
        return "archivo o directorio no encontrado";
    case FAT12_ERR_NOT_DIR:
        return "un componente de la ruta no es un directorio";
    case FAT12_ERR_IS_DIR:
        return "la ruta corresponde a un directorio";
    case FAT12_ERR_RANGE:
        return "lectura fuera de los limites de la imagen";
    case FAT12_ERR_RECOVERY:
        return "no se cumplen las condiciones de recuperacion simple";
    case FAT12_ERR_NOT_IMPLEMENTED:
        return "funcion pendiente de implementar";
    default:
        return "error desconocido";
    }
}

Fat12Status fat12_print_info(const Fat12Fs *fs, FILE *out)
{
    if (!fs || !fs->fp || !out)
    {
        return FAT12_ERR_USAGE;
    }
    fprintf(out, "fat_type=FAT12\n");
    fprintf(out, "partition_lba=%" PRIu32 "\n", fs->partition_lba);
    fprintf(out, "partition_sectors=%" PRIu32 "\n", fs->partition_sectors);
    fprintf(out, "bytes_per_sector=%" PRIu16 "\n", fs->bytes_per_sector);
    fprintf(out, "sectors_per_cluster=%" PRIu8 "\n", fs->sectors_per_cluster);
    fprintf(out, "reserved_sectors=%" PRIu16 "\n", fs->reserved_sectors);
    fprintf(out, "fat_count=%" PRIu8 "\n", fs->fat_count);
    fprintf(out, "sectors_per_fat=%" PRIu16 "\n", fs->sectors_per_fat);
    fprintf(out, "root_entries=%" PRIu16 "\n", fs->root_entry_count);
    fprintf(out, "first_root_sector=%" PRIu32 "\n", fs->first_root_sector);
    fprintf(out, "first_data_sector=%" PRIu32 "\n", fs->first_data_sector);
    fprintf(out, "cluster_count=%" PRIu32 "\n", fs->cluster_count);
    fprintf(out, "volume_label=%.11s\n", fs->volume_label);
    return FAT12_OK;
}

typedef struct
{
    FILE *out;
} ListContext;

static Fat12Status list_visitor(const Fat12DirEntry *entry, void *ctx_ptr, bool *stop)
{
    (void)stop;
    ListContext *ctx = ctx_ptr;
    const char *type = entry->deleted ? "DELETED" : (entry->directory ? "DIR" : "FILE");
    fprintf(ctx->out, "%s\t%" PRIu32 "\t%" PRIu16 "\t%s\n",
            type, entry->size, entry->first_cluster, entry->name);
    return FAT12_OK;
}

Fat12Status fat12_list(const Fat12Fs *fs, const char *path, FILE *out)
{
    if (!fs || !path || !out)
    {
        return FAT12_ERR_USAGE;
    }
    Fat12DirEntry entry;
    bool root = false;
    Fat12Status status = resolve_path(fs, path, false, &entry, &root);
    if (status != FAT12_OK)
    {
        return status;
    }
    if (!root && !entry.directory)
    {
        return FAT12_ERR_NOT_DIR;
    }
    fprintf(out, "TYPE\tSIZE\tCLUSTER\tNAME\n");
    ListContext ctx = {.out = out};
    return visit_directory(fs, root, root ? 0u : entry.first_cluster, list_visitor, &ctx);
}

static Fat12Status read_active_entry(const Fat12Fs *fs, const Fat12DirEntry *entry,
                                     uint8_t **data, size_t *size)
{
    /* TODO 4: leer un archivo activo siguiendo su cadena de clusters.
     * Debe respetar entry->size, detectar ciclos, fin de cadena prematuro,
     * clusters reservados/defectuosos y lecturas fuera de la imagen.
     */
    (void)fs;
    (void)entry;
    if (data)
    {
        *data = NULL;
    }
    if (size)
    {
        *size = 0;
    }
    return FAT12_ERR_NOT_IMPLEMENTED;
}

Fat12Status fat12_read_file(const Fat12Fs *fs, const char *path, uint8_t **data, size_t *size)
{
    if (!fs || !path || !data || !size)
    {
        return FAT12_ERR_USAGE;
    }
    Fat12DirEntry entry;
    bool root = false;
    Fat12Status status = resolve_path(fs, path, false, &entry, &root);
    if (status != FAT12_OK)
    {
        return status;
    }
    if (root || entry.directory || entry.deleted)
    {
        return FAT12_ERR_IS_DIR;
    }
    return read_active_entry(fs, &entry, data, size);
}

static Fat12Status write_output_file(const char *output_path, const uint8_t *data, size_t size)
{
    FILE *out = fopen(output_path, "wb");
    if (!out)
    {
        return FAT12_ERR_IO;
    }
    bool ok = fwrite(data, 1, size, out) == size;
    if (fclose(out) != 0)
    {
        ok = false;
    }
    return ok ? FAT12_OK : FAT12_ERR_IO;
}

Fat12Status fat12_extract(const Fat12Fs *fs, const char *path, const char *output_path)
{
    uint8_t *data = NULL;
    size_t size = 0;
    Fat12Status status = fat12_read_file(fs, path, &data, &size);
    if (status != FAT12_OK)
    {
        return status;
    }
    status = write_output_file(output_path, data, size);
    free(data);
    return status;
}

Fat12Status fat12_recover_deleted(const Fat12Fs *fs, const char *path, const char *output_path)
{
    /* TODO 5: recuperar un archivo borrado bajo el modelo acotado del TP.
     * Condiciones: nombre 8.3 conocido, archivo regular, clusters contiguos,
     * clusters todavia libres en la FAT e imagen abierta solo para lectura.
     */
    (void)fs;
    (void)path;
    (void)output_path;
    return FAT12_ERR_NOT_IMPLEMENTED;
}
