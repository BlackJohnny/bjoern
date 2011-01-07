#include <Python.h>
#include <cStringIO.h>
#include "request.h"

static PyObject* wsgi_http_header(Request*, const char*, const size_t);
static http_parser_settings parser_settings;
static PyObject* wsgi_base_dict = NULL;

Request* Request_new(int client_fd, const char* client_addr)
{
    Request* request = malloc(sizeof(Request));
#ifdef DEBUG
    static unsigned long request_id = 0;
    request->id = request_id++;
#endif
    request->client_fd = client_fd;
    request->client_addr = PyString_FromString(client_addr);
    http_parser_init((http_parser*)&request->parser, HTTP_REQUEST);
    request->parser.parser.data = request;
    Request_reset(request);
    return request;
}

void Request_reset(Request* request)
{
    memset(&request->state, 0, sizeof(Request) - (size_t)&((Request*)NULL)->state);
    request->state.response_length_unknown = true;
}

void Request_free(Request* request)
{
    Request_clean(request);
    free(request);
}

void Request_clean(Request* request)
{
    Py_XDECREF(request->iterable);
    Py_XDECREF(request->body);
    if(request->headers)
        assert(request->headers->ob_refcnt >= 1);
    if(request->status)
        assert(request->status->ob_refcnt >= 1);
    Py_XDECREF(request->headers);
    Py_XDECREF(request->status);
}

/* Parse stuff */

void Request_parse(Request* request,
                   const char* data,
                   const size_t data_len) {
    assert(data_len);
    size_t nparsed = http_parser_execute((http_parser*)&request->parser,
                                         &parser_settings, data, data_len);
    if(nparsed != data_len)
        request->state.error_code = HTTP_BAD_REQUEST;
}

static int a;
static void x() {}

#define REQUEST ((Request*)parser->data)
#define PARSER  ((bj_parser*)parser)
#define _update_length(name) \
    /* Update the len of a header field/value.
     *
     * Short explaination of the pointer arithmetics fun used here:
     *
     *   [old header data ] ...stuff... [ new header data ]
     *   ^-------------- A -------------^--------B--------^
     *
     * A = XXX_start - PARSER->XXX_start
     * B = XXX_len
     * A + B = old header start to new header end
     */ \
    do {\
        PARSER->name##_len = (name##_start - PARSER->name##_start) \
                                + name##_len; \
    } while(0)

#define _set_header(k, v) PyDict_SetItem(REQUEST->headers, k, v);
#define _set_header_free_value(k, v) \
    do { \
        PyObject* val = (v); \
        _set_header(k, val); \
        Py_DECREF(val); \
    } while(0)
#define _set_header_free_both(k, v) \
    do { \
        PyObject* key = (k); \
        PyObject* val = (v); \
        _set_header(key, val); \
        Py_DECREF(key); \
        Py_DECREF(val); \
    } while(0)

static int on_message_begin(http_parser* parser)
{
    REQUEST->headers = PyDict_New();
    PARSER->field_start = NULL;
    PARSER->field_len = 0;
    PARSER->value_start = NULL;
    PARSER->value_len = 0;
    return 0;
}

static int on_path(http_parser* parser,
                   char* path_start,
                   size_t path_len) {
    if(!(path_len = unquote_url_inplace(path_start, path_len)))
        return 1;
    _set_header_free_value(
        _PATH_INFO,
        PyString_FromStringAndSize(path_start, path_len)
    );
    return 0;
}

static int on_query_string(http_parser* parser,
                           const char* query_start,
                           const size_t query_len) {
    _set_header_free_value(
        _QUERY_STRING,
        PyString_FromStringAndSize(query_start, query_len)
    );
    return 0;
}

static int on_fragment(http_parser* parser,
                       const char* fragm_start,
                       const size_t fragm_len) {
    _set_header_free_value(
        _HTTP_FRAGMENT,
        PyString_FromStringAndSize(fragm_start, fragm_len)
    );
    return 0;
}

static int on_header_field(http_parser* parser,
                           const char* field_start,
                           const size_t field_len) {
    if(PARSER->value_start) {
        /* Store previous header and start a new one */
        _set_header_free_both(
            wsgi_http_header(REQUEST, PARSER->field_start, PARSER->field_len),
            PyString_FromStringAndSize(PARSER->value_start, PARSER->value_len)
        );

    } else if(PARSER->field_start) {
        _update_length(field);
        return 0;
    }

    PARSER->field_start = field_start;
    PARSER->field_len = field_len;
    PARSER->value_start = NULL;
    PARSER->value_len = 0;

    return 0;
}

static int on_header_value(http_parser* parser,
                           const char* value_start,
                           const size_t value_len) {
    if(PARSER->value_start) {
        _update_length(value);
    } else {
        /* Start a new value */
        PARSER->value_start = value_start;
        PARSER->value_len = value_len;
    }
    return 0;
}

static int on_headers_complete(http_parser* parser)
{
    if(PARSER->field_start) {
        _set_header_free_both(
            wsgi_http_header(REQUEST, PARSER->field_start, PARSER->field_len),
            PyString_FromStringAndSize(PARSER->value_start, PARSER->value_len)
        );
    }
    return 0;
}

static int on_body(http_parser* parser,
                   const char* body_start,
                   const size_t body_len) {
    if(!REQUEST->body) {
        if(!parser->content_length) {
            REQUEST->state.error_code = HTTP_LENGTH_REQUIRED;
            return 1;
        }
        REQUEST->body = PycStringIO->NewOutput(parser->content_length);
    }

    if(PycStringIO->cwrite(REQUEST->body, body_start, body_len) < 0) {
        REQUEST->state.error_code = HTTP_SERVER_ERROR;
        return 1;
    }

    return 0;
}

static int on_message_complete(http_parser* parser)
{
    /* SERVER_PROTOCOL (REQUEST_PROTOCOL) */
    _set_header(_SERVER_PROTOCOL, parser->http_minor == 1 ? _HTTP_1_1 : _HTTP_1_0);
    /* REQUEST_METHOD */
    if(parser->method == HTTP_GET) {
        /* I love useless micro-optimizations. */
        _set_header(_REQUEST_METHOD, _GET);
    } else {
    _set_header_free_value(_REQUEST_METHOD,
        PyString_FromString(http_method_str(parser->method)));
    }
    /* REMOTE_ADDR */
    _set_header(_REMOTE_ADDR, REQUEST->client_addr);
    /* wsgi.input */
    _set_header_free_value(_wsgi_input,
        PycStringIO->NewInput(
            REQUEST->body ? PycStringIO->cgetvalue(REQUEST->body)
                          : _empty_string));

    PyDict_Update(REQUEST->headers, wsgi_base_dict);

    REQUEST->state.parse_finished = true;
    return 0;
}


static PyObject*
wsgi_http_header(Request* request, const char* data, size_t len)
{
    /* Do not rename Content-Length and Content-Type */
    if(string_iequal(data, len, "Content-Length")) {
        Py_INCREF(_Content_Length);
        return _Content_Length;
    }
    if(string_iequal(data, len, "Content-Type")) {
        Py_INCREF(_Content_Type);
        return _Content_Type;
    }

    PyObject* obj = PyString_FromStringAndSize(/* empty string */ NULL,
                                               len+strlen("HTTP_"));
    char* dest = PyString_AS_STRING(obj);

    *dest++ = 'H';
    *dest++ = 'T';
    *dest++ = 'T';
    *dest++ = 'P';
    *dest++ = '_';

    while(len--) {
        char c = *data++;
        if(c == '-')
            *dest++ = '_';
        else if(c >= 'a' && c <= 'z')
            *dest++ = c - ('a'-'A');
        else
            *dest++ = c;
    }
    return obj;
}


static http_parser_settings
parser_settings = {
    .on_message_begin    = on_message_begin,
    .on_path             = on_path,
    .on_query_string     = on_query_string,
    .on_url              = NULL,
    .on_fragment         = on_fragment,
    .on_header_field     = on_header_field,
    .on_header_value     = on_header_value,
    .on_headers_complete = on_headers_complete,
    .on_body             = on_body,
    .on_message_complete = on_message_complete
};

void _initialize_request_module(const char* server_host, const int server_port)
{
    if(wsgi_base_dict == NULL) {
        PycString_IMPORT;
        wsgi_base_dict = PyDict_New();

        /* dct['wsgi.version'] = (1, 0) */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.version",
            PyTuple_Pack(2, PyInt_FromLong(1), PyInt_FromLong(0))
        );

        /* dct['wsgi.url_scheme'] = 'http'
         * (This can be hard-coded as there is no TLS support in bjoern.) */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.url_scheme",
            PyString_FromString("http")
        );

        /* dct['wsgi.errors'] = sys.stderr */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.errors",
            PySys_GetObject("stderr")
        );

        /* dct['wsgi.multithread'] = True
         * If I correctly interpret the WSGI specs, this means
         * "Can the server be ran in a thread?" */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.multithread",
            Py_True
        );

        /* dct['wsgi.multiprocess'] = True
         * ... and this one "Can the server process be forked?" */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.multiprocess",
            Py_True
        );

        /* dct['wsgi.run_once'] = False (bjoern is no CGI gateway) */
        PyDict_SetItemString(
            wsgi_base_dict,
            "wsgi.run_once",
            Py_False
        );
    }

    PyDict_SetItemString(
        wsgi_base_dict,
        "SERVER_NAME",
        PyString_FromString(server_host)
    );

    PyDict_SetItemString(
        wsgi_base_dict,
        "SERVER_PORT",
        PyString_FromFormat("%d", server_port)
    );
}
