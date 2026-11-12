#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <syslog.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include "certs.h"

#define LOG(fmt, args...)      syslog(LOG_INFO,    "certs: " fmt, ## args)
#define LOG_WARN(fmt, args...) syslog(LOG_WARNING, "certs: " fmt, ## args)

#define INDEX_FILE "profiles.json"
#define MAX_PEM    (48 * 1024)

typedef struct {
    char   name[CERTS_NAME_MAX + 1];
    int    insecure;
    double updated;
    cJSON* ca_info;    /* array of certificate summaries, NULL when absent */
    cJSON* cert_info;  /* array, client certificate first then its chain */
    cJSON* key_info;   /* { type, bits } */
} Profile;

static Profile         profiles[CERTS_MAX_PROFILES];
static int             profile_count = 0;
static char            store_dir[400] = "";
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

/* ---------- helpers ---------- */

static void set_err(char* err, size_t n, const char* fmt, ...) __attribute__((format(__printf__, 3, 4)));
static void set_err(char* err, size_t n, const char* fmt, ...) {
    if (!err || !n) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

static void openssl_reason(char* buf, size_t n) {
    unsigned long e = ERR_peek_last_error();
    buf[0] = '\0';
    if (e) {
        const char* r = ERR_reason_error_string(e);
        snprintf(buf, n, "%s", r ? r : "");
    }
    ERR_clear_error();
}

static int valid_name(const char* name) {
    if (!name) return 0;
    size_t len = strlen(name);
    if (len == 0 || len > CERTS_NAME_MAX) return 0;
    if (!isalnum((unsigned char)name[0]) || name[len - 1] == ' ') return 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (!isalnum(c) && c != '-' && c != '_' && c != '.' && c != ' ') return 0;
    }
    return 1;
}

static Profile* find(const char* name) {
    if (!name) return NULL;
    for (int i = 0; i < profile_count; i++)
        if (strcmp(profiles[i].name, name) == 0) return &profiles[i];
    return NULL;
}

static void profile_dir(const char* name, char* out, size_t n) {
    snprintf(out, n, "%s/%s", store_dir, name);
}

static void profile_file(const char* name, const char* file, char* out, size_t n) {
    snprintf(out, n, "%s/%s/%s", store_dir, name, file);
}

static int write_file_secure(const char* path, const char* data, size_t len) {
    char tmp[600];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return 0;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            close(fd);
            unlink(tmp);
            return 0;
        }
        off += (size_t)w;
    }
    fsync(fd);
    close(fd);
    if (rename(tmp, path) != 0) { unlink(tmp); return 0; }
    return 1;
}

static char* read_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 0 || sz > 4 * MAX_PEM) { fclose(f); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[r] = '\0';
    return buf;
}

static double asn1_epoch(const ASN1_TIME* t) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    if (!t || ASN1_TIME_to_tm(t, &tm) != 1) return 0;
    return (double)timegm(&tm);
}

static void name_rfc2253(X509_NAME* nm, char* out, size_t n) {
    out[0] = '\0';
    BIO* b = BIO_new(BIO_s_mem());
    if (!b) return;
    X509_NAME_print_ex(b, nm, 0, XN_FLAG_RFC2253);
    int len = BIO_read(b, out, (int)n - 1);
    out[len > 0 ? len : 0] = '\0';
    BIO_free(b);
}

static cJSON* cert_summary(X509* x) {
    cJSON* o = cJSON_CreateObject();
    char buf[512];

    name_rfc2253(X509_get_subject_name(x), buf, sizeof(buf));
    cJSON_AddStringToObject(o, "subject", buf);
    buf[0] = '\0';
    X509_NAME_get_text_by_NID(X509_get_subject_name(x), NID_commonName, buf, sizeof(buf));
    cJSON_AddStringToObject(o, "cn", buf);
    name_rfc2253(X509_get_issuer_name(x), buf, sizeof(buf));
    cJSON_AddStringToObject(o, "issuer", buf);
    cJSON_AddNumberToObject(o, "not_before", asn1_epoch(X509_get0_notBefore(x)));
    cJSON_AddNumberToObject(o, "not_after",  asn1_epoch(X509_get0_notAfter(x)));
    cJSON_AddBoolToObject(o, "is_ca", X509_check_ca(x) > 0);

    unsigned char md[EVP_MAX_MD_SIZE];
    unsigned int mdlen = 0;
    if (X509_digest(x, EVP_sha256(), md, &mdlen) == 1) {
        char hex[EVP_MAX_MD_SIZE * 3 + 1];
        size_t p = 0;
        for (unsigned int i = 0; i < mdlen; i++)
            p += (size_t)snprintf(hex + p, sizeof(hex) - p, i ? ":%02X" : "%02X", md[i]);
        cJSON_AddStringToObject(o, "sha256", hex);
    }

    cJSON* san = cJSON_AddArrayToObject(o, "san");
    GENERAL_NAMES* gens = X509_get_ext_d2i(x, NID_subject_alt_name, NULL, NULL);
    if (gens) {
        for (int i = 0; i < sk_GENERAL_NAME_num(gens) && i < 16; i++) {
            GENERAL_NAME* g = sk_GENERAL_NAME_value(gens, i);
            if (g->type == GEN_DNS) {
                const unsigned char* s = ASN1_STRING_get0_data(g->d.dNSName);
                cJSON_AddItemToArray(san, cJSON_CreateString((const char*)s));
            } else if (g->type == GEN_IPADD) {
                const unsigned char* ip = ASN1_STRING_get0_data(g->d.iPAddress);
                int iplen = ASN1_STRING_length(g->d.iPAddress);
                if (iplen == 4) {
                    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
                    cJSON_AddItemToArray(san, cJSON_CreateString(buf));
                } else if (iplen == 16) {
                    size_t p = 0;
                    for (int k = 0; k < 16; k += 2)
                        p += (size_t)snprintf(buf + p, sizeof(buf) - p, k ? ":%x" : "%x", (ip[k] << 8) | ip[k + 1]);
                    cJSON_AddItemToArray(san, cJSON_CreateString(buf));
                }
            }
        }
        GENERAL_NAMES_free(gens);
    }
    return o;
}

static cJSON* key_summary(EVP_PKEY* k) {
    cJSON* o = cJSON_CreateObject();
    const char* type;
    switch (EVP_PKEY_base_id(k)) {
        case EVP_PKEY_RSA:     type = "RSA"; break;
        case EVP_PKEY_EC:      type = "EC"; break;
        case EVP_PKEY_ED25519: type = "Ed25519"; break;
        case EVP_PKEY_ED448:   type = "Ed448"; break;
        default:               type = "other"; break;
    }
    cJSON_AddStringToObject(o, "type", type);
    cJSON_AddNumberToObject(o, "bits", EVP_PKEY_bits(k));
    return o;
}

/* All certificates in a PEM text, in order. Returns NULL when there are none. */
static STACK_OF(X509)* parse_certs(const char* pem) {
    BIO* b = BIO_new_mem_buf(pem, -1);
    if (!b) return NULL;
    STACK_OF(X509)* sk = sk_X509_new_null();
    X509* x;
    while ((x = PEM_read_bio_X509(b, NULL, NULL, NULL)) != NULL)
        sk_X509_push(sk, x);
    ERR_clear_error();
    BIO_free(b);
    if (sk_X509_num(sk) == 0) { sk_X509_free(sk); return NULL; }
    return sk;
}

typedef struct {
    const char* pass;
    int asked;
} PassCtx;

static int pass_cb(char* buf, int size, int rwflag, void* u) {
    (void)rwflag;
    PassCtx* pc = u;
    pc->asked = 1;
    if (!pc->pass || !pc->pass[0]) return -1;
    int len = (int)strlen(pc->pass);
    if (len >= size) return -1;
    memcpy(buf, pc->pass, (size_t)len);
    return len;
}

/* First private key in a PEM text. *asked tells whether it was encrypted. */
static EVP_PKEY* parse_key(const char* pem, const char* pass, int* asked) {
    BIO* b = BIO_new_mem_buf(pem, -1);
    if (!b) return NULL;
    PassCtx pc = { pass, 0 };
    EVP_PKEY* k = PEM_read_bio_PrivateKey(b, NULL, pass_cb, &pc);
    ERR_clear_error();
    BIO_free(b);
    if (asked) *asked = pc.asked;
    return k;
}

static cJSON* summarize_stack(STACK_OF(X509)* sk) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < sk_X509_num(sk); i++)
        cJSON_AddItemToArray(arr, cert_summary(sk_X509_value(sk, i)));
    return arr;
}

static char* stack_to_pem(STACK_OF(X509)* sk, size_t* len) {
    BIO* b = BIO_new(BIO_s_mem());
    for (int i = 0; i < sk_X509_num(sk); i++)
        PEM_write_bio_X509(b, sk_X509_value(sk, i));
    char* data = NULL;
    long n = BIO_get_mem_data(b, &data);
    char* out = malloc((size_t)n + 1);
    if (out) { memcpy(out, data, (size_t)n); out[n] = '\0'; *len = (size_t)n; }
    BIO_free(b);
    return out;
}

static char* key_to_pem(EVP_PKEY* k, size_t* len) {
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL);
    char* data = NULL;
    long n = BIO_get_mem_data(b, &data);
    char* out = malloc((size_t)n + 1);
    if (out) { memcpy(out, data, (size_t)n); out[n] = '\0'; *len = (size_t)n; }
    if (n > 0) OPENSSL_cleanse(data, (size_t)n);
    BIO_free(b);
    return out;
}

static void profile_free(Profile* p) {
    cJSON_Delete(p->ca_info);
    cJSON_Delete(p->cert_info);
    cJSON_Delete(p->key_info);
    p->ca_info = p->cert_info = p->key_info = NULL;
}

static void load_profile_files(Profile* p) {
    char path[600];
    profile_free(p);

    profile_file(p->name, "ca.pem", path, sizeof(path));
    char* pem = read_file(path);
    if (pem) {
        STACK_OF(X509)* sk = parse_certs(pem);
        if (sk) { p->ca_info = summarize_stack(sk); sk_X509_pop_free(sk, X509_free); }
        free(pem);
    }
    profile_file(p->name, "cert.pem", path, sizeof(path));
    pem = read_file(path);
    if (pem) {
        STACK_OF(X509)* sk = parse_certs(pem);
        if (sk) { p->cert_info = summarize_stack(sk); sk_X509_pop_free(sk, X509_free); }
        free(pem);
    }
    profile_file(p->name, "key.pem", path, sizeof(path));
    pem = read_file(path);
    if (pem) {
        EVP_PKEY* k = parse_key(pem, NULL, NULL);
        if (k) { p->key_info = key_summary(k); EVP_PKEY_free(k); }
        OPENSSL_cleanse(pem, strlen(pem));
        free(pem);
    }
}

static int write_index(void) {
    cJSON* arr = cJSON_CreateArray();
    for (int i = 0; i < profile_count; i++) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", profiles[i].name);
        cJSON_AddBoolToObject(o, "insecure", profiles[i].insecure);
        cJSON_AddNumberToObject(o, "updated", profiles[i].updated);
        cJSON_AddItemToArray(arr, o);
    }
    char* txt = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    if (!txt) return 0;
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", store_dir, INDEX_FILE);
    int ok = write_file_secure(path, txt, strlen(txt));
    if (!ok) LOG_WARN("could not write %s: %s", path, strerror(errno));
    free(txt);
    return ok;
}

static cJSON* profile_json(const Profile* p) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", p->name);
    cJSON_AddBoolToObject(o, "insecure", p->insecure);
    cJSON_AddNumberToObject(o, "updated", p->updated);
    cJSON_AddItemToObject(o, "ca", p->ca_info ? cJSON_Duplicate(p->ca_info, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(o, "cert", p->cert_info ? cJSON_Duplicate(p->cert_info, 1) : cJSON_CreateNull());
    cJSON_AddItemToObject(o, "key", p->key_info ? cJSON_Duplicate(p->key_info, 1) : cJSON_CreateNull());

    /* Earliest expiry among the client certificate and the CAs, for the UI badge */
    double expires = 0;
    cJSON* leaf = p->cert_info ? cJSON_GetArrayItem(p->cert_info, 0) : NULL;
    if (leaf) expires = cJSON_GetObjectItem(leaf, "not_after")->valuedouble;
    cJSON* c;
    cJSON_ArrayForEach(c, p->ca_info) {
        double na = cJSON_GetObjectItem(c, "not_after")->valuedouble;
        if (na > 0 && (expires == 0 || na < expires)) expires = na;
    }
    if (expires > 0) cJSON_AddNumberToObject(o, "expires", expires);
    else cJSON_AddNullToObject(o, "expires");
    return o;
}

/* ---------- public API ---------- */

int Certs_Init(const char* dir) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < profile_count; i++) profile_free(&profiles[i]);
    profile_count = 0;
    snprintf(store_dir, sizeof(store_dir), "%s", dir ? dir : "");
    if (!store_dir[0]) { pthread_mutex_unlock(&lock); return 0; }
    if (mkdir(store_dir, 0700) != 0 && errno != EEXIST) {
        LOG_WARN("cannot create %s: %s", store_dir, strerror(errno));
        pthread_mutex_unlock(&lock);
        return 0;
    }

    char path[600];
    snprintf(path, sizeof(path), "%s/%s", store_dir, INDEX_FILE);
    char* txt = read_file(path);
    cJSON* arr = txt ? cJSON_Parse(txt) : NULL;
    free(txt);
    cJSON* it;
    cJSON_ArrayForEach(it, arr) {
        const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(it, "name"));
        if (!valid_name(name) || find(name) || profile_count >= CERTS_MAX_PROFILES) continue;
        Profile* p = &profiles[profile_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", name);
        p->insecure = cJSON_IsTrue(cJSON_GetObjectItem(it, "insecure"));
        cJSON* up = cJSON_GetObjectItem(it, "updated");
        p->updated = cJSON_IsNumber(up) ? up->valuedouble : 0;
        load_profile_files(p);
    }
    cJSON_Delete(arr);
    if (profile_count) LOG("loaded %d TLS profile(s)", profile_count);
    pthread_mutex_unlock(&lock);
    return 1;
}

void Certs_Cleanup(void) {
    pthread_mutex_lock(&lock);
    for (int i = 0; i < profile_count; i++) profile_free(&profiles[i]);
    profile_count = 0;
    pthread_mutex_unlock(&lock);
}

cJSON* Certs_List(void) {
    cJSON* arr = cJSON_CreateArray();
    pthread_mutex_lock(&lock);
    for (int i = 0; i < profile_count; i++)
        cJSON_AddItemToArray(arr, profile_json(&profiles[i]));
    pthread_mutex_unlock(&lock);
    return arr;
}

cJSON* Certs_Get(const char* name) {
    pthread_mutex_lock(&lock);
    Profile* p = find(name);
    cJSON* o = p ? profile_json(p) : NULL;
    pthread_mutex_unlock(&lock);
    return o;
}

int Certs_Exists(const char* name) {
    pthread_mutex_lock(&lock);
    int r = find(name) != NULL;
    pthread_mutex_unlock(&lock);
    return r;
}

/* A field is "omitted" when absent or null, "cleared" when an empty string. */
static int field_state(cJSON* body, const char* key, const char** text) {
    cJSON* f = cJSON_GetObjectItem(body, key);
    *text = NULL;
    if (!f || cJSON_IsNull(f)) return 0;
    if (!cJSON_IsString(f)) return -1;
    *text = f->valuestring;
    return f->valuestring[0] ? 2 : 1;
}

int Certs_Save(cJSON* body, char* err, size_t err_size) {
    const char* name = cJSON_GetStringValue(cJSON_GetObjectItem(body, "name"));
    if (!valid_name(name)) {
        set_err(err, err_size, "Name must be 1-%d characters: letters, digits, space, '-', '_' or '.', starting with a letter or digit", CERTS_NAME_MAX);
        return 0;
    }
    cJSON* insecure_j = cJSON_GetObjectItem(body, "insecure");
    if (insecure_j && !cJSON_IsBool(insecure_j)) {
        set_err(err, err_size, "'insecure' must be true or false");
        return 0;
    }

    const char *ca_txt, *cert_txt, *key_txt;
    int ca_st = field_state(body, "ca", &ca_txt);
    int cert_st = field_state(body, "cert", &cert_txt);
    int key_st = field_state(body, "key", &key_txt);
    if (ca_st < 0 || cert_st < 0 || key_st < 0) {
        set_err(err, err_size, "'ca', 'cert' and 'key' must be PEM text");
        return 0;
    }
    if ((ca_txt && strlen(ca_txt) > MAX_PEM) || (cert_txt && strlen(cert_txt) > MAX_PEM) ||
        (key_txt && strlen(key_txt) > MAX_PEM)) {
        set_err(err, err_size, "Each certificate or key may be at most %d KB", MAX_PEM / 1024);
        return 0;
    }
    const char* pass = cJSON_GetStringValue(cJSON_GetObjectItem(body, "passphrase"));

    int ok = 0;
    STACK_OF(X509)* ca_sk = NULL;
    STACK_OF(X509)* cert_sk = NULL;
    EVP_PKEY* key = NULL;
    char* pem = NULL;

    pthread_mutex_lock(&lock);
    Profile* existing = find(name);
    if (!existing && profile_count >= CERTS_MAX_PROFILES) {
        set_err(err, err_size, "At most %d TLS profiles can be stored", CERTS_MAX_PROFILES);
        goto out;
    }

    /* Resolve each part: new text, cleared, or kept from the stored files. */
    char path[600];
    if (ca_st == 2) {
        ca_sk = parse_certs(ca_txt);
        if (!ca_sk) { set_err(err, err_size, "No certificate found in the CA field. Expected PEM text starting with -----BEGIN CERTIFICATE-----"); goto out; }
    } else if (ca_st == 0 && existing && existing->ca_info) {
        profile_file(name, "ca.pem", path, sizeof(path));
        pem = read_file(path);
        if (pem) ca_sk = parse_certs(pem);
        free(pem); pem = NULL;
    }

    if (cert_st == 2) {
        cert_sk = parse_certs(cert_txt);
        if (!cert_sk) { set_err(err, err_size, "No certificate found in the client certificate field. Expected PEM text starting with -----BEGIN CERTIFICATE-----"); goto out; }
    } else if (cert_st == 0 && existing && existing->cert_info) {
        profile_file(name, "cert.pem", path, sizeof(path));
        pem = read_file(path);
        if (pem) cert_sk = parse_certs(pem);
        free(pem); pem = NULL;
    }

    int asked = 0;
    int key_changed = key_st != 0;
    if (key_st == 2) {
        key = parse_key(key_txt, pass, &asked);
        if (!key) {
            if (asked && (!pass || !pass[0])) set_err(err, err_size, "The private key is encrypted. Enter its passphrase.");
            else if (asked) set_err(err, err_size, "Wrong passphrase for the private key");
            else set_err(err, err_size, "No private key found in the key field. Expected PEM text starting with -----BEGIN PRIVATE KEY-----");
            goto out;
        }
    } else if (cert_st == 2 && strstr(cert_txt, "PRIVATE KEY-----")) {
        /* A combined PEM with the certificate and its key */
        key = parse_key(cert_txt, pass, &asked);
        key_changed = 1;
        if (!key) {
            if (asked && (!pass || !pass[0])) set_err(err, err_size, "The private key is encrypted. Enter its passphrase.");
            else set_err(err, err_size, asked ? "Wrong passphrase for the private key" : "Could not read the private key in the client certificate field");
            goto out;
        }
    } else if (key_st == 0 && existing && existing->key_info) {
        profile_file(name, "key.pem", path, sizeof(path));
        pem = read_file(path);
        if (pem) {
            key = parse_key(pem, NULL, NULL);
            OPENSSL_cleanse(pem, strlen(pem));
        }
        free(pem); pem = NULL;
    }

    if (cert_sk && !key) { set_err(err, err_size, "A private key is required with the client certificate"); goto out; }
    if (key && !cert_sk) { set_err(err, err_size, "A client certificate is required with the private key"); goto out; }
    if (cert_sk && X509_check_private_key(sk_X509_value(cert_sk, 0), key) != 1) {
        ERR_clear_error();
        set_err(err, err_size, "The private key does not match the client certificate");
        goto out;
    }
    int insecure = insecure_j ? cJSON_IsTrue(insecure_j) : (existing ? existing->insecure : 0);
    if (!ca_sk && !cert_sk && !insecure) {
        set_err(err, err_size, "Add a CA certificate or a client certificate, or turn on 'Don't verify server certificate'");
        goto out;
    }

    /* Stage every changed part next to its final name, then swap them in, so a
     * failed write never leaves a new certificate next to the old key. A new
     * profile replaces all three parts, so files left in the directory by an
     * earlier failed attempt are never picked up. */
    char dir[500];
    profile_dir(name, dir, sizeof(dir));
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        set_err(err, err_size, "Could not store the profile: %s", strerror(errno));
        goto out;
    }
    int fresh = existing == NULL;
    size_t len = 0;
    const struct { const char* file; int changed; int present; } parts[3] = {
        { "ca.pem",   fresh || ca_st != 0,   ca_sk != NULL },
        { "cert.pem", fresh || cert_st != 0, cert_sk != NULL },
        { "key.pem",  fresh || key_changed,  key != NULL },
    };
    char staged[3][620];
    for (int i = 0; i < 3; i++) {
        staged[i][0] = '\0';
        if (!parts[i].changed || !parts[i].present) continue;
        profile_file(name, parts[i].file, path, sizeof(path));
        snprintf(staged[i], sizeof(staged[i]), "%s.new", path);
        if (i == 0)      pem = stack_to_pem(ca_sk, &len);
        else if (i == 1) pem = stack_to_pem(cert_sk, &len);
        else             pem = key_to_pem(key, &len);
        int w = pem && write_file_secure(staged[i], pem, len);
        int saved_errno = errno;
        if (pem) OPENSSL_cleanse(pem, len);
        free(pem); pem = NULL;
        if (!w) {
            for (int j = 0; j <= i; j++) if (staged[j][0]) unlink(staged[j]);
            set_err(err, err_size, "Could not store the profile: %s", strerror(saved_errno));
            goto out;
        }
    }
    int swapped = 1;
    for (int i = 0; i < 3; i++) {
        if (!parts[i].changed) continue;
        profile_file(name, parts[i].file, path, sizeof(path));
        if (!parts[i].present) unlink(path);
        else if (rename(staged[i], path) != 0) { swapped = 0; unlink(staged[i]); }
    }

    Profile* p = existing;
    if (!p) {
        p = &profiles[profile_count++];
        memset(p, 0, sizeof(*p));
        snprintf(p->name, sizeof(p->name), "%s", name);
    }
    p->insecure = insecure;
    p->updated = (double)time(NULL);
    load_profile_files(p);   /* always reflect what is on disk */
    if (!swapped) {
        set_err(err, err_size, "Could not store the profile: %s", strerror(errno));
        goto out;
    }
    if (!write_index()) {
        set_err(err, err_size, "Could not store the profile list: %s", strerror(errno));
        goto out;
    }
    LOG("TLS profile '%s' saved", name);
    ok = 1;

out:
    pthread_mutex_unlock(&lock);
    if (ca_sk) sk_X509_pop_free(ca_sk, X509_free);
    if (cert_sk) sk_X509_pop_free(cert_sk, X509_free);
    EVP_PKEY_free(key);
    return ok;
}

int Certs_Delete(const char* name) {
    pthread_mutex_lock(&lock);
    Profile* p = find(name);
    if (!p) { pthread_mutex_unlock(&lock); return 0; }
    const char* files[] = { "ca.pem", "cert.pem", "key.pem" };
    char path[600];
    for (int i = 0; i < 3; i++) {
        profile_file(name, files[i], path, sizeof(path));
        unlink(path);
    }
    profile_dir(name, path, sizeof(path));
    rmdir(path);
    profile_free(p);
    int idx = (int)(p - profiles);
    memmove(&profiles[idx], &profiles[idx + 1], (size_t)(profile_count - idx - 1) * sizeof(Profile));
    profile_count--;
    write_index();
    LOG("TLS profile '%s' deleted", name);
    pthread_mutex_unlock(&lock);
    return 1;
}

int Certs_Get_Paths(const char* name, Certs_Paths* out) {
    memset(out, 0, sizeof(*out));
    pthread_mutex_lock(&lock);
    Profile* p = find(name);
    if (p) {
        profile_file(p->name, "ca.pem", out->ca, sizeof(out->ca));
        profile_file(p->name, "cert.pem", out->cert, sizeof(out->cert));
        profile_file(p->name, "key.pem", out->key, sizeof(out->key));
        out->has_ca = p->ca_info != NULL;
        out->has_cert = p->cert_info != NULL && p->key_info != NULL;
        out->insecure = p->insecure;
    }
    pthread_mutex_unlock(&lock);
    return p != NULL;
}

SSL_CTX* Certs_New_SSL_CTX(const char* name, int* insecure, char* err, size_t err_size) {
    Certs_Paths cp;
    if (insecure) *insecure = 0;
    if (!Certs_Get_Paths(name, &cp)) {
        set_err(err, err_size, "TLS profile '%s' does not exist", name ? name : "");
        return NULL;
    }
    char reason[200];
    ERR_clear_error();
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) { openssl_reason(reason, sizeof(reason)); set_err(err, err_size, "SSL_CTX_new failed: %s", reason); return NULL; }
    SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    int loaded = cp.has_ca ? SSL_CTX_load_verify_locations(ctx, cp.ca, NULL)
                           : SSL_CTX_set_default_verify_paths(ctx);
    if (loaded != 1) {
        openssl_reason(reason, sizeof(reason));
        set_err(err, err_size, "TLS profile '%s': could not load CA certificates: %s", name, reason);
        SSL_CTX_free(ctx);
        return NULL;
    }
    if (cp.has_cert) {
        if (SSL_CTX_use_certificate_chain_file(ctx, cp.cert) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, cp.key, SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_check_private_key(ctx) != 1) {
            openssl_reason(reason, sizeof(reason));
            set_err(err, err_size, "TLS profile '%s': could not load the client certificate: %s", name, reason);
            SSL_CTX_free(ctx);
            return NULL;
        }
    }
    if (insecure) *insecure = cp.insecure;
    return ctx;
}

static int is_ip_literal(const char* host) {
    unsigned char addr[16];
    return inet_pton(AF_INET, host, addr) == 1 || inet_pton(AF_INET6, host, addr) == 1;
}

void Certs_Set_SNI(SSL* ssl, const char* host) {
    if (host && host[0] && !is_ip_literal(host)) SSL_set_tlsext_host_name(ssl, host);
}

int Certs_Check_Server_Identity(SSL* ssl, const char* host) {
    if (!host || !host[0]) return 1;
    X509* peer = SSL_get1_peer_certificate(ssl);
    if (!peer) return 0;
    int ok;
    if (!is_ip_literal(host)) {
        ok = X509_check_host(peer, host, 0, 0, NULL) == 1;
    } else {
        ok = X509_check_ip_asc(peer, host, 0) == 1;
        if (!ok && X509_get_ext_by_NID(peer, NID_subject_alt_name, -1) < 0) {
            char cn[256] = "";
            X509_NAME_get_text_by_NID(X509_get_subject_name(peer), NID_commonName, cn, sizeof(cn));
            ok = strcmp(cn, host) == 0;
        }
    }
    X509_free(peer);
    return ok;
}

static void expiry_check(cJSON* out, const char* profile, const char* what, cJSON* info, int days, time_t now) {
    double na = cJSON_GetObjectItem(info, "not_after")->valuedouble;
    if (na <= 0) return;
    char date[32];
    time_t t = (time_t)na;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(date, sizeof(date), "%Y-%m-%d", &tm);
    char msg[256];
    if (na < (double)now) {
        snprintf(msg, sizeof(msg), "TLS profile '%s': %s expired on %s", profile, what, date);
    } else if (na < (double)now + (double)days * 86400.0) {
        int left = (int)((na - (double)now) / 86400.0);
        snprintf(msg, sizeof(msg), "TLS profile '%s': %s expires in %d day%s (%s)", profile, what, left, left == 1 ? "" : "s", date);
    } else {
        return;
    }
    cJSON_AddItemToArray(out, cJSON_CreateString(msg));
}

cJSON* Certs_Expiry_Warnings(int days) {
    cJSON* out = cJSON_CreateArray();
    time_t now = time(NULL);
    pthread_mutex_lock(&lock);
    for (int i = 0; i < profile_count; i++) {
        Profile* p = &profiles[i];
        cJSON* leaf = p->cert_info ? cJSON_GetArrayItem(p->cert_info, 0) : NULL;
        if (leaf) expiry_check(out, p->name, "client certificate", leaf, days, now);
        cJSON* c;
        cJSON_ArrayForEach(c, p->ca_info) expiry_check(out, p->name, "CA certificate", c, days, now);
    }
    pthread_mutex_unlock(&lock);
    return out;
}
