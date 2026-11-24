#define _DEFAULT_SOURCE
#include "rule_secrets.h"

#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static const char* const secret_keys[] = {
    "password", "remote_pass", "pass", "token", "bot_token", "webhook_url", NULL
};

static const char* const step_lists[] = { "triggers", "conditions", "actions", NULL };

static int is_secret_key(const char* key) {
    for (int i = 0; secret_keys[i]; i++)
        if (strcmp(key, secret_keys[i]) == 0) return 1;
    return 0;
}

static int contains_nocase(const char* s, size_t len, const char* needle) {
    size_t nlen = strlen(needle);
    for (size_t i = 0; i + nlen <= len; i++)
        if (strncasecmp(s + i, needle, nlen) == 0) return 1;
    return 0;
}

/* Header names whose values are credentials: Authorization, Cookie and
 * custom ones such as X-API-Key or X-Auth-Token. */
static int is_sensitive_header(const char* name, size_t len) {
    static const char* const parts[] = {
        "auth", "cookie", "token", "secret", "key", "password", "session", "signature", NULL
    };
    for (int i = 0; parts[i]; i++)
        if (contains_nocase(name, len, parts[i])) return 1;
    return 0;
}

/* One "Name: value" line split into trimmed name and value. */
typedef struct {
    const char* name;  size_t name_len;
    const char* value; size_t value_len;
} HeaderLine;

static int parse_header(const char* line, size_t len, HeaderLine* h) {
    const char* colon = memchr(line, ':', len);
    if (!colon) return 0;
    const char* ns = line;
    const char* ne = colon;
    while (ns < ne && isspace((unsigned char)*ns)) ns++;
    while (ne > ns && isspace((unsigned char)ne[-1])) ne--;
    const char* vs = colon + 1;
    const char* ve = line + len;
    while (vs < ve && isspace((unsigned char)*vs)) vs++;
    while (ve > vs && isspace((unsigned char)ve[-1])) ve--;
    h->name = ns;  h->name_len = (size_t)(ne - ns);
    h->value = vs; h->value_len = (size_t)(ve - vs);
    return h->name_len > 0;
}

static int value_is_mask(const HeaderLine* h) {
    return h->value_len == strlen(RULE_SECRET_MASK) &&
           strncmp(h->value, RULE_SECRET_MASK, h->value_len) == 0;
}

/* Find the value of header name in a stored headers string. */
static int find_header(const char* headers, const HeaderLine* want, HeaderLine* found) {
    if (!headers) return 0;
    const char* p = headers;
    while (*p) {
        const char* nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        HeaderLine h;
        if (parse_header(p, len, &h) && h.name_len == want->name_len &&
            strncasecmp(h.name, want->name, h.name_len) == 0 &&
            h.value_len > 0 && !value_is_mask(&h)) {
            *found = h;
            return 1;
        }
        if (!nl) break;
        p = nl + 1;
    }
    return 0;
}

static void append(char** out, size_t* len, size_t* cap, const char* s, size_t n) {
    if (*len + n + 1 > *cap) {
        size_t ncap = (*cap ? *cap * 2 : 128);
        while (ncap < *len + n + 1) ncap *= 2;
        char* grown = realloc(*out, ncap);
        if (!grown) return;
        *out = grown;
        *cap = ncap;
    }
    memcpy(*out + *len, s, n);
    *len += n;
    (*out)[*len] = '\0';
}

static void add_path(cJSON* list, const char* path) {
    if (list) cJSON_AddItemToArray(list, cJSON_CreateString(path));
}

/* Rebuild a headers string: mask sensitive values, or with restoring set,
 * replace masked values with the same header from stored and drop the lines
 * that can't be restored. Returns a new string, or NULL when nothing changed. */
static char* rewrite_headers(const char* headers, int restoring, const char* stored,
                             const char* path, cJSON* missing, int* count) {
    char* out = NULL;
    size_t len = 0, cap = 0;
    int changed = 0, first = 1;
    const char* p = headers;
    for (;;) {
        const char* nl = strchr(p, '\n');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);
        HeaderLine h, s;
        int parsed = parse_header(p, line_len, &h);
        const char* name = NULL; size_t name_len = 0;
        const char* value = p;   size_t value_len = line_len;
        int emit = 1, whole_line = 1;

        if (parsed && !restoring && h.value_len > 0 && !value_is_mask(&h) &&
            is_sensitive_header(h.name, h.name_len)) {
            name = h.name; name_len = h.name_len;
            value = RULE_SECRET_MASK; value_len = strlen(RULE_SECRET_MASK);
            whole_line = 0;
            (*count)++;
        } else if (parsed && restoring && value_is_mask(&h)) {
            if (find_header(stored, &h, &s)) {
                name = h.name; name_len = h.name_len;
                value = s.value; value_len = s.value_len;
                whole_line = 0;
            } else {
                char hp[512];
                snprintf(hp, sizeof(hp), "%s.headers[%.*s]", path, (int)h.name_len, h.name);
                add_path(missing, hp);
                (*count)++;
                emit = 0;
            }
        }

        if (!whole_line || !emit) changed = 1;
        if (emit) {
            if (!first) append(&out, &len, &cap, "\n", 1);
            first = 0;
            if (whole_line) {
                append(&out, &len, &cap, p, line_len);
            } else {
                append(&out, &len, &cap, name, name_len);
                append(&out, &len, &cap, ": ", 2);
                append(&out, &len, &cap, value, value_len);
            }
        }
        if (!nl) break;
        p = nl + 1;
    }
    if (!changed) { free(out); return NULL; }
    if (!out) out = calloc(1, 1);
    return out;
}

static int mask_node(cJSON* node) {
    int n = 0;
    cJSON* it;
    cJSON_ArrayForEach(it, node) {
        if (cJSON_IsString(it) && it->string && it->valuestring && it->valuestring[0]) {
            if (strcmp(it->valuestring, RULE_SECRET_MASK) == 0) continue;
            if (is_secret_key(it->string)) {
                cJSON_SetValuestring(it, RULE_SECRET_MASK);
                n++;
            } else if (strcmp(it->string, "headers") == 0) {
                char* masked = rewrite_headers(it->valuestring, 0, NULL, NULL, NULL, &n);
                if (masked) { cJSON_SetValuestring(it, masked); free(masked); }
            }
        } else if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
            n += mask_node(it);
        }
    }
    return n;
}

int RuleSecrets_Mask(cJSON* rule) {
    if (!rule) return 0;
    int n = 0;
    for (int l = 0; step_lists[l]; l++) {
        cJSON* list = cJSON_GetObjectItem(rule, step_lists[l]);
        if (cJSON_IsArray(list)) n += mask_node(list);
    }
    return n;
}

static int restore_node(cJSON* node, const cJSON* stored, const char* path, cJSON* missing) {
    int unresolved = 0;
    int index = 0;
    cJSON* it = node->child;
    while (it) {
        cJSON* next = it->next;
        char child_path[512];
        const cJSON* stored_child = NULL;
        if (cJSON_IsArray(node)) {
            snprintf(child_path, sizeof(child_path), "%s[%d]", path, index);
            if (cJSON_IsArray(stored)) stored_child = cJSON_GetArrayItem(stored, index);
        } else {
            snprintf(child_path, sizeof(child_path), "%s.%s", path, it->string ? it->string : "");
            if (cJSON_IsObject(stored) && it->string) stored_child = cJSON_GetObjectItem(stored, it->string);
        }

        if (cJSON_IsString(it) && it->valuestring) {
            const char* stored_val = cJSON_GetStringValue(stored_child);
            if (strcmp(it->valuestring, RULE_SECRET_MASK) == 0) {
                if (stored_val && stored_val[0] && strcmp(stored_val, RULE_SECRET_MASK) != 0) {
                    cJSON_SetValuestring(it, stored_val);
                } else {
                    add_path(missing, child_path);
                    cJSON_DetachItemViaPointer(node, it);
                    cJSON_Delete(it);
                    unresolved++;
                }
            } else if (it->string && strcmp(it->string, "headers") == 0 &&
                       strstr(it->valuestring, RULE_SECRET_MASK)) {
                char* restored = rewrite_headers(it->valuestring, 1, stored_val, path, missing, &unresolved);
                if (restored) { cJSON_SetValuestring(it, restored); free(restored); }
            }
        } else if (cJSON_IsObject(it) || cJSON_IsArray(it)) {
            unresolved += restore_node(it, stored_child, child_path, missing);
        }
        it = next;
        index++;
    }
    return unresolved;
}

static void random_token(char* out, size_t size) {
    static const char hex[] = "0123456789abcdef";
    unsigned char bytes[16];
    int ok = 0;
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd >= 0) {
        ok = read(fd, bytes, sizeof(bytes)) == (ssize_t)sizeof(bytes);
        close(fd);
    }
    if (!ok) {
        srand((unsigned)time(NULL) ^ (unsigned)getpid());
        for (size_t i = 0; i < sizeof(bytes); i++) bytes[i] = (unsigned char)rand();
    }
    size_t o = 0;
    for (size_t i = 0; i < sizeof(bytes) && o + 2 < size; i++) {
        out[o++] = hex[bytes[i] >> 4];
        out[o++] = hex[bytes[i] & 0x0f];
    }
    out[o] = '\0';
}

int RuleSecrets_Restore(cJSON* rule, const cJSON* stored, cJSON* missing, cJSON* regenerated) {
    if (!rule) return 0;
    int unresolved = 0;
    for (int l = 0; step_lists[l]; l++) {
        cJSON* list = cJSON_GetObjectItem(rule, step_lists[l]);
        if (!cJSON_IsArray(list)) continue;
        const cJSON* stored_list = stored ? cJSON_GetObjectItem(stored, step_lists[l]) : NULL;
        int i = 0;
        cJSON* step;
        cJSON_ArrayForEach(step, list) {
            char path[64];
            snprintf(path, sizeof(path), "%s[%d]", step_lists[l], i);
            const char* type = cJSON_GetStringValue(cJSON_GetObjectItem(step, "type"));
            const cJSON* stored_step = cJSON_IsArray(stored_list) ? cJSON_GetArrayItem(stored_list, i) : NULL;
            const char* stored_type = cJSON_GetStringValue(cJSON_GetObjectItem(stored_step, "type"));
            if (!type || !stored_type || strcmp(type, stored_type) != 0) stored_step = NULL;

            int had_token_mask = 0;
            const char* tok = cJSON_GetStringValue(cJSON_GetObjectItem(step, "token"));
            if (tok && strcmp(tok, RULE_SECRET_MASK) == 0) had_token_mask = 1;

            cJSON* step_missing = cJSON_CreateArray();
            unresolved += restore_node(step, stored_step, path, step_missing);

            int is_webhook = type && strcmp(type, "http_webhook") == 0;
            if (is_webhook && had_token_mask && !cJSON_GetObjectItem(step, "token")) {
                char token[33];
                random_token(token, sizeof(token));
                cJSON_AddStringToObject(step, "token", token);
                char tp[96];
                snprintf(tp, sizeof(tp), "%s.token", path);
                add_path(regenerated, tp);
                unresolved--;
                /* Reported as regenerated, not missing. */
                cJSON* m;
                int mi = 0;
                cJSON_ArrayForEach(m, step_missing) {
                    if (strcmp(cJSON_GetStringValue(m), tp) == 0) {
                        cJSON_DeleteItemFromArray(step_missing, mi);
                        break;
                    }
                    mi++;
                }
            }
            cJSON* m;
            cJSON_ArrayForEach(m, step_missing) add_path(missing, cJSON_GetStringValue(m));
            cJSON_Delete(step_missing);
            i++;
        }
    }
    return unresolved;
}
