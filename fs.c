#include "fs.h"
#include "utils.h"

fs_node_t *fs_root = NULL;

uint32_t read_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node != NULL && node->read != NULL)
        return node->read(node, offset, size, buffer);
    else
        return 0;
}

uint32_t write_fs(fs_node_t *node, uint32_t offset, uint32_t size, uint8_t *buffer) {
    if (node != NULL && node->write != NULL)
        return node->write(node, offset, size, buffer);
    else
        return 0;
}

void open_fs(fs_node_t *node) {
    if (node != NULL && node->open != NULL)
        node->open(node);
}

void close_fs(fs_node_t *node) {
    if (node != NULL && node->close != NULL)
        node->close(node);
}

dirent_t *readdir_fs(fs_node_t *node, uint32_t index) {
    if (node != NULL && node->readdir != NULL)
        return node->readdir(node, index);
    else
        return NULL;
}

fs_node_t *finddir_fs(fs_node_t *node, const char *name) {
    if (node != NULL && node->finddir != NULL)
        return node->finddir(node, name);
    else
        return NULL;
}

static int parse_component(const char **cursor, char *component) {
    size_t length = 0;

    while (**cursor && **cursor != '/') {
        if (length + 1 >= sizeof(((fs_node_t *)0)->name)) return 0;
        component[length++] = **cursor;
        (*cursor)++;
    }
    component[length] = '\0';
    return length > 0 && strcmp(component, ".") != 0 &&
           strcmp(component, "..") != 0;
}

static fs_node_t *resolve_parent_fs(const char *path, char *name) {
    const char *cursor = path;
    fs_node_t *parent = fs_root;
    char component[128];
    size_t length;

    if (!path || !fs_root) return NULL;

    length = strlen(path);
    if (length == 0 || length >= FS_MAX_PATH) return NULL;
    if (*cursor == '/') cursor++;
    if (*cursor == '\0') return NULL;

    while (*cursor) {
        if (!parse_component(&cursor, component)) return NULL;

        if (*cursor == '\0') {
            strcpy(name, component);
            return parent;
        }

        cursor++;
        if (*cursor == '\0' || *cursor == '/') return NULL;

        parent = finddir_fs(parent, component);
        if (!parent || parent->flags != FS_DIRECTORY) return NULL;
    }
    return NULL;
}

fs_node_t *resolve_fs(const char *path) {
    const char *cursor = path;
    fs_node_t *node = fs_root;
    char component[128];

    if (!path || !fs_root || strlen(path) >= FS_MAX_PATH) return NULL;
    if (*cursor == '/') cursor++;
    if (*cursor == '\0') return node;

    while (*cursor) {
        if (!parse_component(&cursor, component)) return NULL;

        node = finddir_fs(node, component);
        if (!node) return NULL;

        if (*cursor == '/') {
            cursor++;
            if (*cursor == '\0' || *cursor == '/') return NULL;
            /* More path to walk, so this node has to be a directory. Relying
             * on "a file has no finddir, so the next lookup returns NULL"
             * would make correctness a property of every backend rather than
             * of this resolver -- and procfs's finddir ignores the node it is
             * given entirely. resolve_parent_fs() has always checked this;
             * without it the same path means different things depending on
             * which entry point the caller used. */
            if (node->flags != FS_DIRECTORY) return NULL;
        }
    }
    return node;
}

fs_node_t *create_fs(const char *path) {
    char name[128];
    fs_node_t *parent = resolve_parent_fs(path, name);

    if (!parent || !parent->create) return NULL;
    return parent->create(parent, name);
}

int unlink_fs(const char *path) {
    char name[128];
    fs_node_t *parent = resolve_parent_fs(path, name);

    if (!parent || !parent->unlink) return -1;
    return parent->unlink(parent, name);
}

int mkdir_fs(const char *path) {
    char name[128];
    fs_node_t *parent = resolve_parent_fs(path, name);

    if (!parent || !parent->mkdir) return -1;
    return parent->mkdir(parent, name);
}

int rmdir_fs(const char *path) {
    char name[128];
    fs_node_t *parent = resolve_parent_fs(path, name);

    if (!parent || !parent->rmdir) return -1;
    return parent->rmdir(parent, name);
}

/* Append `comp` (length clen) as a new path component in `out`.
 * Returns 0, or -1 if the component would not fit. Overflow MUST be reported
 * rather than silently skipped: dropping a component leaves a well-formed but
 * *different* path (an ancestor directory), and the caller would then operate
 * on the wrong filesystem object -- e.g. rmdir("sub") under a near-limit cwd
 * would resolve to the cwd itself. Callers turn this into a clean failure. */
static int path_push(char *out, unsigned *olen, const char *comp,
                     unsigned clen) {
    if (*olen + 1 + clen + 1 > FS_MAX_PATH) return -1;
    out[(*olen)++] = '/';
    for (unsigned i = 0; i < clen; i++) out[(*olen)++] = comp[i];
    out[*olen] = '\0';
    return 0;
}

/* Drop the last path component from `out` (handles "..", stops at root). */
static void path_pop(char *out, unsigned *olen) {
    while (*olen > 0 && out[*olen - 1] != '/') (*olen)--;
    if (*olen > 0) (*olen)--;   /* remove the '/' */
    out[*olen] = '\0';
}

/* Walk the components of `s`, applying '.', '..' and names onto `out`.
 * Returns 0, or -1 if any component overflowed the output buffer. */
static int path_apply(char *out, unsigned *olen, const char *s) {
    unsigned i = 0;

    while (s[i]) {
        unsigned start, clen;

        if (s[i] == '/') { i++; continue; }
        start = i;
        while (s[i] && s[i] != '/') i++;
        clen = i - start;

        if (clen == 1 && s[start] == '.') continue;
        if (clen == 2 && s[start] == '.' && s[start + 1] == '.') {
            path_pop(out, olen);
            continue;
        }
        if (path_push(out, olen, s + start, clen) != 0) return -1;
    }
    return 0;
}

int vfs_resolve_path(const char *cwd, const char *path, char *out) {
    unsigned olen = 0;

    if (!path || !out) return -1;

    /* Reject empty path components ("//"), matching the strict VFS resolver. */
    for (unsigned i = 0; path[i]; i++) {
        if (path[i] == '/' && path[i + 1] == '/') return -1;
    }

    out[0] = '\0';

    /* Relative paths start from the working directory. A path that does not
     * fit is rejected outright (see path_push) rather than silently resolving
     * to a truncated, different path. */
    if (path[0] != '/' && cwd && cwd[0]) {
        if (path_apply(out, &olen, cwd) != 0) return -1;
    }
    if (path_apply(out, &olen, path) != 0) return -1;

    if (olen == 0) { out[0] = '/'; out[1] = '\0'; }  /* normalised root */
    if (olen + 1 >= FS_MAX_PATH) return -1;
    return 0;
}
