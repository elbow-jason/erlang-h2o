// -*- mode: c; tab-width: 4; indent-tabs-mode: nil; st-rulers: [132] -*-
// vim: ts=4 sw=4 ft=c et

#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#ifdef __GLIBC__
#include <execinfo.h>
#endif

#include "server.h"

#include <yoml-parser.h>
// #include "neverbleed.h"
#include <h2o.h>
#include <h2o/configurator.h>
#include <h2o/http1.h>
#include <h2o/http2.h>
#include <h2o/serverutil.h>
// #include "standalone.h"

// #include "filter.h"
#include "handler.h"
// #include "logger.h"

#ifdef TCP_FASTOPEN
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 4096
#else
#define H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE 0
#endif

/* config commands */
static yoml_t *load_config(yoml_parse_args_t *parse_args, yaml_char_t *input, size_t size);
// static int on_config_erlang_filter(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_erlang_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_fake_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
// static int on_config_erlang_logger(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
// static int on_config_erlang_websocket_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_error_log(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_listen(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_listen_enter(h2o_configurator_t *configurator, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_listen_exit(h2o_configurator_t *configurator, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_max_connections(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_num_name_resolution_threads(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_num_ocsp_updaters(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_num_threads(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_tcp_fastopen(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);
static int on_config_temp_buffer_path(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node);

/* listen functions */
static h2o_nif_cfg_listen_t *add_listener(h2o_nif_config_t *config, int fd, struct sockaddr *addr, socklen_t addrlen, int is_global,
                                          int proxy_protocol);
static h2o_nif_cfg_listen_t *find_listener(h2o_nif_config_t *config, struct sockaddr *addr, socklen_t addrlen);
static int open_tcp_listener(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node, const char *hostname,
                             const char *servname, int domain, int type, int protocol, struct sockaddr *addr, socklen_t addrlen);
static int open_unix_listener(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node,
                              struct sockaddr_un *sa);
static void set_cloexec(int fd);

// #define H2O_NIF_SERVER(c)   (h2o_nif_server_t *)((void *)(c) - sizeof (h2o_nif_port_t))

int
h2o_nif_config_init(h2o_nif_config_t *config)
{
    h2o_nif_server_t *server = H2O_STRUCT_FROM_MEMBER(h2o_nif_server_t, config, config);
    TRACE_F("h2o_nif_config_init:%s:%d server=%p\n", __FILE__, __LINE__, server);
    if (config == NULL) {
        return 0;
    }
    (void)h2o_config_init(&config->globalconf);
    config->listeners = NULL;
    config->num_listeners = 0;
    config->error_log = NULL;
    config->error_log_fd = -1;
    config->max_connections = 1024;
    config->num_threads = h2o_numproc();
    config->tfo_queues = H2O_DEFAULT_LENGTH_TCP_FASTOPEN_QUEUE;
    config->env = NULL;
    /* setup configurators */
    {
        h2o_configurator_t *c = h2o_configurator_create(&config->globalconf, sizeof(*c));
        c->enter = on_config_listen_enter;
        c->exit = on_config_listen_exit;
        (void)h2o_configurator_define_command(c, "listen", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_HOST,
                                              on_config_listen);
    }
    {
        h2o_configurator_t *c = h2o_configurator_create(&config->globalconf, sizeof(*c));
        (void)h2o_configurator_define_command(c, "error-log", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                              on_config_error_log);
        (void)h2o_configurator_define_command(c, "max-connections", H2O_CONFIGURATOR_FLAG_GLOBAL, on_config_max_connections);
        (void)h2o_configurator_define_command(c, "num-name-resolution-threads", H2O_CONFIGURATOR_FLAG_GLOBAL,
                                              on_config_num_name_resolution_threads);
        (void)h2o_configurator_define_command(c, "num-ocsp-updaters",
                                              H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
                                              on_config_num_ocsp_updaters);
        (void)h2o_configurator_define_command(c, "num-threads", H2O_CONFIGURATOR_FLAG_GLOBAL, on_config_num_threads);
        (void)h2o_configurator_define_command(c, "tcp-fastopen", H2O_CONFIGURATOR_FLAG_GLOBAL, on_config_tcp_fastopen);
        (void)h2o_configurator_define_command(
            c, "temp-buffer-path", H2O_CONFIGURATOR_FLAG_GLOBAL | H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR, on_config_temp_buffer_path);
    }
    (void)h2o_access_log_register_configurator(&config->globalconf);
    (void)h2o_compress_register_configurator(&config->globalconf);
    (void)h2o_file_register_configurator(&config->globalconf);
    (void)h2o_status_register_configurator(&config->globalconf);
    {
        h2o_configurator_t *c = h2o_configurator_create(&config->globalconf, sizeof(*c));
        // (void)h2o_configurator_define_command(
        //     c, "erlang.filter", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED |
        //     H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
        //     on_config_erlang_filter);
        (void)h2o_configurator_define_command(
            c, "erlang.handler", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED |
            H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
            on_config_erlang_handler);
        (void)h2o_configurator_define_command(
            c, "fake.handler", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED |
            H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
            on_config_fake_handler);
        // (void)h2o_configurator_define_command(
        //     c, "erlang.logger", H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED |
        //     H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
        //     on_config_erlang_logger);
        // (void)h2o_configurator_define_command(c, "erlang.websocket",
        //                                       H2O_CONFIGURATOR_FLAG_PATH | H2O_CONFIGURATOR_FLAG_DEFERRED |
        //                                           H2O_CONFIGURATOR_FLAG_EXPECT_SCALAR,
        //                                       on_config_erlang_websocket_handler);
    }
    // (void)h2o_config_register_simple_status_handler(&config->globalconf, (h2o_iovec_t){H2O_STRLIT("main")},
    //                                                 h2o_nif_server_on_extra_status);
    return 1;
}

void
h2o_nif_config_dispose(h2o_nif_config_t *config)
{
    (void)h2o_config_dispose(&config->globalconf);
    (void)memset(config, 0, sizeof(*config));
    return;
}

int
h2o_nif_config_get(ErlNifEnv *env, h2o_nif_config_t *config, ERL_NIF_TERM *out)
{
    ERL_NIF_TERM list[7];
    int i = 0;

    (void)enif_mutex_lock(h2o_nif_mutex);
    size_t num_name_resolution_threads = h2o_hostinfo_max_threads;
    size_t num_ocsp_updaters = h2o_ocsp_updater_semaphore._capacity;
    char temp_buffer_path[sizeof(h2o_socket_buffer_mmap_settings.fn_template)];
    (void)memset(temp_buffer_path, 0, sizeof(h2o_socket_buffer_mmap_settings.fn_template));
    size_t temp_buffer_path_len =
        strrchr(h2o_socket_buffer_mmap_settings.fn_template, '/') - h2o_socket_buffer_mmap_settings.fn_template;
    (void)memcpy(temp_buffer_path, h2o_socket_buffer_mmap_settings.fn_template, temp_buffer_path_len);
    (void)enif_mutex_unlock(h2o_nif_mutex);

#define ERL_NIF_LITBIN(s) ((ErlNifBinary){.size = sizeof(s) - 1, .data = (unsigned char *)(s)})

    /* error-log */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("error-log");
        if (config->error_log == NULL) {
            list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), ATOM_nil);
        } else {
            ErlNifBinary val = {strlen(config->error_log), (unsigned char *)config->error_log};
            list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_binary(env, &val));
        }
    }
    /* max-connections */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("max-connections");
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_int(env, config->max_connections));
    }
    /* num-name-resolution-threads */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("num-name-resolution-threads");
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_ulong(env, num_name_resolution_threads));
    }
    /* num-ocsp-updaters */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("num-ocsp-updaters");
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_ulong(env, num_ocsp_updaters));
    }
    /* num-threads */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("num-threads");
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_ulong(env, config->num_threads));
    }
    /* tcp-fastopen */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("tcp-fastopen");
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_ulong(env, config->tfo_queues));
    }
    /* temp-buffer-path */
    {
        ErlNifBinary key = ERL_NIF_LITBIN("temp-buffer-path");
        ErlNifBinary val = {temp_buffer_path_len, (unsigned char *)temp_buffer_path};
        list[i++] = enif_make_tuple2(env, enif_make_binary(env, &key), enif_make_binary(env, &val));
    }

#undef ERL_NIF_LITBIN

    *out = enif_make_list_from_array(env, list, i);

    return 1;
}

int
h2o_nif_config_set(ErlNifEnv *env, h2o_nif_port_t *port, h2o_nif_config_t *config, ErlNifBinary *input, ERL_NIF_TERM *out)
{
    yoml_t *yoml;
    yoml_parse_args_t parse_args = {
        "h2o_nif.yaml", /* filename */
        NULL,           /* mem_set */
        {NULL, NULL}    /* resolve_tag */
    };
    if ((yoml = load_config(&parse_args, (yaml_char_t *)input->data, (size_t)input->size)) == NULL) {
        TRACE_F("error loading YAML\n");
        return 0;
    }
    config->env = env;
    if (h2o_configurator_apply(&config->globalconf, yoml, 0) != 0) {
        TRACE_F("error applying configurator\n");
        (void)yoml_free(yoml, NULL);
        config->env = NULL;
        return 0;
    }
    config->env = NULL;
    (void)yoml_free(yoml, NULL);
    (void)h2o_nif_port_cas_or_state(port, H2O_NIF_PORT_STATE_CONFIGURED);
    *out = ATOM_ok;
    return 1;
}

static yoml_t *
load_config(yoml_parse_args_t *parse_args, yaml_char_t *input, size_t size)
{
    yaml_parser_t parser;
    yoml_t *yoml;

    yaml_parser_initialize(&parser);
    yaml_parser_set_input_string(&parser, input, size);

    yoml = yoml_parse_document(&parser, NULL, parse_args);

    if (yoml == NULL) {
        TRACE_F("failed to parse configuration file %s line %d: %s\n", parse_args->filename, (int)parser.problem_mark.line + 1,
                parser.problem);
    }

    yaml_parser_delete(&parser);

    return yoml;
}

// static int
// on_config_erlang_filter(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
// {
//     TRACE_F("on_config_erlang_filter:%s:%d\n", __FILE__, __LINE__);

//     if (node->type != YOML_TYPE_SCALAR) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.filter` is not a scalar");
//         return -1;
//     }

//     h2o_iovec_t ref_iovec;
//     ref_iovec = h2o_decode_base64url(NULL, node->data.scalar, strlen(node->data.scalar));
//     if (ref_iovec.base == NULL) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.filter` invalid Base64 encoding");
//         return -1;
//     }
//     h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
//     ERL_NIF_TERM ref_term;
//     if (!enif_binary_to_term(config->env, (const unsigned char *)ref_iovec.base, ref_iovec.len, &ref_term,
//     ERL_NIF_BIN2TERM_SAFE)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.filter` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     if (!enif_is_ref(config->env, ref_term)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.filter` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     (void)h2o_nif_filter_register(config->env, ctx->pathconf, ref_term);
//     (void)free(ref_iovec.base);

//     return 0;
// }

static int
on_config_erlang_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_erlang_handler:%s:%d\n", __FILE__, __LINE__);

    if (node->type != YOML_TYPE_SCALAR) {
        (void)h2o_configurator_errprintf(cmd, node, "`erlang.handler` is not a scalar");
        return -1;
    }

    h2o_iovec_t ref_iovec;
    ref_iovec = h2o_decode_base64url(NULL, node->data.scalar, strlen(node->data.scalar));
    if (ref_iovec.base == NULL) {
        (void)h2o_configurator_errprintf(cmd, node, "`erlang.handler` invalid Base64 encoding");
        return -1;
    }
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    ERL_NIF_TERM ref_term;
    if (!enif_binary_to_term(config->env, (const unsigned char *)ref_iovec.base, ref_iovec.len, &ref_term,
    ERL_NIF_BIN2TERM_SAFE)) {
        (void)h2o_configurator_errprintf(cmd, node, "`erlang.handler` must be an erlang reference");
        (void)free(ref_iovec.base);
        return -1;
    }
    if (!enif_is_ref(config->env, ref_term)) {
        (void)h2o_configurator_errprintf(cmd, node, "`erlang.handler` must be an erlang reference");
        (void)free(ref_iovec.base);
        return -1;
    }
    (void)h2o_nif_handler_register(config->env, ctx->pathconf, ref_term, H2O_NIF_HANDLER_HTTP);
    (void)free(ref_iovec.base);

    return 0;
}

struct _fake_on_req_s {
    h2o_req_t *req;
    int x;
};

static void
_fake_on_req(void *data)
{
    struct _fake_on_req_s *ctx = (struct _fake_on_req_s *)data;
    h2o_req_t *req = ctx->req;
    if (ctx->x++ == 1) {
        req->res.status = 200;
        (void)h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("content-length"), 1, NULL, H2O_STRLIT("10"));
        (void)h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("content-type"), 1, NULL, H2O_STRLIT("text/plain"));
        (void)h2o_send_inline(req, H2O_STRLIT("Plain Text"));
        (void)enif_free(data);
        return;
    }
    h2o_nif_srv_thread_ctx_t *thread_ctx = (h2o_nif_srv_thread_ctx_t *)req->conn->ctx;
    (void)h2o_nif_ipc_send(thread_ctx->thread->ipc_queue, _fake_on_req, (void *)ctx);
}

static int
fake_on_req(h2o_handler_t *handler, h2o_req_t *req)
{
    (void)handler;

    struct _fake_on_req_s *ctx = enif_alloc(sizeof(struct _fake_on_req_s));
    assert(ctx != NULL);

    ctx->req = req;
    ctx->x = 0;

    h2o_nif_srv_thread_ctx_t *thread_ctx = (h2o_nif_srv_thread_ctx_t *)req->conn->ctx;
    (void)h2o_nif_ipc_send(thread_ctx->thread->ipc_queue, _fake_on_req, (void *)ctx);

    // req->res.status = 200;
    // (void)h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("content-length"), 1, NULL, H2O_STRLIT("10"));
    // (void)h2o_add_header_by_str(&req->pool, &req->res.headers, H2O_STRLIT("content-type"), 1, NULL, H2O_STRLIT("text/plain"));
    // (void)h2o_send_inline(req, H2O_STRLIT("Plain Text"));

    return 0;
}

static int
on_config_fake_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_fake_handler:%s:%d\n", __FILE__, __LINE__);

    if (node->type != YOML_TYPE_SCALAR) {
        (void)h2o_configurator_errprintf(cmd, node, "`fake.handler` is not a scalar");
        return -1;
    }

    h2o_handler_t *handler = (h2o_handler_t *)h2o_create_handler(ctx->pathconf, sizeof(*handler));
    assert(handler != NULL);
    handler->on_req = fake_on_req;

    return 0;
}

// static int
// on_config_erlang_logger(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
// {
//     TRACE_F("on_config_erlang_logger:%s:%d\n", __FILE__, __LINE__);

//     if (node->type != YOML_TYPE_SCALAR) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.logger` is not a scalar");
//         return -1;
//     }

//     h2o_iovec_t ref_iovec;
//     ref_iovec = h2o_decode_base64url(NULL, node->data.scalar, strlen(node->data.scalar));
//     if (ref_iovec.base == NULL) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.logger` invalid Base64 encoding");
//         return -1;
//     }
//     h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
//     ERL_NIF_TERM ref_term;
//     if (!enif_binary_to_term(config->env, (const unsigned char *)ref_iovec.base, ref_iovec.len, &ref_term,
//     ERL_NIF_BIN2TERM_SAFE)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.logger` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     if (!enif_is_ref(config->env, ref_term)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.logger` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     (void)h2o_nif_logger_register(config->env, ctx->pathconf, ref_term);
//     (void)free(ref_iovec.base);

//     return 0;
// }

// static int
// on_config_erlang_websocket_handler(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
// {
//     TRACE_F("on_config_erlang_websocket_handler:%s:%d\n", __FILE__, __LINE__);

//     if (node->type != YOML_TYPE_SCALAR) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.websocket` is not a scalar");
//         return -1;
//     }

//     h2o_iovec_t ref_iovec;
//     ref_iovec = h2o_decode_base64url(NULL, node->data.scalar, strlen(node->data.scalar));
//     if (ref_iovec.base == NULL) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.websocket` invalid Base64 encoding");
//         return -1;
//     }
//     h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
//     ERL_NIF_TERM ref_term;
//     if (!enif_binary_to_term(config->env, (const unsigned char *)ref_iovec.base, ref_iovec.len, &ref_term,
//     ERL_NIF_BIN2TERM_SAFE)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.websocket` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     if (!enif_is_ref(config->env, ref_term)) {
//         (void)h2o_configurator_errprintf(cmd, node, "`erlang.websocket` must be an erlang reference");
//         (void)free(ref_iovec.base);
//         return -1;
//     }
//     (void)h2o_nif_handler_register(config->env, ctx->pathconf, ref_term, H2O_NIF_HANDLER_TYPE_WEBSOCKET);
//     (void)free(ref_iovec.base);

//     return 0;
// }

static int
on_config_error_log(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_error_log:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    if (node->data.scalar[0] == 0) {
        config->error_log = NULL;
    } else {
        config->error_log = h2o_strdup(NULL, node->data.scalar, SIZE_MAX).base;
    }
    return 0;
}

static int
on_config_listen(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_listen:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    const char *hostname = NULL;
    const char *servname = NULL;
    const char *type = "tcp";
    yoml_t *ssl_node = NULL;
    int proxy_protocol = 0;

    /* fetch servname (and hostname) */
    switch (node->type) {
    case YOML_TYPE_SCALAR:
        servname = node->data.scalar;
        break;
    case YOML_TYPE_MAPPING: {
        yoml_t *t;
        if ((t = yoml_get(node, "host")) != NULL) {
            if (t->type != YOML_TYPE_SCALAR) {
                (void)h2o_configurator_errprintf(cmd, t, "`host` is not a string");
                return -1;
            }
            hostname = t->data.scalar;
        }
        if ((t = yoml_get(node, "port")) == NULL) {
            (void)h2o_configurator_errprintf(cmd, node, "cannot find mandatory property `port`");
            return -1;
        }
        if (t->type != YOML_TYPE_SCALAR) {
            (void)h2o_configurator_errprintf(cmd, node, "`port` is not a string");
            return -1;
        }
        servname = t->data.scalar;
        if ((t = yoml_get(node, "type")) != NULL) {
            if (t->type != YOML_TYPE_SCALAR) {
                (void)h2o_configurator_errprintf(cmd, t, "`type` is not a string");
                return -1;
            }
            type = t->data.scalar;
        }
        if ((t = yoml_get(node, "ssl")) != NULL)
            ssl_node = t;
        if ((t = yoml_get(node, "proxy-protocol")) != NULL) {
            if (t->type != YOML_TYPE_SCALAR) {
                (void)h2o_configurator_errprintf(cmd, node, "`proxy-protocol` must be a string");
                return -1;
            }
            if (strcasecmp(t->data.scalar, "ON") == 0) {
                proxy_protocol = 1;
            } else if (strcasecmp(t->data.scalar, "OFF") == 0) {
                proxy_protocol = 0;
            } else {
                (void)h2o_configurator_errprintf(cmd, node, "value of `proxy-protocol` must be either of: ON,OFF");
                return -1;
            }
        }
    } break;
    default:
        (void)h2o_configurator_errprintf(cmd, node,
                                         "value must be a string or a mapping (with keys: `port` and optionally `host`)");
        return -1;
    }

    if (strcmp(type, "unix") == 0) {

        /* unix socket */
        struct sockaddr_un sa;
        int listener_is_new;
        h2o_nif_cfg_listen_t *listener;
        /* build sockaddr */
        (void)memset(&sa, 0, sizeof(sa));
        if (strlen(servname) >= sizeof(sa.sun_path)) {
            (void)h2o_configurator_errprintf(cmd, node, "path:%s is too long as a unix socket name", servname);
            return -1;
        }
        sa.sun_family = AF_UNIX;
        (void)strcpy(sa.sun_path, servname);
        /* find existing listener or create a new one */
        listener_is_new = 0;
        if ((listener = find_listener(config, (void *)&sa, sizeof(sa))) == NULL) {
            int fd = -1;
            if ((fd = open_unix_listener(cmd, ctx, node, &sa)) == -1)
                return -1;
            listener = add_listener(config, fd, (struct sockaddr *)&sa, sizeof(sa), ctx->hostconf == NULL, proxy_protocol);
            listener_is_new = 1;
        } else if (listener->proxy_protocol != proxy_protocol) {
            goto ProxyConflict;
        }
        // if (listener_setup_ssl(cmd, ctx, node, ssl_node, listener, listener_is_new) != 0)
        //  return -1;
        if (listener->hosts != NULL && ctx->hostconf != NULL)
            (void)h2o_append_to_null_terminated_list((void *)&listener->hosts, ctx->hostconf);

    } else if (strcmp(type, "tcp") == 0) {

        /* TCP socket */
        struct addrinfo hints;
        struct addrinfo *res;
        struct addrinfo *ai;
        int error;
        /* call getaddrinfo */
        (void)memset(&hints, 0, sizeof(hints));
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;
        hints.ai_flags = AI_ADDRCONFIG | AI_NUMERICSERV | AI_PASSIVE;
        if ((error = getaddrinfo(hostname, servname, &hints, &res)) != 0) {
            (void)h2o_configurator_errprintf(cmd, node, "failed to resolve the listening address: %s", gai_strerror(error));
            return -1;
        } else if (res == NULL) {
            (void)h2o_configurator_errprintf(cmd, node,
                                             "failed to resolve the listening address: getaddrinfo returned an empty list");
            return -1;
        }
        /* listen to the returned addresses */
        for (ai = res; ai != NULL; ai = ai->ai_next) {
            h2o_nif_cfg_listen_t *listener = find_listener(config, ai->ai_addr, ai->ai_addrlen);
            int listener_is_new = 0;
            if (listener == NULL) {
                int fd = -1;
                if ((fd = open_tcp_listener(cmd, ctx, node, hostname, servname, ai->ai_family, ai->ai_socktype, ai->ai_protocol,
                                            ai->ai_addr, ai->ai_addrlen)) == -1) {
                    (void)freeaddrinfo(res);
                    return -1;
                }
                listener = add_listener(config, fd, ai->ai_addr, ai->ai_addrlen, ctx->hostconf == NULL, proxy_protocol);
                listener_is_new = 1;
            } else if (listener->proxy_protocol != proxy_protocol) {
                (void)freeaddrinfo(res);
                goto ProxyConflict;
            }
            // if (listener_setup_ssl(cmd, ctx, node, ssl_node, listener, listener_is_new) != 0) {
            //  (void) freeaddrinfo(res);
            //  return -1;
            // }
            if (listener->hosts != NULL && ctx->hostconf != NULL)
                (void)h2o_append_to_null_terminated_list((void *)&listener->hosts, ctx->hostconf);
        }
        /* release res */
        freeaddrinfo(res);

    } else {

        (void)h2o_configurator_errprintf(cmd, node, "unknown listen type: %s", type);
        return -1;
    }

    return 0;

ProxyConflict:
    h2o_configurator_errprintf(cmd, node, "`proxy-protocol` cannot be turned %s, already defined as opposite",
                               proxy_protocol ? "on" : "off");
    return -1;
}

static int
on_config_listen_enter(h2o_configurator_t *configurator, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_listen_enter:%s:%d\n", __FILE__, __LINE__);
    (void)configurator; // Unused
    (void)ctx;          // Unused
    (void)node;         // Unused
    return 0;
}

static int
on_config_listen_exit(h2o_configurator_t *configurator, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_listen_exit:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    if (ctx->pathconf != NULL) {
        /* skip */
    } else if (ctx->hostconf == NULL) {
        /* at global level: bind all hostconfs to the global-level listeners */
        size_t i;
        for (i = 0; i != config->num_listeners; ++i) {
            h2o_nif_cfg_listen_t *listener = config->listeners[i];
            if (listener->hosts == NULL) {
                listener->hosts = config->globalconf.hosts;
            }
        }
    } else if (ctx->pathconf == NULL) {
        /* at host-level */
        if (config->num_listeners == 0) {
            h2o_configurator_errprintf(
                NULL, node,
                "mandatory configuration directive `listen` does not exist, neither at global level or at this host level");
            return -1;
        }
    }

    return 0;
}

static int
on_config_max_connections(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_max_connections:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    return h2o_configurator_scanf(cmd, node, "%d", &config->max_connections);
}

static int
on_config_num_name_resolution_threads(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_num_name_resolution_threads:%s:%d\n", __FILE__, __LINE__);
    (void)enif_mutex_lock(h2o_nif_mutex);
    if (h2o_configurator_scanf(cmd, node, "%zu", &h2o_hostinfo_max_threads) != 0) {
        (void)enif_mutex_unlock(h2o_nif_mutex);
        return -1;
    }
    if (h2o_hostinfo_max_threads == 0) {
        h2o_configurator_errprintf(cmd, node, "num-name-resolution-threads must be >=1");
        (void)enif_mutex_unlock(h2o_nif_mutex);
        return -1;
    }
    (void)enif_mutex_unlock(h2o_nif_mutex);
    return 0;
}

static int
on_config_num_ocsp_updaters(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_num_ocsp_updaters:%s:%d\n", __FILE__, __LINE__);
    ssize_t n;
    if (h2o_configurator_scanf(cmd, node, "%zd", &n) != 0) {
        return -1;
    }
    if (n <= 0) {
        (void)h2o_configurator_errprintf(cmd, node, "num-ocsp-updaters must be >=1");
        return -1;
    }
    (void)enif_mutex_lock(h2o_nif_mutex);
    (void)h2o_sem_set_capacity(&h2o_ocsp_updater_semaphore, n);
    (void)enif_mutex_unlock(h2o_nif_mutex);
    return 0;
}

static int
on_config_num_threads(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_num_threads:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    if (h2o_configurator_scanf(cmd, node, "%zu", &config->num_threads) != 0) {
        return -1;
    }
    if (config->num_threads == 0) {
        (void)h2o_configurator_errprintf(cmd, node, "num-threads must be >=1");
        return -1;
    }
    return 0;
}

static int
on_config_tcp_fastopen(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    TRACE_F("on_config_tcp_fastopen:%s:%d\n", __FILE__, __LINE__);
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    if (h2o_configurator_scanf(cmd, node, "%d", &config->tfo_queues) != 0) {
        return -1;
    }
#ifndef TCP_FASTOPEN
    if (config->tfo_queues != 0) {
        h2o_configurator_errprintf(cmd, node, "[warning] ignoring the value; the platform does not support TCP_FASTOPEN");
        config->tfo_queues = 0;
    }
#endif
    return 0;
}

static int
on_config_temp_buffer_path(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node)
{
    (void)enif_mutex_lock(h2o_nif_mutex);
    char buf[sizeof(h2o_socket_buffer_mmap_settings.fn_template)];
    int len = snprintf(buf, sizeof(buf), "%s%s", node->data.scalar, strrchr(h2o_socket_buffer_mmap_settings.fn_template, '/'));
    if (len >= sizeof(buf)) {
        (void)h2o_configurator_errprintf(cmd, node, "path is too long");
        (void)enif_mutex_unlock(h2o_nif_mutex);
        return -1;
    }
    (void)strcpy(h2o_socket_buffer_mmap_settings.fn_template, buf);
    (void)enif_mutex_unlock(h2o_nif_mutex);
    return 0;
}

/* listen functions */

static h2o_nif_cfg_listen_t *
add_listener(h2o_nif_config_t *config, int fd, struct sockaddr *addr, socklen_t addrlen, int is_global, int proxy_protocol)
{
    h2o_nif_cfg_listen_t *listener = enif_alloc(sizeof(*listener));

    (void)memcpy(&listener->addr, addr, addrlen);
    listener->fd = fd;
    listener->addrlen = addrlen;
    if (is_global) {
        listener->hosts = NULL;
    } else {
        listener->hosts = enif_alloc(sizeof(listener->hosts[0]));
        listener->hosts[0] = NULL;
    }
    // (void) memset(&listener->ssl, 0, sizeof(listener->ssl));
    listener->proxy_protocol = proxy_protocol;

    config->listeners = enif_realloc(config->listeners, sizeof(*(config->listeners)) * (config->num_listeners + 1));
    config->listeners[config->num_listeners++] = listener;

    return listener;
}

static h2o_nif_cfg_listen_t *
find_listener(h2o_nif_config_t *config, struct sockaddr *addr, socklen_t addrlen)
{
    size_t i;

    for (i = 0; i != config->num_listeners; ++i) {
        h2o_nif_cfg_listen_t *listener = config->listeners[i];
        if (listener->addrlen == addrlen && h2o_socket_compare_address((void *)(&listener->addr), addr) == 0)
            return listener;
    }

    return NULL;
}

static int
open_tcp_listener(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node, const char *hostname,
                  const char *servname, int domain, int type, int protocol, struct sockaddr *addr, socklen_t addrlen)
{
    h2o_nif_config_t *config = (h2o_nif_config_t *)ctx->globalconf;
    int fd;

    if ((fd = socket(domain, type, protocol)) == -1)
        goto Error;
    (void)set_cloexec(fd);
    { /* set reuseaddr */
        int flag = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#ifdef TCP_DEFER_ACCEPT
    { /*set TCP_DEFER_ACCEPT */
        int flag = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#endif
#ifdef IPV6_V6ONLY
    /* set IPv6only */
    if (domain == AF_INET6) {
        int flag = 1;
        if (setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &flag, sizeof(flag)) != 0)
            goto Error;
    }
#endif
    if (bind(fd, addr, addrlen) != 0)
        goto Error;
    if (listen(fd, H2O_SOMAXCONN) != 0)
        goto Error;

    /* set TCP_FASTOPEN; when tfo_queues is zero TFO is always disabled */
    if (config->tfo_queues > 0) {
#ifdef TCP_FASTOPEN
        int tfo_queues;
#ifdef __APPLE__
        /* In OS X, the option value for TCP_FASTOPEN must be 1 if is's enabled */
        tfo_queues = 1;
#else
        tfo_queues = config->tfo_queues;
#endif
        if (setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, (const void *)&tfo_queues, sizeof(tfo_queues)) != 0)
            fprintf(stderr, "[warning] failed to set TCP_FASTOPEN:%s\n", strerror(errno));
#else
        assert(!"config->tfo_queues not zero on platform without TCP_FASTOPEN");
#endif
    }

    return fd;

Error:
    if (fd != -1)
        (void)close(fd);
    (void)h2o_configurator_errprintf(NULL, node, "failed to listen to port %s:%s: %s", hostname != NULL ? hostname : "ANY",
                                     servname, strerror(errno));
    return -1;
}

static int
open_unix_listener(h2o_configurator_command_t *cmd, h2o_configurator_context_t *ctx, yoml_t *node, struct sockaddr_un *sa)
{
    struct stat st;
    int fd = -1;
    struct passwd *owner = NULL;
    struct passwd pwbuf;
    char pwbuf_buf[65536];
    unsigned mode = UINT_MAX;
    yoml_t *t;

    /* obtain owner and permission */
    if ((t = yoml_get(node, "owner")) != NULL) {
        if (t->type != YOML_TYPE_SCALAR) {
            (void)h2o_configurator_errprintf(cmd, t, "`owner` is not a scalar");
            goto ErrorExit;
        }
        if (getpwnam_r(t->data.scalar, &pwbuf, pwbuf_buf, sizeof(pwbuf_buf), &owner) != 0 || owner == NULL) {
            (void)h2o_configurator_errprintf(cmd, t, "failed to obtain uid of user:%s: %s", t->data.scalar, strerror(errno));
            goto ErrorExit;
        }
    }
    if ((t = yoml_get(node, "permission")) != NULL) {
        if (t->type != YOML_TYPE_SCALAR || sscanf(t->data.scalar, "%o", &mode) != 1) {
            (void)h2o_configurator_errprintf(cmd, t, "`permission` must be an octal number");
            goto ErrorExit;
        }
    }

    /* remove existing socket file as suggested in #45 */
    if (lstat(sa->sun_path, &st) == 0) {
        if (S_ISSOCK(st.st_mode)) {
            (void)unlink(sa->sun_path);
        } else {
            (void)h2o_configurator_errprintf(cmd, node, "path:%s already exists and is not an unix socket.", sa->sun_path);
            goto ErrorExit;
        }
    }

    /* add new listener */
    if ((fd = socket(AF_UNIX, SOCK_STREAM, 0)) == -1 || bind(fd, (void *)sa, sizeof(*sa)) != 0 || listen(fd, H2O_SOMAXCONN) != 0) {
        (void)h2o_configurator_errprintf(NULL, node, "failed to listen to socket:%s: %s", sa->sun_path, strerror(errno));
        goto ErrorExit;
    }
    (void)set_cloexec(fd);

    /* set file owner and permission */
    if (owner != NULL && chown(sa->sun_path, owner->pw_uid, owner->pw_gid) != 0) {
        (void)h2o_configurator_errprintf(NULL, node, "failed to chown socket:%s to %s: %s", sa->sun_path, owner->pw_name,
                                         strerror(errno));
        goto ErrorExit;
    }
    if (mode != UINT_MAX && chmod(sa->sun_path, mode) != 0) {
        (void)h2o_configurator_errprintf(NULL, node, "failed to chmod socket:%s to %o: %s", sa->sun_path, mode, strerror(errno));
        goto ErrorExit;
    }

    return fd;

ErrorExit:
    if (fd != -1)
        (void)close(fd);
    return -1;
}

static void
set_cloexec(int fd)
{
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == -1) {
        perror("failed to set FD_CLOEXEC");
        abort();
    }
}