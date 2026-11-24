#ifndef RULE_SECRETS_H
#define RULE_SECRETS_H

#include "../cJSON.h"

/* Credentials in a rule's triggers, conditions and actions (passwords,
 * tokens, webhook URLs and sensitive HTTP headers) are replaced with this
 * placeholder in exports unless secrets are explicitly included. */
#define RULE_SECRET_MASK "__REDACTED__"

/* Replace every secret in rule with RULE_SECRET_MASK, in place.
 * Returns the number of values masked. */
int RuleSecrets_Mask(cJSON* rule);

/* Resolve RULE_SECRET_MASK placeholders in an incoming rule, in place.
 *
 * A placeholder takes the value at the same place in stored (the saved copy
 * of the rule, or NULL) when the step there has the same type. Otherwise the
 * field is removed, or for a header line the line is dropped, and its path
 * (for example "actions[1].bot_token") is appended to missing. An
 * http_webhook trigger never keeps an empty token, because an empty token
 * matches every webhook call: it gets a new random token and its path is
 * appended to regenerated. missing and regenerated may be NULL.
 *
 * Returns the number of placeholders that could not be restored from stored. */
int RuleSecrets_Restore(cJSON* rule, const cJSON* stored, cJSON* missing, cJSON* regenerated);

#endif
