/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

#include <fluent-bit.h>
#include <monkey/mk_lib.h>
#include "flb_tests_runtime.h"

struct kube_test {
    flb_ctx_t *flb;
    mk_ctx_t *http;
};

#define KUBE_IP   "127.0.0.1"
#define KUBE_PORT "8002"
#define KUBE_URL  "http://" KUBE_IP ":" KUBE_PORT
#define DPATH     FLB_TESTS_DATA_PATH "/data/kubernetes/"

/*
 * Data files
 * ==========
 */
#define T_APACHE_LOGS          DPATH "apache-logs"
#define T_APACHE_LOGS_ANN      DPATH "apache-logs-annotated"
#define T_APACHE_LOGS_ANN_INV  DPATH "apache-logs-annotated-invalid"
#define T_JSON_LOGS            DPATH "json-logs"

static int file_to_buf(char *path, char **out_buf, size_t *out_size)
{
    int ret;
    long bytes;
    char *buf;
    FILE *fp;
    struct stat st;

    ret = stat(path, &st);
    if (ret == -1) {
        return -1;
    }

    fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    buf = flb_malloc(st.st_size);
    if (!buf) {
        flb_errno();
        fclose(fp);
        return -1;
    }

    bytes = fread(buf, st.st_size, 1, fp);
    if (bytes != 1) {
        flb_errno();
        flb_free(buf);
        fclose(fp);
        return -1;
    }

    fclose(fp);
    *out_buf = buf;
    *out_size = st.st_size;

    return 0;
}

static void cb_api_server_root(mk_request_t *request, void *data)
{
    int ret;
    char *pod;
    char meta[PATH_MAX];
    char *uri = NULL;
    char *meta_buf;
    size_t meta_size;

    uri = strndup(request->uri.data, request->uri.len);
    pod = strrchr(uri, '/');
    if (!pod) {
        goto not_found;
    }

    snprintf(meta, sizeof(meta) - 1, "%s%s.meta", DPATH, pod);
    flb_free(uri);

    ret = file_to_buf(meta, &meta_buf, &meta_size);
    if (ret == -1) {
        goto not_found;
    }

    mk_http_status(request, 200);
    mk_http_send(request, meta_buf, meta_size, NULL);
    flb_free(meta_buf);
    mk_http_done(request);
    return;

 not_found:
    mk_http_status(request, 404);
    mk_http_send(request, "Resource not found\n", 19, NULL);
    mk_http_done(request);

}

/* A simple fake Kubernetes API Server */
static mk_ctx_t *api_server_create(char *listen, char *tcp)
{
    int vid;
    int ret;
    char tmp[32];
    mk_ctx_t *ctx;

    /* Monkey HTTP Server context */
    ctx = mk_create();
    if (!ctx) {
        flb_error("[rt-filter_kube] error creating API Server");
        return NULL;
    }

    /* Bind */
    snprintf(tmp, sizeof(tmp) -1, "%s:%s", listen, tcp);
    mk_config_set(ctx, "Listen", tmp, NULL);

    /* Default Virtual Host */
    vid = mk_vhost_create(ctx, NULL);
    mk_vhost_set(ctx, vid, "Name", "rt-filter_kube", NULL);
    mk_vhost_handler(ctx, vid, "/", cb_api_server_root, NULL);

    ret = mk_start(ctx);
    if (ret != 0) {
        TEST_CHECK(ret != 0);
        flb_error("Fake API Server ERROR");
        mk_destroy(ctx);
        return NULL;
    }

    return ctx;
}

static void api_server_stop(mk_ctx_t *ctx)
{
    mk_stop(ctx);
    mk_destroy(ctx);
}

/* Given a target, lookup the .out file and return it content in a new buffer */
static char *get_out_file_content(char *target)
{
    int ret;
    char file[PATH_MAX];
    char *p;
    char *out_buf;
    size_t out_size;

    snprintf(file, sizeof(file) - 1, "%s.out", target);

    ret = file_to_buf(file, &out_buf, &out_size);
    if (ret != 0) {
        flb_error("no output file found '%s'", file);
        exit(EXIT_FAILURE);
    }

    /* Sanitize content, get rid of ending \n */
    p = out_buf + (out_size - 1);
    while (*p == '\n' || *p == '\r') p--;
    *++p = '\0';

    return out_buf;
}

static int cb_check_result(void *record, size_t size, void *data)
{
    char *target;
    char *out;
    char *check;

    target = (char *) data;
    out = get_out_file_content(target);
    if (!out) {
        exit(EXIT_FAILURE);
    }

    /*
     * Our validation is: check that the content of out file is found
     * in the output record.
     */
    check = strstr(record, out);
    TEST_CHECK(check != NULL);
    if (size > 0) {
        flb_free(record);
    }

    flb_free(out);
    return 0;
}

static struct kube_test *kube_test_create(char *target)
{
    int ret;
    int in_ffd;
    int filter_ffd;
    int out_ffd;
    char path[PATH_MAX];
    struct kube_test *ctx;
    struct flb_lib_out_cb cb_data;

    /* Compose path pattern based on target */
    snprintf(path, sizeof(path) - 1, "%s_default*.log", target);

    ctx = flb_malloc(sizeof(struct kube_test));
    if (!ctx) {
        flb_errno();
        TEST_CHECK(ctx != NULL);
        exit(EXIT_FAILURE);
    }

    ctx->http = api_server_create(KUBE_IP, KUBE_PORT);
    TEST_CHECK(ctx->http != NULL);
    if (!ctx->http) {
        exit(EXIT_FAILURE);
    }

    ctx->flb = flb_create();
    flb_service_set(ctx->flb,
                    "Flush", "1",
                    "Parsers_File", "../conf/parsers.conf",
                    NULL);
    in_ffd = flb_input(ctx->flb, "tail", NULL);
    ret = flb_input_set(ctx->flb, in_ffd,
                        "Tag", "kube.*",
                        "Path", path,
                        "Parser", "docker",
                        "Decode_Field", "json log",
                        NULL);
    TEST_CHECK(ret == 0);

    filter_ffd = flb_filter(ctx->flb, "kubernetes", NULL);
    ret = flb_filter_set(ctx->flb, filter_ffd,
                         "Match", "kube.*",
                         "Kube_URL", KUBE_URL,
                         "Merge_Log", "On",
                         "Regex_Parser", "filter-kube-test",
                         "k8s-logging.parser", "On",
                         NULL);

    /* Prepare output callback context*/
    cb_data.cb = cb_check_result;
    cb_data.data = target;

    /* Output */
    out_ffd = flb_output(ctx->flb, "lib", (void *) &cb_data);
    TEST_CHECK(out_ffd >= 0);
    flb_output_set(ctx->flb, out_ffd,
                   "Match", "kube.*",
                   "format", "json",
                   NULL);

    ret = flb_start(ctx->flb);
    TEST_CHECK(ret == 0);
    if (ret == -1) {
        exit(EXIT_FAILURE);
    }

    return ctx;
}

static void kube_test_destroy(struct kube_test *ctx)
{
    sleep(1);
    flb_stop(ctx->flb);
    flb_destroy(ctx->flb);
    api_server_stop(ctx->http);
    flb_free(ctx);
}

void flb_test_apache_logs()
{
    struct kube_test *ctx;

    ctx = kube_test_create(T_APACHE_LOGS);
    if (!ctx) {
        exit(EXIT_FAILURE);
    }
    kube_test_destroy(ctx);
}

void flb_test_apache_logs_annotated()
{
    struct kube_test *ctx;

    ctx = kube_test_create(T_APACHE_LOGS_ANN);
    if (!ctx) {
        exit(EXIT_FAILURE);
    }
    kube_test_destroy(ctx);
}

void flb_test_apache_logs_annotated_invalid()
{
    struct kube_test *ctx;

    ctx = kube_test_create(T_APACHE_LOGS_ANN_INV);
    if (!ctx) {
        exit(EXIT_FAILURE);
    }
    kube_test_destroy(ctx);
}

void flb_test_json_logs()
{
    struct kube_test *ctx;

    ctx = kube_test_create(T_JSON_LOGS);
    if (!ctx) {
        exit(EXIT_FAILURE);
    }
    kube_test_destroy(ctx);
}

TEST_LIST = {
    {"kube_apache_logs", flb_test_apache_logs},
    {"kube_apache_logs_annotated", flb_test_apache_logs_annotated},
    {"kube_apache_logs_annotated_invalid", flb_test_apache_logs_annotated_invalid},
    {"kube_json_logs", flb_test_json_logs},
    {NULL, NULL}
};