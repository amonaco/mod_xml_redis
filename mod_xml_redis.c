/* 
 * FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 * Copyright (C) 2005-2012, Anthony Minessale II <anthm@freeswitch.org>
 *
 * Version: MPL 1.1
 *
 * The contents of this file are subject to the Mozilla Public License Version
 * 1.1 (the "License"); you may not use this file except in compliance with
 * the License. You may obtain a copy of the License at
 * http://www.mozilla.org/MPL/
 *
 * Software distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTY OF ANY KIND, either express or implied. See the License
 * for the specific language governing rights and limitations under the
 * License.
 *
 * The Original Code is FreeSWITCH Modular Media Switching Software Library / Soft-Switch Application
 *
 * The Initial Developer of the Original Code is
 * Anthony Minessale II <anthm@freeswitch.org>
 * Portions created by the Initial Developer are Copyright (C)
 * the Initial Developer. All Rights Reserved.
 *
 * Based on:
 * mod_xml_curl.c - CURL XML Gateway
 *
 * Ariel Monaco <amonaco@gmail.com>
 * mod_xml_redis.c - Redis XML Interface
 */

#include <switch.h>
#include <hiredis.h>

SWITCH_MODULE_LOAD_FUNCTION(mod_xml_redis_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_xml_redis_shutdown);
SWITCH_MODULE_DEFINITION(mod_xml_redis, mod_xml_redis_load, mod_xml_redis_shutdown, NULL);

struct xml_binding {
    char *host;
    int port;
    char *bindings;
    char *key_prefix;
    char *key_use_variable;
    struct timeval timeout;
    switch_hash_t *vars_map;
};

typedef struct xml_binding xml_binding_t;

typedef struct hash_node {
    switch_hash_t *hash;
    struct hash_node *next;
} hash_node_t;

static struct {
    switch_memory_pool_t *pool;
    hash_node_t *hash_root;
    hash_node_t *hash_tail;
} globals;

static int xml_redis_debug = 0;
#define XML_REDIS_SYNTAX "[debug_on|debug_off]"

SWITCH_STANDARD_API(xml_redis_function)
{
    if (session) {
        return SWITCH_STATUS_FALSE;
    }

    if (zstr(cmd)) {
        goto usage;
    }

    if (!strcasecmp(cmd, "debug_on")) {
        xml_redis_debug = 1;
    } else if (!strcasecmp(cmd, "debug_off")) {
        xml_redis_debug = 0;
    } else {
        goto usage;
    }

    stream->write_function(stream, "OK\n");
    return SWITCH_STATUS_SUCCESS;

  usage:
    stream->write_function(stream, "USAGE: %s\n", XML_REDIS_SYNTAX);
    return SWITCH_STATUS_SUCCESS;
}

static switch_xml_t xml_redis_fetch(const char *section, const char *tag_name, const char *key_name, const char *key_value,
        switch_event_t *params, void *user_data)
{
    switch_xml_t xml = NULL;
    xml_binding_t *binding = (xml_binding_t *) user_data;
    redisContext *redis;
    redisReply *data;
    char *redis_key;

    if (!binding) {
        return NULL;
    }

    /* connect to redis */
    redis = redisConnectWithTimeout(binding->host, binding->port, binding->timeout);
    if (redis == NULL || redis->err) {
        if (redis) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't connect to redis server %s:%d, error:%s\n",
                    binding->host, binding->port, redis->errstr);
            redisFree(redis);
        } else {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't allocate memory!");
        }
        return NULL;
    }

    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Redis connenction, host: %s, port: %d\n", binding->host, binding->port);

    /* build the redis lookup key */ 
    redis_key = switch_core_sprintf(globals.pool, "%s%s", binding->key_prefix, switch_event_get_header(params, binding->key_use_variable));
    if (!redis_key) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Invalid lookup variable: %s, check config!\n", binding->key_use_variable);
        redisFree(redis);
        return NULL;
    } else {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Quering Redis key: %s\n", redis_key);
    }

    /* query redis back-end */
    data = redisCommand(redis, "GET %s", redis_key);
    if (!data) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Couldn't get data for key: %s\n", redis_key);
        goto end;
    }

    /* debug enabled via console */
    if (xml_redis_debug) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Requested key: %s\n", redis_key); 
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "Got response:\n%s\n", data->str);
    }

    /* tell switch to parse the data */
    if (!(xml = switch_xml_parse_str_dynamic(data->str, FALSE))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Error Parsing Result! data: [%s]\n", data->str);
        xml = NULL;
    }

  end:

    freeReplyObject(redis);
    switch_safe_free(data);

    return xml;
}

static switch_status_t do_config(void)
{
    int x = 0;
    char *cf = "xml_redis.conf";
    switch_xml_t cfg, xml, bindings_tag, binding_tag, param;
    xml_binding_t *binding = NULL;
    switch_hash_t *vars_map = NULL;

    if (!(xml = switch_xml_open_cfg(cf, &cfg, NULL))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "open of %s failed\n", cf);
        return SWITCH_STATUS_TERM;
    }

    if (!(bindings_tag = switch_xml_child(cfg, "bindings"))) {
        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Missing <bindings> tag!\n");
        goto done;
    }

    for (binding_tag = switch_xml_child(bindings_tag, "binding"); binding_tag; binding_tag = binding_tag->next) {
        char *bname = (char *) switch_xml_attr_soft(binding_tag, "name");
        int port = 0;
        char *host = NULL;
        int timeout = 0;
        char *key_prefix = NULL;
        char *key_use_variable = NULL;
        char *bind_mask = NULL;
        hash_node_t *hash_node;
        vars_map = NULL;

        for (param = switch_xml_child(binding_tag, "param"); param; param = param->next) {

            char *var = (char *) switch_xml_attr_soft(param, "name");
            char *val = (char *) switch_xml_attr_soft(param, "value");

            if (!strcasecmp(var, "host")) {
                host = val;
            } else if (!strcasecmp(var, "port")) {
                port = atoi(val);
            } else if (!strcasecmp(var, "bindings")) {
                bind_mask = val;
            } else if (!strcasecmp(var, "key_prefix")) {
                key_prefix = val;
            } else if (!strcasecmp(var, "key_use_variable")) {
                key_use_variable = val;
                switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_DEBUG, "DEBGGING VARIABLE %s!\n", key_use_variable);
            } else if (!strcasecmp(var, "timeout")) {
                if (timeout < 0 || timeout > 5000) {
                    switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Set timeout between 0 and 5000 milliseconds!\n");
                } else {
                    timeout = atoi(val);
                }
            }
        }

        if (!host) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Binding has no host!\n");
            if (vars_map)
                switch_core_hash_destroy(&vars_map);
            continue;
        }

        if (!port) {
            switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "Binding has no port!\n");
            if (vars_map)
                switch_core_hash_destroy(&vars_map);
            continue;
        }

        if (!(binding = malloc(sizeof(*binding)))) {
            if (vars_map)
                switch_core_hash_destroy(&vars_map);
            goto done;
        }

        memset(binding, 0, sizeof(*binding));

        if (host != NULL) {
            binding->host = strdup(host);
        }

        if (port) {
            binding->port = port;
        }

        if (bind_mask) {
            binding->bindings = strdup(bind_mask);
        }

        if (key_prefix != NULL) {
            binding->key_prefix = strdup(key_prefix);
        } 

        if (key_use_variable != NULL) {
            binding->key_use_variable = strdup(key_use_variable);
        }

        binding->timeout.tv_sec = timeout / 1000;
        binding->timeout.tv_usec = (timeout - binding->timeout.tv_sec * 1000) * 1000;
        binding->vars_map = vars_map;

        if (vars_map) {
            switch_zmalloc(hash_node, sizeof(hash_node_t));
            hash_node->hash = vars_map;
            hash_node->next = NULL;

            if (!globals.hash_root) {
                globals.hash_root = hash_node;
                globals.hash_tail = globals.hash_root;
            }

            else {
                globals.hash_tail->next = hash_node;
                globals.hash_tail = globals.hash_tail->next;
            }
        }

        switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE, "Binding [%s] XML Fetch Function [%s]\n",
                          zstr(bname) ? "N/A" : bname, binding->bindings ? binding->bindings : "all");

        switch_xml_bind_search_function(xml_redis_fetch, switch_xml_parse_section_string(binding->bindings), binding);
        x++;
        binding = NULL;
    }

  done:

    switch_xml_free(xml);

    return x ? SWITCH_STATUS_SUCCESS : SWITCH_STATUS_FALSE;
}

SWITCH_MODULE_LOAD_FUNCTION(mod_xml_redis_load)
{
    switch_api_interface_t *xml_redis_api_interface;

    /* connect my internal structure to the blank pointer passed to me */
    *module_interface = switch_loadable_module_create_module_interface(pool, modname);

    memset(&globals, 0, sizeof(globals));
    globals.pool = pool;
    globals.hash_root = NULL;
    globals.hash_tail = NULL;

    if (do_config() != SWITCH_STATUS_SUCCESS) {
        return SWITCH_STATUS_FALSE;
    }


    SWITCH_ADD_API(xml_redis_api_interface, "xml_redis", "XML Redis", xml_redis_function, XML_REDIS_SYNTAX);
    switch_console_set_complete("add xml_redis debug_on");
    switch_console_set_complete("add xml_redis debug_off");

    /* indicate that the module should continue to be loaded */
    return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_xml_redis_shutdown)
{
    hash_node_t *ptr = NULL;

    while (globals.hash_root) {
        ptr = globals.hash_root;
        switch_core_hash_destroy(&ptr->hash);
        globals.hash_root = ptr->next;
        switch_safe_free(ptr);
    }

    switch_xml_unbind_search_function_ptr(xml_redis_fetch);

    return SWITCH_STATUS_SUCCESS;
}

/* For Emacs:
 * Local Variables:
 * mode:c
 * indent-tabs-mode:t
 * tab-width:4
 * c-basic-offset:4
 * End:
 * For VIM:
 * vim:set softtabstop=4 shiftwidth=4 tabstop=4:
 */
