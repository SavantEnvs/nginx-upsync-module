/* Fuzz the bundled cJSON parser the way ngx_http_upsync_module.c uses it: the consul/etcd
 * response body is a NUL-terminated buffer handed to cJSON_Parse, then walked by key. */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../src/ngx_http_json.h"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    char   *buf;
    cJSON  *root, *item, *dup;
    char   *out;
    int     i, n;

    buf = malloc(size + 1);
    if (buf == NULL) {
        return 0;
    }
    memcpy(buf, data, size);
    buf[size] = '\0';

    root = cJSON_Parse(buf);
    if (root == NULL) {
        (void) cJSON_GetErrorPtr();
        free(buf);
        return -1;
    }

    n = cJSON_GetArraySize(root);
    for (i = 0; i < n && i < 64; i++) {
        item = cJSON_GetArrayItem(root, i);
        if (item == NULL) {
            continue;
        }
        (void) cJSON_GetObjectItem(item, "Key");
        (void) cJSON_GetObjectItem(item, "Value");
        (void) cJSON_GetObjectItem(item, "Service");
        (void) cJSON_GetObjectItem(item, "Checks");
        (void) cJSON_GetObjectItem(item, "weight");
    }
    (void) cJSON_GetObjectItem(root, "node");
    (void) cJSON_GetObjectItem(root, "nodes");

    out = cJSON_Print(root);
    free(out);
    out = cJSON_PrintUnformatted(root);
    free(out);

    dup = cJSON_Duplicate(root, 1);
    cJSON_Delete(dup);

    cJSON_Delete(root);
    free(buf);
    return 0;
}
