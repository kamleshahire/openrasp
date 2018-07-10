/*
 * Copyright 2017-2018 Baidu Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "openrasp_security_policy.h"
#include "openrasp_hook.h"
#include "openrasp_ini.h"
#include "openrasp_inject.h"
#include <regex>

static void _check_header_content_type_if_html(void *data, void *arg TSRMLS_DC);
static int _detect_param_occur_in_html_output(const char *param TSRMLS_DC);
static bool _gpc_parameter_filter(const zval *param TSRMLS_DC);
static bool _is_content_type_html(TSRMLS_D);

#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION <= 3)

void openrasp_detect_output(INTERNAL_FUNCTION_PARAMETERS)
{
    char *input;
    int input_len;
    long mode;

    if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &input, &input_len, &mode) == FAILURE)
    {
        RETVAL_FALSE;
    }
    if (_is_content_type_html(TSRMLS_C))
    {
        int status = _detect_param_occur_in_html_output(input TSRMLS_CC);
        if (status == SUCCESS)
        {
            char *block_url = openrasp_ini.block_url;
            char *request_id = OPENRASP_INJECT_G(request_id);
            add_location_header(block_url, request_id TSRMLS_CC);
            RETVAL_STRING("", 1);
        }
    }
}

#else

static php_output_handler *openrasp_output_handler_init(const char *handler_name, size_t handler_name_len, size_t chunk_size, int flags TSRMLS_DC);
static void openrasp_clean_output_start(const char *name, size_t name_len TSRMLS_DC);
static int openrasp_output_handler(void **nothing, php_output_context *output_context);

static int openrasp_output_handler(void **nothing, php_output_context *output_context)
{
    int status = FAILURE;
    PHP_OUTPUT_TSRMLS(output_context);
    if (_is_content_type_html(TSRMLS_C) &&
        (output_context->op & PHP_OUTPUT_HANDLER_START) &&
        (output_context->op & PHP_OUTPUT_HANDLER_FINAL))
    {
        status = _detect_param_occur_in_html_output(output_context->in.data TSRMLS_CC);
        if (status == SUCCESS)
        {
            char *block_url = openrasp_ini.block_url;
            char *request_id = OPENRASP_INJECT_G(request_id);
            add_location_header(block_url, request_id TSRMLS_CC);
        }
    }
    return status;
}

static php_output_handler *openrasp_output_handler_init(const char *handler_name, size_t handler_name_len, size_t chunk_size, int flags TSRMLS_DC)
{
    if (chunk_size)
    {
        return nullptr;
    }
    return php_output_handler_create_internal(handler_name, handler_name_len, openrasp_output_handler, chunk_size, flags TSRMLS_CC);
}

static void openrasp_clean_output_start(const char *name, size_t name_len TSRMLS_DC)
{
    php_output_handler *h;

    if (h = openrasp_output_handler_init(name, name_len, 0, PHP_OUTPUT_HANDLER_STDFLAGS TSRMLS_CC))
    {
        php_output_handler_start(h TSRMLS_CC);
    }
}

#endif

static void _check_header_content_type_if_html(void *data, void *arg TSRMLS_DC)
{
    bool *is_html = static_cast<bool *>(arg);
    if (*is_html)
    {
        sapi_header_struct *sapi_header = (sapi_header_struct *)data;
        static const char *suffix = "Content-type";
        char *header = (char *)(sapi_header->header);
        size_t header_len = strlen(header);
        size_t suffix_len = strlen(suffix);
        if (header_len > suffix_len &&
            strncmp(suffix, header, suffix_len) == 0 &&
            NULL == strstr(header, "text/html"))
        {
            *is_html = false;
        }
    }
}

static bool _gpc_parameter_filter(const zval *param TSRMLS_DC)
{
    if (Z_TYPE_P(param) == IS_STRING && Z_STRLEN_P(param) > openrasp_ini.xss_min_param_length)
    {
        std::regex r(openrasp_ini.xss_filter_regex);
        std::basic_string<char> text(Z_STRVAL_P(param));
        std::smatch sm;
        if (std::regex_search(text, sm, r))
        {
            return true;
        }
    }
    return false;
}

static int _detect_param_occur_in_html_output(const char *param TSRMLS_DC)
{
    int status = FAILURE;
    if (!PG(http_globals)[TRACK_VARS_GET] &&
        !zend_is_auto_global("_GET", strlen("_GET") TSRMLS_CC) &&
        Z_TYPE_P(PG(http_globals)[TRACK_VARS_GET]) != IS_ARRAY)
    {
        return FAILURE;
    }
    HashTable *ht = Z_ARRVAL_P(PG(http_globals)[TRACK_VARS_GET]);
    int count = 0;
    for (zend_hash_internal_pointer_reset(ht);
         zend_hash_has_more_elements(ht) == SUCCESS;
         zend_hash_move_forward(ht))
    {
        zval **ele_value;
        if (zend_hash_get_current_data(ht, (void **)&ele_value) != SUCCESS)
        {
            continue;
        }
        if (_gpc_parameter_filter(*ele_value TSRMLS_CC))
        {
            if (++count > openrasp_ini.xss_max_detection_num)
            {
                zval *attack_params = NULL;
                MAKE_STD_ZVAL(attack_params);
                ZVAL_STRING(attack_params, "", 1);
                zval *plugin_message = NULL;
                MAKE_STD_ZVAL(plugin_message);
                ZVAL_STRING(plugin_message, _("Excessively suspected xss parameters"), 1);
                openrasp_buildin_php_risk_handle(0, "xss", 100, attack_params, plugin_message TSRMLS_CC);
                return SUCCESS;
            }
            if (NULL != strstr(param, Z_STRVAL_PP(ele_value)))
            {
                zval *attack_params = NULL;
                MAKE_STD_ZVAL(attack_params);
                ZVAL_STRING(attack_params, Z_STRVAL_PP(ele_value), 1);
                zval *plugin_message = NULL;
                MAKE_STD_ZVAL(plugin_message);
                char *message_str = NULL;
                spprintf(&message_str, 0, _("Reflected XSS attack detected: using get parameter: '%s'"), Z_STRVAL_PP(ele_value));
                ZVAL_STRING(plugin_message, message_str, 1);
                efree(message_str);
                openrasp_buildin_php_risk_handle(0, "xss", 100, attack_params, plugin_message TSRMLS_CC);
                return SUCCESS;
            }
        }
    }
    return status;
}

static bool _is_content_type_html(TSRMLS_D)
{
    bool is_html = true;
    zend_llist_apply_with_argument(&SG(sapi_headers).headers, _check_header_content_type_if_html, &is_html TSRMLS_CC);
    return is_html;
}

PHP_MINIT_FUNCTION(openrasp_output_detect)
{
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION > 3)
    php_output_handler_alias_register(ZEND_STRL("openrasp_ob_handler"), openrasp_output_handler_init TSRMLS_CC);
#endif
    return SUCCESS;
}

PHP_RINIT_FUNCTION(openrasp_output_detect)
{
    if (!openrasp_check_type_ignored(ZEND_STRL("xss") TSRMLS_CC))
    {
#if (PHP_MAJOR_VERSION == 5) && (PHP_MINOR_VERSION <= 3)
        if (php_start_ob_buffer_named("openrasp_ob_handler", 0, 1 TSRMLS_CC) == FAILURE)
        {
            openrasp_error(E_WARNING, RUNTIME_ERROR, _("Failure start OpenRASP output buffering."));
        }
#else
        openrasp_clean_output_start(ZEND_STRL("openrasp_ob_handler") TSRMLS_CC);
#endif
    }
    return SUCCESS;
}