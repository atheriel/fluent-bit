/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019-2021 The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_output_plugin.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_gzip.h>
#include <fluent-bit/flb_ra_key.h>

#include <msgpack.h>
#include "splunk.h"
#include "splunk_conf.h"

static int cb_splunk_init(struct flb_output_instance *ins,
                          struct flb_config *config, void *data)
{
    struct flb_splunk *ctx;

    ctx = flb_splunk_conf_create(ins, config);
    if (!ctx) {
        flb_plg_error(ins, "configuration failed");
        return -1;
    }

    flb_output_set_context(ins, ctx);

    /*
     * This plugin instance uses the HTTP client interface, let's register
     * it debugging callbacks.
     */
    flb_output_set_http_debug_callbacks(ins);

    return 0;
}

static int pack_map(struct flb_splunk *ctx, msgpack_packer *mp_pck,
                    struct flb_time *tm, msgpack_object map)
{
    int i;
    double t;
    int map_size;
    msgpack_object k;
    msgpack_object v;

    t = flb_time_to_double(tm);
    map_size = map.via.map.size;

    if (ctx->splunk_send_raw == FLB_TRUE) {
        msgpack_pack_map(mp_pck, map_size /* all k/v */);
    }
    else {
        msgpack_pack_map(mp_pck, 2 /* time + event */);

        /* Append the time key */
        msgpack_pack_str(mp_pck, sizeof(FLB_SPLUNK_DEFAULT_TIME) -1);
        msgpack_pack_str_body(mp_pck,
                              FLB_SPLUNK_DEFAULT_TIME,
                              sizeof(FLB_SPLUNK_DEFAULT_TIME) - 1);
        msgpack_pack_double(mp_pck, t);

        /* Add k/v pairs under the key 'event' instead of to the top level object */
        msgpack_pack_str(mp_pck, sizeof(FLB_SPLUNK_DEFAULT_EVENT) -1);
        msgpack_pack_str_body(mp_pck,
                              FLB_SPLUNK_DEFAULT_EVENT,
                              sizeof(FLB_SPLUNK_DEFAULT_EVENT) - 1);
        msgpack_pack_map(mp_pck, map_size);
    }

    /* Append k/v */
    for (i = 0; i < map_size; i++) {
        k = map.via.map.ptr[i].key;
        v = map.via.map.ptr[i].val;

        msgpack_pack_object(mp_pck, k);
        msgpack_pack_object(mp_pck, v);
    }

    return 0;
}


static inline int pack_event_key(struct flb_splunk *ctx, msgpack_packer *mp_pck,
                                 struct flb_time *tm, msgpack_object map)
{
    double t;
    struct flb_ra_value *rval;

    t = flb_time_to_double(tm);
    rval = flb_ra_get_value_object(ctx->ra_event_key, map);
    if (!rval) {
        return -1;
    }

    if (ctx->splunk_send_raw == FLB_FALSE) {
        msgpack_pack_map(mp_pck, 2 /* time + raw event key value */);

        /* Append the time key */
        msgpack_pack_str(mp_pck, sizeof(FLB_SPLUNK_DEFAULT_TIME) -1);
        msgpack_pack_str_body(mp_pck,
                              FLB_SPLUNK_DEFAULT_TIME,
                              sizeof(FLB_SPLUNK_DEFAULT_TIME) - 1);
        msgpack_pack_double(mp_pck, t);

        /* Add k/v pairs under the key 'event' instead of to the top level object */
        msgpack_pack_str(mp_pck, sizeof(FLB_SPLUNK_DEFAULT_EVENT) -1);
        msgpack_pack_str_body(mp_pck,
                              FLB_SPLUNK_DEFAULT_EVENT,
                              sizeof(FLB_SPLUNK_DEFAULT_EVENT) - 1);
    }

    msgpack_pack_object(mp_pck, rval->o);

    flb_ra_key_value_destroy(rval);
    return 0;
}

static inline int splunk_format(const void *in_buf, size_t in_bytes,
                                char **out_buf, size_t *out_size,
                                struct flb_splunk *ctx)
{
    int ret;
    size_t off = 0;
    struct flb_time tm;
    msgpack_unpacked result;
    msgpack_object root;
    msgpack_object *obj;
    msgpack_object map;
    msgpack_sbuffer mp_sbuf;
    msgpack_packer mp_pck;
    char *err;
    flb_sds_t tmp;
    flb_sds_t record;
    flb_sds_t json_out;

    json_out = flb_sds_create_size(in_bytes * 1.5);
    if (!json_out) {
        flb_errno();
        return -1;
    }

    /* Iterate the original buffer and perform adjustments */
    msgpack_unpacked_init(&result);

    while (msgpack_unpack_next(&result, in_buf, in_bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        if (result.data.type != MSGPACK_OBJECT_ARRAY) {
            continue;
        }

        root = result.data;
        if (root.via.array.size != 2) {
            continue;
        }

        /* Get timestamp */
        flb_time_pop_from_msgpack(&tm, &result, &obj);

        /* Create temporary msgpack buffer */
        msgpack_sbuffer_init(&mp_sbuf);
        msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

        map = root.via.array.ptr[1];

        if (ctx->event_key) {
            /* Pack the value of a event key */
            ret = pack_event_key(ctx, &mp_pck, &tm, map);
        }
        else {
            /* Pack as a map */
            ret = pack_map(ctx, &mp_pck, &tm, map);
        }

        /* Validate packaging */
        if (ret != 0) {
            /* Format invalid record */
            err = flb_msgpack_to_json_str(1048, &map);
            if (err) {
                /* Print error and continue processing other records */
                flb_plg_warn(ctx->ins, "could not process record: %s", err);
                msgpack_sbuffer_destroy(&mp_sbuf);
                flb_free(err);
            }
            continue;
        }

        /* Format as JSON */
        record = flb_msgpack_raw_to_json_sds(mp_sbuf.data, mp_sbuf.size);
        if (!record) {
            flb_errno();
            msgpack_sbuffer_destroy(&mp_sbuf);
            msgpack_unpacked_destroy(&result);
            flb_sds_destroy(json_out);
            return -1;
        }

        /* On raw mode, append a breakline to every record */
        if (ctx->splunk_send_raw) {
            tmp = flb_sds_cat(record, "\n", 1);
            if (tmp) {
                record = tmp;
            }
        }

        tmp = flb_sds_cat(json_out, record, flb_sds_len(record));
        flb_sds_destroy(record);
        if (tmp) {
            json_out = tmp;
        }
        else {
            flb_errno();
            msgpack_sbuffer_destroy(&mp_sbuf);
            msgpack_unpacked_destroy(&result);
            flb_sds_destroy(json_out);
            return -1;
        }
        msgpack_sbuffer_destroy(&mp_sbuf);
    }

    *out_buf = json_out;
    *out_size = flb_sds_len(json_out);

    return 0;
}

static void cb_splunk_flush(const void *data, size_t bytes,
                            const char *tag, int tag_len,
                            struct flb_input_instance *i_ins,
                            void *out_context,
                            struct flb_config *config)
{
    int ret;
    int compressed = FLB_FALSE;
    size_t b_sent;
    flb_sds_t buf_data;
    size_t buf_size;
    char *endpoint;
    struct flb_splunk *ctx = out_context;
    struct flb_upstream_conn *u_conn;
    struct flb_http_client *c;
    void *payload_buf;
    size_t payload_size;
    (void) i_ins;
    (void) config;

    /* Get upstream connection */
    u_conn = flb_upstream_conn_get(ctx->u);
    if (!u_conn) {
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Convert binary logs into a JSON payload */
    ret = splunk_format(data, bytes, &buf_data, &buf_size, ctx);
    if (ret == -1) {
        flb_upstream_conn_release(u_conn);
        FLB_OUTPUT_RETURN(FLB_ERROR);
    }

    /* Map buffer */
    payload_buf = buf_data;
    payload_size = buf_size;

    /* Should we compress the payload ? */
    if (ctx->compress_gzip == FLB_TRUE) {
        ret = flb_gzip_compress((void *) buf_data, buf_size,
                                &payload_buf, &payload_size);
        if (ret == -1) {
            flb_plg_error(ctx->ins,
                          "cannot gzip payload, disabling compression");
        }
        else {
            compressed = FLB_TRUE;

            /* JSON buffer is not longer needed */
            flb_sds_destroy(buf_data);
        }
    }

    /* Splunk URI endpoint */
    if (ctx->splunk_send_raw) {
        endpoint = FLB_SPLUNK_DEFAULT_URI_RAW;
    }
    else {
        endpoint = FLB_SPLUNK_DEFAULT_URI_EVENT;
    }

    /* Compose HTTP Client request */
    c = flb_http_client(u_conn, FLB_HTTP_POST, endpoint,
                        payload_buf, payload_size, NULL, 0, NULL, 0);
    flb_http_buffer_size(c, FLB_HTTP_DATA_SIZE_MAX);
    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);

    /* Try to use http_user and http_passwd if not, fallback to auth_header */
    if (ctx->http_user && ctx->http_passwd) {
        flb_http_basic_auth(c, ctx->http_user, ctx->http_passwd);
    }
    else if (ctx->auth_header) {
        flb_http_add_header(c, "Authorization", 13,
                            ctx->auth_header, flb_sds_len(ctx->auth_header));
    }

    /* Content Encoding: gzip */
    if (compressed == FLB_TRUE) {
        flb_http_set_content_encoding_gzip(c);
    }

    /* Map debug callbacks */
    flb_http_client_debug(c, ctx->ins->callback);

    /* Perform HTTP request */
    ret = flb_http_do(c, &b_sent);
    if (ret != 0) {
        flb_plg_warn(ctx->ins, "http_do=%i", ret);
        ret = FLB_RETRY;
    }
    else {
        if (c->resp.status != 200) {
            if (c->resp.payload_size > 0) {
                flb_plg_warn(ctx->ins, "http_status=%i:\n%s",
                         c->resp.status, c->resp.payload);
            }
            else {
                flb_plg_warn(ctx->ins, "http_status=%i", c->resp.status);
            }
            /*
             * Requests that get 4xx responses from the Splunk HTTP Event
             * Collector will 'always' fail, so there is no point in retrying
             * them:
             *
             * https://docs.splunk.com/Documentation/Splunk/8.0.5/Data/TroubleshootHTTPEventCollector#Possible_error_codes
             */
            ret = (c->resp.status < 400 || c->resp.status >= 500) ?
                FLB_RETRY : FLB_ERROR;
        }
        else {
            ret = FLB_OK;
        }
    }

    /*
     * If the payload buffer is different than incoming records in body, means
     * we generated a different payload and must be freed.
     */
    if (compressed == FLB_TRUE) {
        flb_free(payload_buf);
    }
    else {
        flb_sds_destroy(buf_data);
    }

    /* Cleanup */
    flb_http_client_destroy(c);
    flb_upstream_conn_release(u_conn);
    FLB_OUTPUT_RETURN(ret);
}

static int cb_splunk_exit(void *data, struct flb_config *config)
{
    struct flb_splunk *ctx = data;

    flb_splunk_conf_destroy(ctx);
    return 0;
}

/* Configuration properties map */
static struct flb_config_map config_map[] = {
    {
     FLB_CONFIG_MAP_STR, "compress", NULL,
     0, FLB_FALSE, 0,
     "Set payload compression mechanism. Option available is 'gzip'"
    },

    {
     FLB_CONFIG_MAP_STR, "http_user", NULL,
     0, FLB_TRUE, offsetof(struct flb_splunk, http_user),
     "Set HTTP auth user"
    },

    {
     FLB_CONFIG_MAP_STR, "http_passwd", "",
     0, FLB_TRUE, offsetof(struct flb_splunk, http_passwd),
     "Set HTTP auth password"
    },

    {
     FLB_CONFIG_MAP_STR, "event_key", NULL,
     0, FLB_TRUE, offsetof(struct flb_splunk, event_key),
     "Specify the key name that will be used to send a single value as part of the record."
    },

    {
     FLB_CONFIG_MAP_STR, "splunk_token", NULL,
     0, FLB_FALSE, 0,
     "Specify the Authentication Token for the HTTP Event Collector interface."
    },

    {
     FLB_CONFIG_MAP_BOOL, "splunk_send_raw", "off",
     0, FLB_TRUE, offsetof(struct flb_splunk, splunk_send_raw),
     "When enabled, the record keys and values are set in the top level of the "
     "map instead of under the event key. Refer to the Sending Raw Events section "
     "from the docs for more details to make this option work properly."
    },

    /* EOF */
    {0}
};


static int cb_splunk_format_test(struct flb_config *config,
                                 struct flb_input_instance *ins,
                                 void *plugin_context,
                                 void *flush_ctx,
                                 const char *tag, int tag_len,
                                 const void *data, size_t bytes,
                                 void **out_data, size_t *out_size)
{
    struct flb_splunk *ctx = plugin_context;
    return splunk_format(data, bytes, (char**)out_data, out_size,ctx);
}

struct flb_output_plugin out_splunk_plugin = {
    .name         = "splunk",
    .description  = "Send events to Splunk HTTP Event Collector",
    .cb_init      = cb_splunk_init,
    .cb_flush     = cb_splunk_flush,
    .cb_exit      = cb_splunk_exit,
    .config_map   = config_map,

    /* for testing */
    .test_formatter.callback = cb_splunk_format_test,
    /* Plugin flags */
    .flags          = FLB_OUTPUT_NET | FLB_IO_OPT_TLS,
};
