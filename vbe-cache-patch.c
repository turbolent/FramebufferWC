#define _POSIX_SOURCE 1
#define _NEXT_SOURCE 1

#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* OPENSTEP's public headers omit these BSD routine prototypes. */
extern int fchown(int descriptor, uid_t owner, gid_t group);
extern int fchmod(int descriptor, mode_t mode);
extern int fsync(int descriptor);

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#define TOOL_VERSION "0.27"
#define VBE_DRIVER_PATH \
    "/private/Drivers/i386/VBE20DisplayDriver.config/VBE20DisplayDriver_reloc"
#define VBE_DRIVER_SIZE 37984UL
#define VBE_MAX_FILE_SIZE (16UL * 1024UL * 1024UL)
#define CURRENT_OFFSET 0x0b80UL
#define MODE_OFFSET 0x0cacUL

typedef unsigned int U32;
typedef unsigned long long U64;

typedef struct SHA256Context {
    U32 state[8];
    U64 bitCount;
    unsigned char block[64];
    unsigned blockLength;
} SHA256Context;

typedef enum VBEState {
    VBE_STATE_UNKNOWN = 0,
    VBE_STATE_STOCK,
    VBE_STATE_PATCHED
} VBEState;

static const unsigned char stockDigest[32] = {
    0x9f, 0xbc, 0x2c, 0xaf, 0xbd, 0xd0, 0x12, 0x4c,
    0xc6, 0x3b, 0x90, 0x21, 0x61, 0xc8, 0x6b, 0xbf,
    0xec, 0x0e, 0xf4, 0x8d, 0x59, 0x1d, 0xda, 0x68,
    0x1a, 0x32, 0x5f, 0xf7, 0xb6, 0x8d, 0xad, 0xed
};

static const unsigned char patchedDigest[32] = {
    0xcd, 0xc9, 0x25, 0xec, 0x0f, 0xfb, 0xb0, 0xd5,
    0x52, 0x45, 0x40, 0x08, 0x6a, 0xea, 0x34, 0x91,
    0x87, 0x51, 0x70, 0xa2, 0x6a, 0xaf, 0xd2, 0x64,
    0x8f, 0x04, 0x7a, 0x20, 0x00, 0xaf, 0x91, 0xae
};

static const unsigned char currentStockBytes[6] = {
    0x0f, 0xb7, 0x05, 0x58, 0x28, 0x01
};
static const unsigned char currentPatchedBytes[6] = {
    0x83, 0x4e, 0x60, 0x04, 0xeb, 0x20
};
static const unsigned char modeStockBytes[10] = {
    0x8b, 0x4c, 0x18, 0x04, 0x51, 0x8b, 0x1c, 0x18, 0x53, 0x8b
};
static const unsigned char modePatchedBytes[10] = {
    0x83, 0x4c, 0x18, 0x60, 0x04, 0x83, 0xc4, 0x14, 0xeb, 0x27
};

#define ROTR32(value, count) \
    (((value) >> (count)) | ((value) << (32U - (count))))

static const U32 sha256Constants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U,
    0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
    0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
    0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
    0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U,
    0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U,
    0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
    0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U
};

static void
sha256Transform(SHA256Context *context, const unsigned char block[64])
{
    U32 words[64];
    U32 a;
    U32 b;
    U32 c;
    U32 d;
    U32 e;
    U32 f;
    U32 g;
    U32 h;
    U32 first;
    U32 second;
    U32 choice;
    U32 majority;
    U32 sigma0;
    U32 sigma1;
    unsigned index;

    for (index = 0; index < 16U; index++) {
        words[index] = ((U32)block[index * 4U] << 24) |
                       ((U32)block[index * 4U + 1U] << 16) |
                       ((U32)block[index * 4U + 2U] << 8) |
                       (U32)block[index * 4U + 3U];
    }
    for (index = 16U; index < 64U; index++) {
        sigma0 = ROTR32(words[index - 15U], 7U) ^
                 ROTR32(words[index - 15U], 18U) ^
                 (words[index - 15U] >> 3);
        sigma1 = ROTR32(words[index - 2U], 17U) ^
                 ROTR32(words[index - 2U], 19U) ^
                 (words[index - 2U] >> 10);
        words[index] = words[index - 16U] + sigma0 +
                       words[index - 7U] + sigma1;
    }

    a = context->state[0];
    b = context->state[1];
    c = context->state[2];
    d = context->state[3];
    e = context->state[4];
    f = context->state[5];
    g = context->state[6];
    h = context->state[7];
    for (index = 0; index < 64U; index++) {
        sigma1 = ROTR32(e, 6U) ^ ROTR32(e, 11U) ^ ROTR32(e, 25U);
        choice = (e & f) ^ ((~e) & g);
        first = h + sigma1 + choice + sha256Constants[index] + words[index];
        sigma0 = ROTR32(a, 2U) ^ ROTR32(a, 13U) ^ ROTR32(a, 22U);
        majority = (a & b) ^ (a & c) ^ (b & c);
        second = sigma0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void
sha256Init(SHA256Context *context)
{
    context->state[0] = 0x6a09e667U;
    context->state[1] = 0xbb67ae85U;
    context->state[2] = 0x3c6ef372U;
    context->state[3] = 0xa54ff53aU;
    context->state[4] = 0x510e527fU;
    context->state[5] = 0x9b05688cU;
    context->state[6] = 0x1f83d9abU;
    context->state[7] = 0x5be0cd19U;
    context->bitCount = 0;
    context->blockLength = 0;
}

static void
sha256Update(SHA256Context *context, const unsigned char *bytes,
             unsigned long length)
{
    unsigned take;

    context->bitCount += (U64)length * 8ULL;
    while (length != 0UL) {
        take = 64U - context->blockLength;
        if ((unsigned long)take > length)
            take = (unsigned)length;
        memcpy(context->block + context->blockLength, bytes, take);
        context->blockLength += take;
        bytes += take;
        length -= take;
        if (context->blockLength == 64U) {
            sha256Transform(context, context->block);
            context->blockLength = 0;
        }
    }
}

static void
sha256Final(SHA256Context *context, unsigned char digest[32])
{
    U64 bitCount;
    unsigned index;

    bitCount = context->bitCount;
    context->block[context->blockLength++] = 0x80;
    if (context->blockLength > 56U) {
        while (context->blockLength < 64U)
            context->block[context->blockLength++] = 0;
        sha256Transform(context, context->block);
        context->blockLength = 0;
    }
    while (context->blockLength < 56U)
        context->block[context->blockLength++] = 0;
    for (index = 0; index < 8U; index++)
        context->block[63U - index] =
            (unsigned char)(bitCount >> (index * 8U));
    sha256Transform(context, context->block);

    for (index = 0; index < 8U; index++) {
        digest[index * 4U] = (unsigned char)(context->state[index] >> 24);
        digest[index * 4U + 1U] =
            (unsigned char)(context->state[index] >> 16);
        digest[index * 4U + 2U] =
            (unsigned char)(context->state[index] >> 8);
        digest[index * 4U + 3U] = (unsigned char)context->state[index];
    }
}

static void
hashBytes(const unsigned char *bytes, unsigned long length,
          unsigned char digest[32])
{
    SHA256Context context;

    sha256Init(&context);
    sha256Update(&context, bytes, length);
    sha256Final(&context, digest);
}

static VBEState
classify(const unsigned char *bytes, unsigned long length,
         unsigned char digest[32])
{
    VBEState state;

    hashBytes(bytes, length, digest);
    if (length != VBE_DRIVER_SIZE)
        return VBE_STATE_UNKNOWN;
    if (memcmp(digest, stockDigest, 32) == 0)
        state = VBE_STATE_STOCK;
    else if (memcmp(digest, patchedDigest, 32) == 0)
        state = VBE_STATE_PATCHED;
    else
        return VBE_STATE_UNKNOWN;

    if (state == VBE_STATE_STOCK &&
        (memcmp(bytes + CURRENT_OFFSET, currentStockBytes,
                sizeof(currentStockBytes)) != 0 ||
         memcmp(bytes + MODE_OFFSET, modeStockBytes,
                sizeof(modeStockBytes)) != 0))
        return VBE_STATE_UNKNOWN;
    if (state == VBE_STATE_PATCHED &&
        (memcmp(bytes + CURRENT_OFFSET, currentPatchedBytes,
                sizeof(currentPatchedBytes)) != 0 ||
         memcmp(bytes + MODE_OFFSET, modePatchedBytes,
                sizeof(modePatchedBytes)) != 0))
        return VBE_STATE_UNKNOWN;
    return state;
}

static const char *
stateName(VBEState state)
{
    if (state == VBE_STATE_STOCK)
        return "stock-write-through";
    if (state == VBE_STATE_PATCHED)
        return "patched-copy-back";
    return "unknown";
}

static void
printDigest(const unsigned char digest[32])
{
    unsigned index;

    for (index = 0; index < 32U; index++)
        printf("%02x", digest[index]);
}

static int
readFile(const char *path, unsigned char **bytes, unsigned long *length,
         struct stat *information, int quietMissing)
{
    struct stat status;
    unsigned char *buffer;
    unsigned long position;
    int descriptor;
    int received;

    if (lstat(path, &status) != 0) {
        if (!quietMissing || errno != ENOENT)
            fprintf(stderr, "%s: cannot inspect: %s\n", path,
                    strerror(errno));
        return 0;
    }
    if (!S_ISREG(status.st_mode) || status.st_size <= 0) {
        fprintf(stderr, "%s: not a nonempty regular file\n", path);
        return 0;
    }
    if ((unsigned long)status.st_size > VBE_MAX_FILE_SIZE) {
        fprintf(stderr, "%s: file is too large\n", path);
        return 0;
    }
    descriptor = open(path, O_RDONLY, 0);
    if (descriptor < 0) {
        fprintf(stderr, "%s: cannot open: %s\n", path, strerror(errno));
        return 0;
    }
    buffer = (unsigned char *)malloc((size_t)status.st_size);
    if (buffer == 0) {
        fprintf(stderr, "%s: out of memory\n", path);
        close(descriptor);
        return 0;
    }
    position = 0;
    while (position < (unsigned long)status.st_size) {
        received = read(descriptor, buffer + position,
                        (unsigned)((unsigned long)status.st_size - position));
        if (received <= 0) {
            fprintf(stderr, "%s: read failed\n", path);
            free(buffer);
            close(descriptor);
            return 0;
        }
        position += (unsigned)received;
    }
    if (close(descriptor) != 0) {
        fprintf(stderr, "%s: close failed\n", path);
        free(buffer);
        return 0;
    }
    *bytes = buffer;
    *length = (unsigned long)status.st_size;
    if (information != 0)
        *information = status;
    return 1;
}

static int
writeAll(int descriptor, const unsigned char *bytes, unsigned long length)
{
    unsigned long position;
    int written;

    position = 0;
    while (position < length) {
        written = write(descriptor, bytes + position,
                        (unsigned)(length - position));
        if (written <= 0)
            return 0;
        position += (unsigned)written;
    }
    return 1;
}

static int
createFile(const char *path, const unsigned char *bytes, unsigned long length,
           const struct stat *information)
{
    int descriptor;
    int succeeded;

    descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL,
                      information->st_mode & 07777);
    if (descriptor < 0) {
        fprintf(stderr, "%s: cannot create safely: %s\n", path,
                strerror(errno));
        return 0;
    }
    succeeded = writeAll(descriptor, bytes, length);
    if (succeeded && fchown(descriptor, information->st_uid,
                            information->st_gid) != 0)
        succeeded = 0;
    if (succeeded && fchmod(descriptor, information->st_mode & 07777) != 0)
        succeeded = 0;
    if (succeeded && fsync(descriptor) != 0)
        succeeded = 0;
    if (close(descriptor) != 0)
        succeeded = 0;
    if (!succeeded) {
        fprintf(stderr, "%s: write or metadata update failed\n", path);
        unlink(path);
        return 0;
    }
    return 1;
}

static int
appendSuffix(char result[PATH_MAX], const char *path, const char *suffix)
{
    if (strlen(path) + strlen(suffix) + 1U > PATH_MAX) {
        fprintf(stderr, "path is too long\n");
        return 0;
    }
    strcpy(result, path);
    strcat(result, suffix);
    return 1;
}

static int
atomicInstall(const char *path, const unsigned char *bytes,
              unsigned long length, const struct stat *information)
{
    char temporary[PATH_MAX];

    if (!appendSuffix(temporary, path, ".framebufferwc-new"))
        return 0;
    if (!createFile(temporary, bytes, length, information))
        return 0;
    if (rename(temporary, path) != 0) {
        fprintf(stderr, "%s: atomic replacement failed: %s\n", path,
                strerror(errno));
        unlink(temporary);
        return 0;
    }
    return 1;
}

static int
inspect(const char *path, unsigned char **bytes, unsigned long *length,
        struct stat *information, VBEState *state,
        unsigned char digest[32])
{
    if (!readFile(path, bytes, length, information, 0))
        return 0;
    *state = classify(*bytes, *length, digest);
    return 1;
}

static void
report(const char *label, const char *path, VBEState state,
       unsigned long length, const unsigned char digest[32])
{
    printf("vbe-cache-patch %s %s path=%s state=%s length=%lu sha256=",
           TOOL_VERSION, label, path, stateName(state), length);
    printDigest(digest);
    printf("\n");
}

static int
loadStockBackup(const char *backupPath, unsigned char **bytes,
                unsigned long *length, struct stat *information)
{
    unsigned char digest[32];
    VBEState state;

    if (!inspect(backupPath, bytes, length, information, &state, digest))
        return 0;
    report("backup", backupPath, state, *length, digest);
    if (state != VBE_STATE_STOCK) {
        fprintf(stderr, "refusing backup that is not the verified stock binary\n");
        free(*bytes);
        *bytes = 0;
        return 0;
    }
    return 1;
}

static int
verifyInstalled(const char *path, VBEState expected)
{
    unsigned char *bytes;
    unsigned char digest[32];
    unsigned long length;
    VBEState state;

    bytes = 0;
    if (!inspect(path, &bytes, &length, 0, &state, digest))
        return 0;
    report("readback", path, state, length, digest);
    free(bytes);
    return state == expected;
}

static int
statusCommand(const char *path)
{
    unsigned char *bytes;
    unsigned char *backupBytes;
    unsigned char digest[32];
    unsigned char backupDigest[32];
    unsigned long length;
    unsigned long backupLength;
    struct stat ignored;
    char backupPath[PATH_MAX];
    VBEState state;
    VBEState backupState;

    bytes = 0;
    if (!inspect(path, &bytes, &length, 0, &state, digest))
        return 1;
    report("driver", path, state, length, digest);
    free(bytes);

    if (!appendSuffix(backupPath, path, ".stock"))
        return 1;
    backupBytes = 0;
    if (readFile(backupPath, &backupBytes, &backupLength, &ignored, 1)) {
        backupState = classify(backupBytes, backupLength, backupDigest);
        report("backup", backupPath, backupState, backupLength, backupDigest);
        free(backupBytes);
    } else if (errno == ENOENT) {
        printf("backup path=%s state=missing\n", backupPath);
    } else {
        return 1;
    }
    return state == VBE_STATE_UNKNOWN ? 1 : 0;
}

static int
patchCommand(const char *path)
{
    unsigned char *bytes;
    unsigned char *backupBytes;
    unsigned char digest[32];
    unsigned char patched[32];
    unsigned long length;
    unsigned long backupLength;
    struct stat information;
    struct stat backupInformation;
    char backupPath[PATH_MAX];
    VBEState state;

    bytes = 0;
    backupBytes = 0;
    if (!inspect(path, &bytes, &length, &information, &state, digest))
        return 1;
    report("driver", path, state, length, digest);
    if (!appendSuffix(backupPath, path, ".stock")) {
        free(bytes);
        return 1;
    }
    if (state == VBE_STATE_PATCHED) {
        if (!loadStockBackup(backupPath, &backupBytes, &backupLength,
                             &backupInformation)) {
            free(bytes);
            return 1;
        }
        free(backupBytes);
        free(bytes);
        printf("already patched; no change\n");
        return 0;
    }
    if (state != VBE_STATE_STOCK) {
        fprintf(stderr, "patch requires the verified stock binary\n");
        free(bytes);
        return 1;
    }

    if (lstat(backupPath, &backupInformation) == 0) {
        if (!loadStockBackup(backupPath, &backupBytes, &backupLength,
                             &backupInformation)) {
            free(bytes);
            return 1;
        }
        free(backupBytes);
    } else if (errno == ENOENT) {
        if (!createFile(backupPath, bytes, length, &information)) {
            free(bytes);
            return 1;
        }
        printf("created verified stock backup %s\n", backupPath);
    } else {
        fprintf(stderr, "%s: cannot inspect backup: %s\n", backupPath,
                strerror(errno));
        free(bytes);
        return 1;
    }

    memcpy(bytes + CURRENT_OFFSET, currentPatchedBytes,
           sizeof(currentPatchedBytes));
    memcpy(bytes + MODE_OFFSET, modePatchedBytes, sizeof(modePatchedBytes));
    hashBytes(bytes, length, patched);
    if (memcmp(patched, patchedDigest, 32) != 0) {
        fprintf(stderr, "internal patched SHA-256 verification failed\n");
        free(bytes);
        return 1;
    }
    if (!atomicInstall(path, bytes, length, &information)) {
        free(bytes);
        return 1;
    }
    free(bytes);
    if (!verifyInstalled(path, VBE_STATE_PATCHED)) {
        fprintf(stderr, "CRITICAL: patched-file readback failed; restore backup\n");
        return 1;
    }
    printf("patch complete; reboot required\n");
    return 0;
}

static int
restoreCommand(const char *path)
{
    unsigned char *bytes;
    unsigned char *backupBytes;
    unsigned char digest[32];
    unsigned long length;
    unsigned long backupLength;
    struct stat information;
    struct stat backupInformation;
    char backupPath[PATH_MAX];
    VBEState state;

    bytes = 0;
    backupBytes = 0;
    if (!inspect(path, &bytes, &length, &information, &state, digest))
        return 1;
    report("driver", path, state, length, digest);
    free(bytes);
    if (state == VBE_STATE_STOCK) {
        printf("already stock; no change\n");
        return 0;
    }
    if (state != VBE_STATE_PATCHED) {
        fprintf(stderr, "restore requires the verified patched binary\n");
        return 1;
    }
    if (!appendSuffix(backupPath, path, ".stock"))
        return 1;
    if (!loadStockBackup(backupPath, &backupBytes, &backupLength,
                         &backupInformation))
        return 1;
    if (!atomicInstall(path, backupBytes, backupLength, &backupInformation)) {
        free(backupBytes);
        return 1;
    }
    free(backupBytes);
    if (!verifyInstalled(path, VBE_STATE_STOCK)) {
        fprintf(stderr, "CRITICAL: restored-file readback failed\n");
        return 1;
    }
    printf("restore complete; reboot required\n");
    return 0;
}

static int
usage(const char *program)
{
    fprintf(stderr, "usage: %s status|patch|restore [VBE-driver-binary]\n",
            program);
    return 2;
}

int
main(int argc, char **argv)
{
    const char *path;

    if (argc < 2 || argc > 3)
        return usage(argv[0]);
    path = argc == 3 ? argv[2] : VBE_DRIVER_PATH;
    if (strcmp(argv[1], "status") == 0)
        return statusCommand(path);
    if (strcmp(argv[1], "patch") != 0 && strcmp(argv[1], "restore") != 0)
        return usage(argv[0]);
    if (geteuid() != 0) {
        fprintf(stderr, "%s requires root\n", argv[1]);
        return 1;
    }
    if (strcmp(argv[1], "patch") == 0)
        return patchCommand(path);
    return restoreCommand(path);
}
