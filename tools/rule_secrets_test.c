/* Host tests for app/engine/rule_secrets.c: masking secrets in exported rules
 * and restoring masked values on save and import.
 *
 *   cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -Iapp/engine \
 *      -o /tmp/rule_secrets_test tools/rule_secrets_test.c \
 *      app/engine/rule_secrets.c app/cJSON.c -lm
 */
#include <stdio.h>
#include <string.h>

#include "rule_secrets.h"

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const char* RULE =
    "{\"name\":\"token password\",\"enabled\":true,"
    "\"triggers\":[{\"type\":\"http_webhook\",\"token\":\"hooktok\"}],"
    "\"conditions\":[{\"type\":\"http_check\",\"url\":\"http://x\",\"remote_pass\":\"rp\"}],"
    "\"actions\":["
    "{\"type\":\"telegram\",\"bot_token\":\"123:ABC\",\"chat_id\":\"42\",\"message\":\"hi\"},"
    "{\"type\":\"http_request\",\"url\":\"http://h\",\"username\":\"u\",\"password\":\"pw\","
    "\"headers\":\"Content-Type: application/json\\nAuthorization: Bearer abc\\nX-Api-Key: k1\"},"
    "{\"type\":\"slack_webhook\",\"webhook_url\":\"https://hooks.slack.com/services/T/B/x\"},"
    "{\"type\":\"email\",\"password\":\"\"},"
    "{\"type\":\"digest\",\"deliver\":{\"webhook_url\":\"https://hook/nested\"}}]}";

static const char* str_at(cJSON* rule, const char* list, int i, const char* key) {
    return cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(rule, list), i), key));
}

static int has_path(cJSON* arr, const char* path) {
    cJSON* it;
    cJSON_ArrayForEach(it, arr)
        if (strcmp(cJSON_GetStringValue(it), path) == 0) return 1;
    return 0;
}

static void test_mask(void) {
    cJSON* r = cJSON_Parse(RULE);
    int n = RuleSecrets_Mask(r);
    CHECK(n == 8, "masked %d values, expected 8", n);
    CHECK(strcmp(str_at(r, "triggers", 0, "token"), RULE_SECRET_MASK) == 0, "webhook token");
    CHECK(strcmp(str_at(r, "conditions", 0, "remote_pass"), RULE_SECRET_MASK) == 0, "remote_pass");
    CHECK(strcmp(str_at(r, "actions", 0, "bot_token"), RULE_SECRET_MASK) == 0, "bot_token");
    CHECK(strcmp(str_at(r, "actions", 0, "chat_id"), "42") == 0, "chat_id kept");
    CHECK(strcmp(str_at(r, "actions", 1, "username"), "u") == 0, "username kept");
    CHECK(strcmp(str_at(r, "actions", 1, "password"), RULE_SECRET_MASK) == 0, "password");
    CHECK(strcmp(str_at(r, "actions", 1, "headers"),
                 "Content-Type: application/json\nAuthorization: " RULE_SECRET_MASK
                 "\nX-Api-Key: " RULE_SECRET_MASK) == 0,
          "headers: %s", str_at(r, "actions", 1, "headers"));
    CHECK(strcmp(str_at(r, "actions", 2, "webhook_url"), RULE_SECRET_MASK) == 0, "webhook_url");
    CHECK(strcmp(str_at(r, "actions", 3, "password"), "") == 0, "empty password stays empty");
    cJSON* deliver = cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "actions"), 4), "deliver");
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(deliver, "webhook_url")), RULE_SECRET_MASK) == 0,
          "nested webhook_url");
    CHECK(strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(r, "name")), "token password") == 0, "name kept");
    CHECK(RuleSecrets_Mask(r) == 0, "masking twice changes nothing");
    cJSON_Delete(r);
}

static void test_round_trip(void) {
    cJSON* orig = cJSON_Parse(RULE);
    cJSON* r = cJSON_Parse(RULE);
    RuleSecrets_Mask(r);
    cJSON* missing = cJSON_CreateArray();
    cJSON* regen = cJSON_CreateArray();
    int unresolved = RuleSecrets_Restore(r, orig, missing, regen);
    CHECK(unresolved == 0, "unresolved %d", unresolved);
    CHECK(cJSON_GetArraySize(missing) == 0 && cJSON_GetArraySize(regen) == 0, "nothing reported");
    CHECK(cJSON_Compare(r, orig, 1), "restored rule equals original");
    cJSON_Delete(missing); cJSON_Delete(regen); cJSON_Delete(r); cJSON_Delete(orig);
}

static void test_restore_without_stored(void) {
    cJSON* r = cJSON_Parse(RULE);
    RuleSecrets_Mask(r);
    cJSON* missing = cJSON_CreateArray();
    cJSON* regen = cJSON_CreateArray();
    int unresolved = RuleSecrets_Restore(r, NULL, missing, regen);
    CHECK(unresolved == 7, "unresolved %d, expected 7", unresolved);
    CHECK(has_path(missing, "actions[0].bot_token"), "bot_token reported");
    CHECK(has_path(missing, "actions[1].headers[Authorization]"), "Authorization reported");
    CHECK(has_path(missing, "actions[4].deliver.webhook_url"), "nested path reported");
    CHECK(!has_path(missing, "triggers[0].token"), "webhook token not reported missing");
    CHECK(has_path(regen, "triggers[0].token"), "webhook token regenerated");
    const char* tok = str_at(r, "triggers", 0, "token");
    CHECK(tok && strlen(tok) == 32 && strcmp(tok, RULE_SECRET_MASK) != 0, "new token: %s", tok ? tok : "(null)");
    CHECK(!cJSON_GetObjectItem(cJSON_GetArrayItem(cJSON_GetObjectItem(r, "actions"), 0), "bot_token"),
          "unresolved field removed");
    CHECK(strcmp(str_at(r, "actions", 1, "headers"), "Content-Type: application/json") == 0,
          "only unmasked headers kept: %s", str_at(r, "actions", 1, "headers"));
    char* text = cJSON_PrintUnformatted(r);
    CHECK(!strstr(text, RULE_SECRET_MASK), "no placeholder left: %s", text);
    cJSON_free(text);
    cJSON_Delete(missing); cJSON_Delete(regen); cJSON_Delete(r);
}

static void test_type_mismatch_and_changed_values(void) {
    cJSON* stored = cJSON_Parse(RULE);
    cJSON* r = cJSON_Parse(
        "{\"name\":\"x\",\"triggers\":[{\"type\":\"manual\"}],\"actions\":["
        "{\"type\":\"http_request\",\"password\":\"" RULE_SECRET_MASK "\"},"
        "{\"type\":\"http_request\",\"password\":\"new-pw\","
        "\"headers\":\"authorization: " RULE_SECRET_MASK "\\nX-Other: 1\"}]}");
    cJSON* missing = cJSON_CreateArray();
    int unresolved = RuleSecrets_Restore(r, stored, missing, NULL);
    /* actions[0] is telegram in stored, so its password is not reused. */
    CHECK(has_path(missing, "actions[0].password"), "type mismatch not restored");
    CHECK(strcmp(str_at(r, "actions", 1, "password"), "new-pw") == 0, "changed value kept");
    CHECK(strcmp(str_at(r, "actions", 1, "headers"), "authorization: Bearer abc\nX-Other: 1") == 0,
          "header matched case-insensitively: %s", str_at(r, "actions", 1, "headers"));
    CHECK(unresolved == 1, "unresolved %d", unresolved);
    cJSON_Delete(missing); cJSON_Delete(r); cJSON_Delete(stored);
}

static void test_headers_edge_cases(void) {
    cJSON* r = cJSON_Parse(
        "{\"actions\":[{\"type\":\"http_request\",\"headers\":\"\"},"
        "{\"type\":\"http_request\",\"headers\":\"Cookie: a=b\\r\\nAccept: */*\\n\"},"
        "{\"type\":\"http_request\",\"headers\":\"no colon line\\nKeep-Alive: 5\"}]}");
    int n = RuleSecrets_Mask(r);
    CHECK(n == 1, "masked %d, expected 1", n);
    CHECK(strcmp(str_at(r, "actions", 0, "headers"), "") == 0, "empty headers");
    CHECK(strcmp(str_at(r, "actions", 1, "headers"), "Cookie: " RULE_SECRET_MASK "\nAccept: */*\n") == 0,
          "cookie masked, rest kept: [%s]", str_at(r, "actions", 1, "headers"));
    CHECK(strcmp(str_at(r, "actions", 2, "headers"), "no colon line\nKeep-Alive: 5") == 0, "untouched");
    cJSON_Delete(r);
}

int main(void) {
    test_mask();
    test_round_trip();
    test_restore_without_stored();
    test_type_mismatch_and_changed_values();
    test_headers_edge_cases();
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("rule_secrets: all tests passed\n");
    return 0;
}
