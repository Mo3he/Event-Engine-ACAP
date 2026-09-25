#ifndef _ENGINE_ACTIONS_H_
#define _ENGINE_ACTIONS_H_

#include "../cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Action executor. Actions run sequentially; "delay" suspends the sequence and
 * resumes it from a GLib timeout. String fields are template-expanded
 * ({{camera.name}}, {{trigger.KEY}}, ...) before use. The accepted type list
 * lives in validate_rule_json() in main.c.
 */

void  Actions_Init(void);
void  Actions_Execute(const char* rule_id, cJSON* actions_array, cJSON* trigger_data);
char* Actions_Expand_Template(const char* tmpl, cJSON* trigger_data); /* caller must free() */
void  Actions_Stop_Active_Siren(const char* rule_id); /* undo every while_active action of this rule */
void  Actions_ForEach_Active_Siren(int (*cb)(const char* rule_id, void* userdata), void* userdata);
void  Actions_Set_Proxy(const char* proxy); /* set SOCKS5 proxy for outbound HTTP (e.g. "socks5h://localhost:1055") */
void  Actions_Digest_Tick(void);           /* call from 1s timer to flush digest buffers */
int         Actions_Test(const char* type, cJSON* config); /* test a single action without a rule */
const char* Actions_Get_Last_Error(void);              /* error from last Actions_Test(); NULL if none */

#ifdef __cplusplus
}
#endif
#endif /* _ENGINE_ACTIONS_H_ */
