/* Known-answer tests for the two parsers bundled with nginx-upsync-module: the cJSON copy that
 * decodes consul/etcd replies and the joyent http_parser that splits their HTTP envelope.
 * Prints one PASS/FAIL line per check and a final "SUMMARY: N tests run; F failed". */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/ngx_http_json.h"
#include "../../src/ngx_http_parser.h"

static int  tests, failed;

#define CHECK(name, cond)                                         \
    do {                                                          \
        tests++;                                                  \
        if (cond) { printf("PASS %s\n", name); }                  \
        else { failed++; printf("FAIL %s (line %d)\n", name, __LINE__); } \
    } while (0)

typedef struct {
    char     url[256];
    char     status[64];
    char     field[8][64];
    char     value[8][128];
    int      nfields;
    int      last_was_value;
    char     body[256];
    int      begin, headers_done, complete;
} cap_t;

static void
append(char *dst, size_t cap, const char *at, size_t len)
{
    size_t  n = strlen(dst);

    if (n + len >= cap) {
        len = cap - n - 1;
    }
    memcpy(dst + n, at, len);
    dst[n + len] = '\0';
}

static int cb_begin(http_parser *p) { ((cap_t *) p->data)->begin++; return 0; }
static int cb_hdone(http_parser *p) { ((cap_t *) p->data)->headers_done++; return 0; }
static int cb_done(http_parser *p) { ((cap_t *) p->data)->complete++; return 0; }
static int cb_url(http_parser *p, const char *at, size_t n)
{ append(((cap_t *) p->data)->url, 256, at, n); return 0; }
static int cb_status(http_parser *p, const char *at, size_t n)
{ append(((cap_t *) p->data)->status, 64, at, n); return 0; }
static int cb_body(http_parser *p, const char *at, size_t n)
{ append(((cap_t *) p->data)->body, 256, at, n); return 0; }

static int
cb_field(http_parser *p, const char *at, size_t n)
{
    cap_t  *c = p->data;

    if (c->last_was_value || c->nfields == 0) {
        if (c->nfields < 8) {
            c->nfields++;
        }
    }
    c->last_was_value = 0;
    append(c->field[c->nfields - 1], 64, at, n);
    return 0;
}

static int
cb_value(http_parser *p, const char *at, size_t n)
{
    cap_t  *c = p->data;

    c->last_was_value = 1;
    append(c->value[c->nfields - 1], 128, at, n);
    return 0;
}

static size_t
run_http(enum http_parser_type type, const char *msg, http_parser *p, cap_t *c)
{
    http_parser_settings  s;

    memset(c, 0, sizeof(*c));
    http_parser_settings_init(&s);
    s.on_message_begin = cb_begin;
    s.on_url = cb_url;
    s.on_status = cb_status;
    s.on_header_field = cb_field;
    s.on_header_value = cb_value;
    s.on_headers_complete = cb_hdone;
    s.on_body = cb_body;
    s.on_message_complete = cb_done;
    http_parser_init(p, type);
    p->data = c;
    return http_parser_execute(p, &s, msg, strlen(msg));
}

static void
test_json(void)
{
    cJSON  *root, *a, *b, *c, *d, *dup, *arr, *kv;
    char   *out;
    const char *end = NULL;

    root = cJSON_Parse("{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.5}}");
    CHECK("json.parse.object", root != NULL && root->type == cJSON_Object);
    a = cJSON_GetObjectItem(root, "a");
    CHECK("json.int.type", a != NULL && a->type == cJSON_Number);
    CHECK("json.int.value", a != NULL && a->valueint == 1);
    b = cJSON_GetObjectItem(root, "b");
    CHECK("json.array.size", cJSON_GetArraySize(b) == 3);
    CHECK("json.array.true", cJSON_GetArrayItem(b, 0)->type == cJSON_True);
    CHECK("json.array.null", cJSON_GetArrayItem(b, 1)->type == cJSON_NULL);
    CHECK("json.array.string", strcmp(cJSON_GetArrayItem(b, 2)->valuestring, "x") == 0);
    c = cJSON_GetObjectItem(root, "c");
    d = cJSON_GetObjectItem(c, "d");
    CHECK("json.nested.double", d != NULL && d->valuedouble == 2.5);
    CHECK("json.key.case-insensitive", cJSON_GetObjectItem(root, "A") == a);
    CHECK("json.missing.key", cJSON_GetObjectItem(root, "zzz") == NULL);

    out = cJSON_PrintUnformatted(root);
    CHECK("json.print.unformatted",
          out != NULL && strcmp(out, "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.500000}}") == 0);
    free(out);

    dup = cJSON_Duplicate(root, 1);
    out = cJSON_PrintUnformatted(dup);
    CHECK("json.duplicate.roundtrip",
          out != NULL && strcmp(out, "{\"a\":1,\"b\":[true,null,\"x\"],\"c\":{\"d\":2.500000}}") == 0);
    free(out);
    cJSON_Delete(dup);
    cJSON_Delete(root);

    root = cJSON_Parse("{\"a\":");
    CHECK("json.truncated.rejected", root == NULL);
    CHECK("json.truncated.errorptr", cJSON_GetErrorPtr() != NULL);

    root = cJSON_Parse("[1,2,");
    CHECK("json.unterminated.array", root == NULL);

    root = cJSON_Parse("\"\\u00e9\\n\"");
    CHECK("json.unicode.escape",
          root != NULL && root->type == cJSON_String
          && strcmp(root->valuestring, "\xc3\xa9\n") == 0);
    cJSON_Delete(root);

    root = cJSON_Parse("[1e3,-12,0.5]");
    CHECK("json.numbers.exp", cJSON_GetArrayItem(root, 0)->valueint == 1000);
    CHECK("json.numbers.negative", cJSON_GetArrayItem(root, 1)->valueint == -12);
    CHECK("json.numbers.fraction", cJSON_GetArrayItem(root, 2)->valuedouble == 0.5);
    cJSON_Delete(root);

    root = cJSON_ParseWithOpts("[1] trailing", &end, 1);
    CHECK("json.trailing.rejected", root == NULL);

    arr = cJSON_Parse("[{\"Key\":\"upstreams/foo/127.0.0.1:8080\","
                      "\"Value\":\"eyJ3ZWlnaHQiOjV9\",\"ModifyIndex\":42}]");
    kv = cJSON_GetArrayItem(arr, 0);
    CHECK("json.consul.key",
          strcmp(cJSON_GetObjectItem(kv, "Key")->valuestring, "upstreams/foo/127.0.0.1:8080") == 0);
    CHECK("json.consul.value",
          strcmp(cJSON_GetObjectItem(kv, "Value")->valuestring, "eyJ3ZWlnaHQiOjV9") == 0);
    CHECK("json.consul.index", cJSON_GetObjectItem(kv, "ModifyIndex")->valueint == 42);
    cJSON_Delete(arr);

    root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "weight", 5);
    cJSON_AddStringToObject(root, "name", "srv");
    out = cJSON_PrintUnformatted(root);
    CHECK("json.build.print", out != NULL && strcmp(out, "{\"weight\":5,\"name\":\"srv\"}") == 0);
    free(out);
    cJSON_Delete(root);
}

static void
test_http(void)
{
    http_parser         p;
    cap_t               c;
    struct http_parser_url  u;
    const char         *url;
    size_t              n;

    n = run_http(HTTP_RESPONSE,
                 "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Consul-Index: 42\r\n\r\nhello",
                 &p, &c);
    CHECK("http.response.consumed", n == strlen(
          "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nX-Consul-Index: 42\r\n\r\nhello"));
    CHECK("http.response.no-error", HTTP_PARSER_ERRNO(&p) == HPE_OK);
    CHECK("http.response.status-code", p.status_code == 200);
    CHECK("http.response.status-text", strcmp(c.status, "OK") == 0);
    CHECK("http.response.version", p.http_major == 1 && p.http_minor == 1);
    CHECK("http.response.header-count", c.nfields == 2);
    CHECK("http.response.header-0", strcmp(c.field[0], "Content-Length") == 0
                                    && strcmp(c.value[0], "5") == 0);
    CHECK("http.response.header-1", strcmp(c.field[1], "X-Consul-Index") == 0
                                    && strcmp(c.value[1], "42") == 0);
    CHECK("http.response.content-length", p.content_length == 0 || p.content_length == 5);
    CHECK("http.response.body", strcmp(c.body, "hello") == 0);
    CHECK("http.response.callbacks", c.begin == 1 && c.headers_done == 1 && c.complete == 1);
    CHECK("http.response.keepalive", http_should_keep_alive(&p) == 1);

    run_http(HTTP_RESPONSE,
             "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n3\r\n!!!\r\n0\r\n\r\n",
             &p, &c);
    CHECK("http.chunked.body", strcmp(c.body, "hello!!!") == 0);
    CHECK("http.chunked.complete", c.complete == 1 && HTTP_PARSER_ERRNO(&p) == HPE_OK);

    run_http(HTTP_RESPONSE, "HTTP/1.0 404 Not Found\r\nConnection: close\r\n\r\n", &p, &c);
    CHECK("http.404.status", p.status_code == 404 && strcmp(c.status, "Not Found") == 0);
    CHECK("http.1.0.close", http_should_keep_alive(&p) == 0);

    run_http(HTTP_REQUEST, "GET /v1/kv/upstreams/foo?recurse HTTP/1.1\r\nHost: consul\r\n\r\n", &p, &c);
    CHECK("http.request.method", p.method == HTTP_GET);
    CHECK("http.request.url", strcmp(c.url, "/v1/kv/upstreams/foo?recurse") == 0);
    CHECK("http.request.host", strcmp(c.field[0], "Host") == 0 && strcmp(c.value[0], "consul") == 0);

    run_http(HTTP_RESPONSE, "XTTP/1.1 200 OK\r\n\r\n", &p, &c);
    CHECK("http.bad-constant", HTTP_PARSER_ERRNO(&p) == HPE_INVALID_CONSTANT);
    CHECK("http.errno-name", strcmp(http_errno_name(HPE_INVALID_CONSTANT), "HPE_INVALID_CONSTANT") == 0);

    run_http(HTTP_RESPONSE, "HTTP/1.1 200 OK\r\nContent-Length: 1x\r\n\r\n", &p, &c);
    CHECK("http.bad-content-length", HTTP_PARSER_ERRNO(&p) == HPE_INVALID_CONTENT_LENGTH);

    CHECK("http.method-str", strcmp(http_method_str(HTTP_POST), "POST") == 0);

    url = "http://user@example.com:8080/path/x?q=1#frag";
    memset(&u, 0, sizeof(u));
    CHECK("http.url.ok", http_parser_parse_url(url, strlen(url), 0, &u) == 0);
    CHECK("http.url.schema", (u.field_set & (1 << UF_SCHEMA))
          && u.field_data[UF_SCHEMA].len == 4 && strncmp(url + u.field_data[UF_SCHEMA].off, "http", 4) == 0);
    CHECK("http.url.host", (u.field_set & (1 << UF_HOST))
          && u.field_data[UF_HOST].len == 11 && strncmp(url + u.field_data[UF_HOST].off, "example.com", 11) == 0);
    CHECK("http.url.port", u.port == 8080);
    CHECK("http.url.path", u.field_data[UF_PATH].len == 7
          && strncmp(url + u.field_data[UF_PATH].off, "/path/x", 7) == 0);
    CHECK("http.url.query", u.field_data[UF_QUERY].len == 3
          && strncmp(url + u.field_data[UF_QUERY].off, "q=1", 3) == 0);
    CHECK("http.url.userinfo", u.field_data[UF_USERINFO].len == 4
          && strncmp(url + u.field_data[UF_USERINFO].off, "user", 4) == 0);
    memset(&u, 0, sizeof(u));
    CHECK("http.url.invalid", http_parser_parse_url("http:///toto", 12, 0, &u) != 0);
}

int
main(void)
{
    test_json();
    test_http();
    printf("SUMMARY: %d tests run; %d failed\n", tests, failed);
    return failed != 0;
}
