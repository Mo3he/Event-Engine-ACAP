/* Host tests for app/engine/certs.c: validation, storage, reload, expiry and
 * real TLS handshakes against a server that requires a client certificate.
 *
 *   cc -std=gnu11 -Wall -Wextra -Werror -Iapp/engine -o /tmp/certs_test \
 *      tools/certs_test.c app/engine/certs.c app/cJSON.c -lssl -lcrypto -lpthread
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

#include "certs.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static EVP_PKEY* new_key(void) { return EVP_EC_gen("P-256"); }

static X509* make_cert(const char* cn, EVP_PKEY* key, X509* issuer, EVP_PKEY* issuer_key,
                       int is_ca, long days_valid, const char* san) {
    X509* x = X509_new();
    X509_set_version(x, 2);
    static long serial = 1;
    ASN1_INTEGER_set(X509_get_serialNumber(x), serial++);
    X509_gmtime_adj(X509_getm_notBefore(x), -3600);
    X509_gmtime_adj(X509_getm_notAfter(x), days_valid * 86400L);
    X509_set_pubkey(x, key);
    X509_NAME* nm = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(nm, "CN", MBSTRING_ASC, (const unsigned char*)cn, -1, -1, 0);
    X509_set_issuer_name(x, issuer ? X509_get_subject_name(issuer) : nm);
    X509V3_CTX ctx;
    X509V3_set_ctx_nodb(&ctx);
    X509V3_set_ctx(&ctx, issuer ? issuer : x, x, NULL, NULL, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_basic_constraints, is_ca ? "critical,CA:TRUE" : "CA:FALSE");
    X509_add_ext(x, ext, -1);
    X509_EXTENSION_free(ext);
    if (san) {
        ext = X509V3_EXT_conf_nid(NULL, &ctx, NID_subject_alt_name, san);
        X509_add_ext(x, ext, -1);
        X509_EXTENSION_free(ext);
    }
    X509_sign(x, issuer_key ? issuer_key : key, EVP_sha256());
    return x;
}

static char* cert_pem(X509* x) {
    BIO* b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(b, x);
    char* d; long n = BIO_get_mem_data(b, &d);
    char* out = strndup(d, (size_t)n);
    BIO_free(b);
    return out;
}

static char* key_pem(EVP_PKEY* k, const char* pass) {
    BIO* b = BIO_new(BIO_s_mem());
    if (pass) PEM_write_bio_PKCS8PrivateKey(b, k, EVP_aes_256_cbc(), pass, (int)strlen(pass), NULL, NULL);
    else      PEM_write_bio_PrivateKey(b, k, NULL, NULL, 0, NULL, NULL);
    char* d; long n = BIO_get_mem_data(b, &d);
    char* out = strndup(d, (size_t)n);
    BIO_free(b);
    return out;
}

static char* concat(const char* a, const char* b) {
    char* out = malloc(strlen(a) + strlen(b) + 1);
    strcpy(out, a); strcat(out, b);
    return out;
}

static int save(const char* json, char* err) {
    cJSON* body = cJSON_Parse(json);
    err[0] = '\0';
    int r = Certs_Save(body, err, 256);
    cJSON_Delete(body);
    return r;
}

/* Build {"name":..., <key>:<pem>, ...} without hand-escaping PEM newlines. */
static int save_obj(char* err, const char* name, const char* k1, const char* v1,
                    const char* k2, const char* v2, const char* k3, const char* v3) {
    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", name);
    if (k1) cJSON_AddStringToObject(o, k1, v1);
    if (k2) cJSON_AddStringToObject(o, k2, v2);
    if (k3) cJSON_AddStringToObject(o, k3, v3);
    err[0] = '\0';
    int r = Certs_Save(o, err, 256);
    cJSON_Delete(o);
    return r;
}

/* ---------- TLS handshake harness ---------- */

typedef struct { SSL_CTX* ctx; int fd; int ok; char peer_cn[128]; } ServerArg;

static void* server_thread(void* a) {
    ServerArg* s = a;
    SSL* ssl = SSL_new(s->ctx);
    SSL_set_fd(ssl, s->fd);
    s->ok = SSL_accept(ssl) == 1;
    if (s->ok) {
        X509* peer = SSL_get1_peer_certificate(ssl);
        if (peer) {
            X509_NAME_get_text_by_NID(X509_get_subject_name(peer), NID_commonName, s->peer_cn, sizeof(s->peer_cn));
            X509_free(peer);
        }
        char buf[8];
        SSL_read(ssl, buf, sizeof(buf));
        SSL_write(ssl, "pong", 4);
    }
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(s->fd);
    return NULL;
}

/* Handshake using a profile the way the MQTT client does. Returns 1 on success. */
static int handshake(SSL_CTX* server_ctx, const char* profile, const char* host, char* peer_cn) {
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    ServerArg sa = { server_ctx, sv[1], 0, "" };
    pthread_t th;
    pthread_create(&th, NULL, server_thread, &sa);

    int ok = 0, insecure = 0;
    char err[256] = "";
    SSL_CTX* cctx = Certs_New_SSL_CTX(profile, &insecure, err, sizeof(err));
    if (cctx) {
        SSL* ssl = SSL_new(cctx);
        SSL_CTX_free(cctx);  /* the SSL keeps its own reference */
        SSL_set_verify(ssl, insecure ? SSL_VERIFY_NONE : SSL_VERIFY_PEER, NULL);
        Certs_Set_SNI(ssl, host);
        SSL_set_fd(ssl, sv[0]);
        if (SSL_connect(ssl) == 1 && (insecure || Certs_Check_Server_Identity(ssl, host)) &&
            SSL_write(ssl, "ping", 4) == 4) {
            char buf[8];
            ok = SSL_read(ssl, buf, sizeof(buf)) == 4;
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
    }
    ERR_clear_error();
    close(sv[0]);
    pthread_join(th, NULL);
    if (peer_cn) strcpy(peer_cn, sa.peer_cn);
    return ok && sa.ok;
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    char dir[] = "/tmp/certs_test_XXXXXX";
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    char store[300];
    snprintf(store, sizeof(store), "%s/tls", dir);
    char err[256];

    EVP_PKEY* ca_key = new_key();
    X509* ca = make_cert("Test Root CA", ca_key, NULL, NULL, 1, 3650, NULL);
    EVP_PKEY* other_ca_key = new_key();
    X509* other_ca = make_cert("Other CA", other_ca_key, NULL, NULL, 1, 3650, NULL);
    EVP_PKEY* srv_key = new_key();
    X509* srv = make_cert("upload.example", srv_key, ca, ca_key, 0, 365, "DNS:upload.example,IP:127.0.0.1");
    EVP_PKEY* cli_key = new_key();
    X509* cli = make_cert("camera-01", cli_key, ca, ca_key, 0, 365, NULL);
    X509* cli_renewed = make_cert("camera-01", cli_key, ca, ca_key, 0, 730, NULL);
    X509* cli_soon = make_cert("camera-soon", cli_key, ca, ca_key, 0, 5, NULL);
    X509* cli_expired = make_cert("camera-old", cli_key, ca, ca_key, 0, -2, NULL);
    EVP_PKEY* stray_key = new_key();
    X509* other_cli = make_cert("intruder", stray_key, other_ca, other_ca_key, 0, 365, NULL);

    char *ca_pem = cert_pem(ca), *cli_pem = cert_pem(cli), *cli_key_pem = key_pem(cli_key, NULL);
    char* cli_key_enc = key_pem(cli_key, "s3cret");
    char* stray_key_pem = key_pem(stray_key, NULL);
    char* combined = concat(cli_pem, cli_key_pem);

    CHECK(Certs_Init(store), "init");
    cJSON* l = Certs_List();
    CHECK(cJSON_GetArraySize(l) == 0, "empty list");
    cJSON_Delete(l);

    /* Validation */
    CHECK(!save("{\"name\":\"../evil\",\"insecure\":true}", err), "path in name rejected");
    CHECK(!save("{\"name\":\"\",\"insecure\":true}", err), "empty name rejected");
    CHECK(!save("{\"name\":\"nothing\"}", err) && strstr(err, "CA certificate"), "empty profile rejected: %s", err);
    CHECK(!save("{\"name\":\"x\",\"insecure\":\"yes\"}", err), "non-bool insecure rejected");
    CHECK(!save_obj(err, "bad", "ca", "hello", NULL, NULL, NULL, NULL) && strstr(err, "CA field"), "garbage CA: %s", err);
    CHECK(!save_obj(err, "nokey", "cert", cli_pem, NULL, NULL, NULL, NULL) && strstr(err, "private key is required"), "cert without key: %s", err);
    CHECK(!save_obj(err, "nocert", "key", cli_key_pem, NULL, NULL, NULL, NULL) && strstr(err, "client certificate is required"), "key without cert: %s", err);
    CHECK(!save_obj(err, "mismatch", "cert", cli_pem, "key", stray_key_pem, NULL, NULL) && strstr(err, "does not match"), "mismatch: %s", err);
    CHECK(!save_obj(err, "enc", "cert", cli_pem, "key", cli_key_enc, NULL, NULL) && strstr(err, "encrypted"), "encrypted no pass: %s", err);
    CHECK(!save_obj(err, "enc", "cert", cli_pem, "key", cli_key_enc, "passphrase", "wrong") && strstr(err, "Wrong passphrase"), "wrong pass: %s", err);
    CHECK(!Certs_Exists("enc") && !Certs_Exists("mismatch"), "failed saves leave nothing behind");

    /* Valid profiles */
    CHECK(save("{\"name\":\"self-signed\",\"insecure\":true}", err), "insecure-only profile: %s", err);
    CHECK(save_obj(err, "ca-only", "ca", ca_pem, NULL, NULL, NULL, NULL), "CA only: %s", err);
    {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", "Walmart upload");
        cJSON_AddStringToObject(o, "ca", ca_pem);
        cJSON_AddStringToObject(o, "cert", cli_pem);
        cJSON_AddStringToObject(o, "key", cli_key_enc);
        cJSON_AddStringToObject(o, "passphrase", "s3cret");
        CHECK(Certs_Save(o, err, sizeof(err)), "full profile with encrypted key: %s", err);
        cJSON_Delete(o);
    }
    CHECK(save_obj(err, "combined", "cert", combined, NULL, NULL, NULL, NULL), "combined cert+key: %s", err);

    /* Stored key is decrypted, private, and never listed */
    char path[600];
    snprintf(path, sizeof(path), "%s/Walmart upload/key.pem", store);
    struct stat st;
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0600, "key.pem mode 0600");
    FILE* f = fopen(path, "r");
    char kbuf[4096] = "";
    if (f) { size_t n = fread(kbuf, 1, sizeof(kbuf) - 1, f); kbuf[n] = '\0'; fclose(f); }
    CHECK(strstr(kbuf, "BEGIN PRIVATE KEY") && !strstr(kbuf, "ENCRYPTED"), "key stored decrypted");
    snprintf(path, sizeof(path), "%s/Walmart upload", store);
    CHECK(stat(path, &st) == 0 && (st.st_mode & 0777) == 0700, "profile dir mode 0700");

    l = Certs_List();
    char* txt = cJSON_PrintUnformatted(l);
    CHECK(!strstr(txt, "PRIVATE KEY") && !strstr(txt, "s3cret"), "list has no secrets");
    CHECK(cJSON_GetArraySize(l) == 4, "4 profiles, got %d", cJSON_GetArraySize(l));
    free(txt);
    cJSON_Delete(l);

    cJSON* w = Certs_Get("Walmart upload");
    cJSON* leaf = cJSON_GetArrayItem(cJSON_GetObjectItem(w, "cert"), 0);
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(leaf, "cn")), "camera-01") == 0, "leaf CN");
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(leaf, "issuer")), "Test Root CA") != NULL, "leaf issuer");
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(w, "key"), "type")), "EC") == 0, "key type");
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(w, "ca"), 0), "is_ca")), "CA flagged");
    CHECK(cJSON_GetObjectItem(w, "expires")->valuedouble > 0, "expires set");
    cJSON_Delete(w);
    w = Certs_Get("ca-only");
    char srv_info_ok = cJSON_IsNull(cJSON_GetObjectItem(w, "cert")) && cJSON_IsNull(cJSON_GetObjectItem(w, "key"));
    CHECK(srv_info_ok, "ca-only has no cert/key");
    cJSON_Delete(w);

    /* Partial updates */
    CHECK(save("{\"name\":\"Walmart upload\",\"insecure\":true}", err), "toggle insecure: %s", err);
    Certs_Paths cp;
    CHECK(Certs_Get_Paths("Walmart upload", &cp) && cp.has_ca && cp.has_cert && cp.insecure, "update kept CA and cert");
    CHECK(save("{\"name\":\"Walmart upload\",\"insecure\":false}", err), "toggle back: %s", err);
    char* renewed = cert_pem(cli_renewed);
    CHECK(save_obj(err, "Walmart upload", "cert", renewed, NULL, NULL, NULL, NULL), "renewed cert, same key: %s", err);
    char* intruder = cert_pem(other_cli);
    CHECK(!save_obj(err, "Walmart upload", "cert", intruder, NULL, NULL, NULL, NULL) && strstr(err, "does not match"), "new cert, old key mismatch: %s", err);
    CHECK(Certs_Get_Paths("Walmart upload", &cp) && cp.has_cert, "failed update kept the old cert");
    CHECK(save_obj(err, "combined", "ca", "", NULL, NULL, NULL, NULL), "clearing an absent CA is fine: %s", err);
    CHECK(save_obj(err, "ca-only", "ca", "", NULL, NULL, NULL, NULL) == 0, "clearing the only content is rejected");
    CHECK(!save_obj(err, "Walmart upload", "cert", "", NULL, NULL, NULL, NULL), "removing cert but not key rejected");
    CHECK(save_obj(err, "combined", "cert", "", "key", "", "ca", ca_pem), "replace client cert by CA: %s", err);
    CHECK(Certs_Get_Paths("combined", &cp) && cp.has_ca && !cp.has_cert && access(cp.key, F_OK) != 0, "key file removed");

    /* Reload as after a restart */
    Certs_Cleanup();
    CHECK(Certs_Init(store), "re-init");
    l = Certs_List();
    CHECK(cJSON_GetArraySize(l) == 4, "4 profiles after reload");
    cJSON_Delete(l);
    w = Certs_Get("Walmart upload");
    leaf = cJSON_GetArrayItem(cJSON_GetObjectItem(w, "cert"), 0);
    CHECK(leaf && cJSON_GetObjectItem(leaf, "not_after")->valuedouble > (double)time(NULL) + 600 * 86400.0, "renewed cert reloaded");
    CHECK(!cJSON_IsTrue(cJSON_GetObjectItem(w, "insecure")), "insecure flag reloaded");
    cJSON_Delete(w);
    CHECK(Certs_Get_Paths("self-signed", &cp) && cp.insecure && !cp.has_ca, "insecure profile reloaded");

    /* Expiry */
    char* soon = cert_pem(cli_soon);
    char* old = cert_pem(cli_expired);
    CHECK(save_obj(err, "soon", "cert", soon, "key", cli_key_pem, NULL, NULL), "soon: %s", err);
    CHECK(save_obj(err, "old", "cert", old, "key", cli_key_pem, NULL, NULL), "expired cert can be stored: %s", err);
    cJSON* warn = Certs_Expiry_Warnings(30);
    txt = cJSON_PrintUnformatted(warn);
    CHECK(strstr(txt, "'soon': client certificate expires in 4 days") || strstr(txt, "'soon': client certificate expires in 5 days"), "soon warning: %s", txt);
    CHECK(strstr(txt, "'old': client certificate expired on"), "expired warning: %s", txt);
    CHECK(cJSON_GetArraySize(warn) == 2, "only two warnings: %s", txt);
    free(txt);
    cJSON_Delete(warn);

    /* Real handshakes against a server that requires a client certificate from the test CA */
    SSL_CTX* sctx = SSL_CTX_new(TLS_server_method());
    SSL_CTX_use_certificate(sctx, srv);
    SSL_CTX_use_PrivateKey(sctx, srv_key);
    X509_STORE_add_cert(SSL_CTX_get_cert_store(sctx), ca);
    SSL_CTX_set_verify(sctx, SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);

    char peer[128] = "";
    CHECK(handshake(sctx, "Walmart upload", "upload.example", peer) && strcmp(peer, "camera-01") == 0, "mTLS with CA + client cert (peer %s)", peer);
    CHECK(handshake(sctx, "Walmart upload", "127.0.0.1", NULL), "IP SAN accepted");
    CHECK(!handshake(sctx, "Walmart upload", "127.0.0.2", NULL), "IP not in SAN rejected");
    CHECK(!handshake(sctx, "Walmart upload", "wrong.example", NULL), "wrong host name rejected");
    CHECK(!handshake(sctx, "ca-only", "upload.example", NULL), "no client cert rejected by server");
    CHECK(!handshake(sctx, "soon", "upload.example", NULL), "unknown server CA rejected (system CAs)");
    CHECK(save("{\"name\":\"soon\",\"insecure\":true}", err), "make soon insecure: %s", err);
    CHECK(handshake(sctx, "soon", "anything", peer) && strcmp(peer, "camera-soon") == 0, "insecure still presents the client cert (peer %s)", peer);
    CHECK(!handshake(sctx, "missing", "upload.example", NULL), "missing profile fails");
    int ins = 0;
    CHECK(Certs_New_SSL_CTX("missing", &ins, err, sizeof(err)) == NULL && strstr(err, "does not exist"), "missing profile error: %s", err);

    /* IP address in the CN is accepted only when the certificate has no SANs */
    EVP_PKEY* ipk = new_key();
    X509* ip_cn = make_cert("127.0.0.1", ipk, ca, ca_key, 0, 365, NULL);
    X509* ip_cn_dns = make_cert("127.0.0.1", ipk, ca, ca_key, 0, 365, "DNS:other.example");
    for (int v = 0; v < 2; v++) {
        SSL_CTX* s2 = SSL_CTX_new(TLS_server_method());
        SSL_CTX_use_certificate(s2, v ? ip_cn_dns : ip_cn);
        SSL_CTX_use_PrivateKey(s2, ipk);
        int got = handshake(s2, "Walmart upload", "127.0.0.1", NULL);
        CHECK(got == !v, v ? "IP CN ignored when SANs exist" : "IP CN accepted without SANs");
        SSL_CTX_free(s2);
    }
    X509_free(ip_cn); X509_free(ip_cn_dns); EVP_PKEY_free(ipk);

    /* A new profile never picks up files left in its directory by an earlier failed save */
    snprintf(path, sizeof(path), "%s/leftover", store);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/leftover/ca.pem", store);
    f = fopen(path, "w"); if (f) { fputs(ca_pem, f); fclose(f); }
    CHECK(save("{\"name\":\"leftover\",\"insecure\":true}", err), "leftover: %s", err);
    CHECK(Certs_Get_Paths("leftover", &cp) && !cp.has_ca && access(cp.ca, F_OK) != 0, "stale CA file not adopted");

    /* A write that fails part-way (certificate written, key not) leaves the stored
     * certificate and key together: block the key's temporary file with a directory. */
    CHECK(save_obj(err, "atomic", "cert", cli_pem, "key", cli_key_pem, NULL, NULL), "atomic: %s", err);
    {
        char blk1[600], blk2[600];
        snprintf(blk1, sizeof(blk1), "%s/atomic/key.pem.tmp", store);
        snprintf(blk2, sizeof(blk2), "%s/atomic/key.pem.new.tmp", store);
        mkdir(blk1, 0700);
        mkdir(blk2, 0700);
        CHECK(!save_obj(err, "atomic", "cert", intruder, "key", stray_key_pem, NULL, NULL) && strstr(err, "Could not store"),
              "write failure reported: %s", err);
        rmdir(blk1);
        rmdir(blk2);
        w = Certs_Get("atomic");
        leaf = cJSON_GetArrayItem(cJSON_GetObjectItem(w, "cert"), 0);
        CHECK(leaf && strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(leaf, "cn")), "camera-01") == 0, "old certificate kept");
        cJSON_Delete(w);
        SSL_CTX* actx = Certs_New_SSL_CTX("atomic", &ins, err, sizeof(err));
        CHECK(actx != NULL, "certificate and key still match after the failed write: %s", err);
        SSL_CTX_free(actx);
        snprintf(path, sizeof(path), "%s/atomic/cert.pem.new", store);
        CHECK(access(path, F_OK) != 0, "no staged files left behind");
    }
    Certs_Delete("leftover");
    Certs_Delete("atomic");

    /* Delete and limits */
    CHECK(Certs_Delete("Walmart upload") && !Certs_Exists("Walmart upload"), "delete");
    snprintf(path, sizeof(path), "%s/Walmart upload", store);
    CHECK(access(path, F_OK) != 0, "profile directory removed");
    CHECK(!Certs_Delete("Walmart upload"), "delete twice");
    int made = 0;
    for (int i = 0; i < 20; i++) {
        char nm[32];
        snprintf(nm, sizeof(nm), "p%d", i);
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", nm);
        cJSON_AddBoolToObject(o, "insecure", 1);
        made += Certs_Save(o, err, sizeof(err));
        cJSON_Delete(o);
    }
    l = Certs_List();
    CHECK(cJSON_GetArraySize(l) == CERTS_MAX_PROFILES && strstr(err, "At most"), "profile limit (%d, %s)", cJSON_GetArraySize(l), err);
    cJSON_Delete(l);
    (void)made;

    Certs_Cleanup();
    SSL_CTX_free(sctx);
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", dir);
    if (system(cmd) != 0) printf("cleanup of %s failed\n", dir);

    if (failures) { printf("%d check(s) failed\n", failures); return 1; }
    printf("certs: all checks passed\n");
    return 0;
}
