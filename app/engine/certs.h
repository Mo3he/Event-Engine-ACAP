#ifndef _ENGINE_CERTS_H_
#define _ENGINE_CERTS_H_

#include <stddef.h>
#include <openssl/ssl.h>
#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * TLS profiles: named sets of certificates used when the camera connects to a
 * server (HTTPS actions, HTTP check condition, MQTT).
 *
 * A profile may hold:
 *   - a CA certificate (or bundle) used instead of the system CAs to verify the server
 *   - a client certificate (optionally followed by its chain) and its private key,
 *     presented to servers that require mutual TLS
 *   - an "insecure" flag that skips verification of the server certificate
 *
 * Each profile is stored in its own directory below the store directory as
 * ca.pem / cert.pem / key.pem (mode 0600). Private keys are decrypted when
 * uploaded and are never returned by Certs_List / Certs_Get.
 */

#define CERTS_MAX_PROFILES 16
#define CERTS_NAME_MAX     48

typedef struct {
    char ca[512];
    char cert[512];
    char key[512];
    int  has_ca;
    int  has_cert;   /* client certificate and key */
    int  insecure;
} Certs_Paths;

/* dir: absolute directory for the store, created if missing. */
int    Certs_Init(const char* dir);
void   Certs_Cleanup(void);

/* Array of profile metadata objects (never contains key material). Caller frees. */
cJSON* Certs_List(void);
/* Metadata for one profile, or NULL. Caller frees. */
cJSON* Certs_Get(const char* name);
int    Certs_Exists(const char* name);

/* Create a profile or update an existing one. Body fields:
 *   name        required
 *   ca, cert, key  PEM text. Omitted = keep the stored value, "" = remove it.
 *                  "cert" may also contain the private key.
 *   passphrase  for an encrypted key
 *   insecure    true = don't verify the server certificate
 * Returns 1 on success; on failure 0 with a user-readable message in err. */
int    Certs_Save(cJSON* body, char* err, size_t err_size);
int    Certs_Delete(const char* name);

/* File paths for a profile. Returns 0 when the profile does not exist. */
int    Certs_Get_Paths(const char* name, Certs_Paths* out);

/* SSL_CTX for a TLS client using the profile's CA and client certificate.
 * *insecure is set when the profile skips server verification.
 * Returns NULL with a message in err when the profile is missing or unusable. */
SSL_CTX* Certs_New_SSL_CTX(const char* name, int* insecure, char* err, size_t err_size);

/* Server identity for TLS clients. Call Certs_Set_SNI before the handshake and,
 * when verifying, Certs_Check_Server_Identity after it. Names are matched like
 * OpenSSL host checks; IP addresses match an IP SAN, or the CN when the
 * certificate has no SANs (as curl does). IPs are not sent as SNI. */
void   Certs_Set_SNI(SSL* ssl, const char* host);
int    Certs_Check_Server_Identity(SSL* ssl, const char* host);

/* Messages for certificates that expired or expire within `days`. Caller frees. */
cJSON* Certs_Expiry_Warnings(int days);

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_CERTS_H_ */
