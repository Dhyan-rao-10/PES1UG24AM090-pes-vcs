// object.c — Content-addressable object store

#include "pes.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <openssl/evp.h>
#include <errno.h>

// ─── PROVIDED ────────────────────────────────────────────────────────────────

void hash_to_hex(const ObjectID *id, char *hex_out) {
    for (int i = 0; i < HASH_SIZE; i++) {
        sprintf(hex_out + i * 2, "%02x", id->hash[i]);
    }
    hex_out[HASH_HEX_SIZE] = '\0';
}

int hex_to_hash(const char *hex, ObjectID *id_out) {
    if (strlen(hex) < HASH_HEX_SIZE) return -1;
    for (int i = 0; i < HASH_SIZE; i++) {
        unsigned int byte;
        if (sscanf(hex + i * 2, "%2x", &byte) != 1) return -1;
        id_out->hash[i] = (uint8_t)byte;
    }
    return 0;
}

void compute_hash(const void *data, size_t len, ObjectID *id_out) {
    unsigned int hash_len;
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), NULL);
    EVP_DigestUpdate(ctx, data, len);
    EVP_DigestFinal_ex(ctx, id_out->hash, &hash_len);
    EVP_MD_CTX_free(ctx);
}

void object_path(const ObjectID *id, char *path_out, size_t path_size) {
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(id, hex);
    snprintf(path_out, path_size, "%s/%.2s/%s", OBJECTS_DIR, hex, hex + 2);
}

int object_exists(const ObjectID *id) {
    char path[512];
    object_path(id, path, sizeof(path));
    return access(path, F_OK) == 0;
}

// ─── IMPLEMENTED ─────────────────────────────────────────────────────────────

int object_write(ObjectType type, const void *data, size_t len, ObjectID *id_out) {
    int fd = -1;
    char type_str[16];

    switch (type) {
        case OBJ_BLOB:   strcpy(type_str, "blob");   break;
        case OBJ_TREE:   strcpy(type_str, "tree");   break;
        case OBJ_COMMIT: strcpy(type_str, "commit"); break;
        default: return -1;
    }

    // 1. Build full object: header + data
    char header[64];
    int header_len = snprintf(header, sizeof(header), "%s %zu", type_str, len) + 1;
    // +1 includes the null terminator as part of the format

    size_t full_len = (size_t)header_len + len;
    char *full = malloc(full_len);
    if (!full) return -1;

    memcpy(full, header, header_len);
    if (len > 0 && data != NULL)
        memcpy(full + header_len, data, len);

    // 2. Compute hash of full object
    ObjectID id;
    compute_hash(full, full_len, &id);

    // 3. Deduplication
    if (object_exists(&id)) {
        *id_out = id;
        free(full);
        return 0;
    }

    // Convert hash to hex string
    char hex[HASH_HEX_SIZE + 1];
    hash_to_hex(&id, hex);

    // 4. Create shard directory
    char dir_path[256];
    snprintf(dir_path, sizeof(dir_path), "%s/%.2s", OBJECTS_DIR, hex);
    if (mkdir(dir_path, 0755) < 0 && errno != EEXIST) {
        free(full);
        return -1;
    }

    // 5. Compute final and temp paths
    char final_path[512];
    object_path(&id, final_path, sizeof(final_path));

    char temp_path[512];
    snprintf(temp_path, sizeof(temp_path), "%s/tmp_%d", dir_path, (int)getpid());

    // 6. Write to temp file
    fd = open(temp_path, O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd < 0) { free(full); return -1; }

    if (write(fd, full, full_len) != (ssize_t)full_len) {
        close(fd); free(full); return -1;
    }

    // 7. fsync the file
    if (fsync(fd) < 0) {
        close(fd); free(full); return -1;
    }
    close(fd);
    fd = -1;

    free(full);
    full = NULL;

    // 8. Atomic rename
    if (rename(temp_path, final_path) < 0) return -1;

    // 9. fsync the directory (best-effort — ignore errors on systems that don't support it)
    int dir_fd = open(dir_path, O_RDONLY);
    if (dir_fd >= 0) {
        fsync(dir_fd);  // ignore return value — not all filesystems support dir fsync
        close(dir_fd);
    }

    // 10. Return hash
    *id_out = id;
    return 0;
}

int object_read(const ObjectID *id, ObjectType *type_out, void **data_out, size_t *len_out) {
    char path[512];
    object_path(id, path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (!f) return -1;

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    rewind(f);

    if (file_size <= 0) { fclose(f); return -1; }

    char *buf = malloc(file_size);
    if (!buf) { fclose(f); return -1; }

    if (fread(buf, 1, file_size, f) != (size_t)file_size) {
        free(buf); fclose(f); return -1;
    }
    fclose(f);

    // Parse header — find the null terminator
    char *null_pos = memchr(buf, '\0', file_size);
    if (!null_pos) { free(buf); return -1; }

    size_t header_len = null_pos - buf;

    char type_str[16];
    size_t size;
    if (sscanf(buf, "%15s %zu", type_str, &size) != 2) {
        free(buf); return -1;
    }

    if      (strcmp(type_str, "blob")   == 0) *type_out = OBJ_BLOB;
    else if (strcmp(type_str, "tree")   == 0) *type_out = OBJ_TREE;
    else if (strcmp(type_str, "commit") == 0) *type_out = OBJ_COMMIT;
    else { free(buf); return -1; }

    // Verify integrity
    ObjectID check_id;
    compute_hash(buf, file_size, &check_id);
    if (memcmp(check_id.hash, id->hash, HASH_SIZE) != 0) {
        free(buf); return -1;
    }

    // Extract data portion
    char *data_start = null_pos + 1;
    if ((size_t)(file_size - (long)(header_len + 1)) != size) {
        free(buf); return -1;
    }

    void *out = malloc(size > 0 ? size : 1);
    if (!out) { free(buf); return -1; }

    if (size > 0) memcpy(out, data_start, size);

    *data_out = out;
    *len_out  = size;

    free(buf);
    return 0;
}
