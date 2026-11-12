/* Fuzz the bundled joyent http_parser (v2.4.2). The upsync module feeds it the consul/etcd HTTP
 * response (HTTP_RESPONSE); request, auto-detect and URL parsing are reachable through the same
 * library. The first byte picks the entry point and whether the rest arrives in one piece or two. */
#include <stdint.h>
#include <string.h>

#include "../src/ngx_http_parser.h"

static unsigned long  sink;

static int
on_cb(http_parser *p)
{
    sink += p->nread;
    return 0;
}

static int
on_data(http_parser *p, const char *at, size_t len)
{
    size_t  i;

    for (i = 0; i < len; i++) {
        sink += (unsigned char) at[i];
    }
    return 0;
}

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    http_parser           parser;
    http_parser_settings  settings;
    struct http_parser_url  u;
    const char           *buf;
    size_t                len, first;
    unsigned              mode;

    if (size < 2) {
        return -1;
    }

    mode = data[0] & 3;
    first = data[1];
    buf = (const char *) data + 2;
    len = size - 2;

    if (mode == 3) {
        memset(&u, 0, sizeof(u));
        sink += http_parser_parse_url(buf, len, (data[0] >> 2) & 1, &u);
        sink += u.field_set + u.port;
        return 0;
    }

    http_parser_settings_init(&settings);
    settings.on_message_begin = on_cb;
    settings.on_url = on_data;
    settings.on_status = on_data;
    settings.on_header_field = on_data;
    settings.on_header_value = on_data;
    settings.on_headers_complete = on_cb;
    settings.on_body = on_data;
    settings.on_message_complete = on_cb;

    http_parser_init(&parser, mode == 0 ? HTTP_RESPONSE
                              : mode == 1 ? HTTP_REQUEST : HTTP_BOTH);

    if (first > len) {
        first = len;
    }

    if (http_parser_execute(&parser, &settings, buf, first) == first) {
        http_parser_execute(&parser, &settings, buf + first, len - first);
    }
    http_parser_execute(&parser, &settings, NULL, 0);

    sink += http_should_keep_alive(&parser) + http_body_is_final(&parser);
    sink += (unsigned long) (uintptr_t) http_errno_name(HTTP_PARSER_ERRNO(&parser));
    return 0;
}
