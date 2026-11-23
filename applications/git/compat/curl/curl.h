#ifndef _GIT_COMPAT_CURL_CURL_H
#define _GIT_COMPAT_CURL_CURL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/select.h>
#include <sys/time.h>

#define LIBCURL_VERSION_NUM 0x073d00
#define LIBCURL_VERSION "7.61.0-ipo"

typedef int64_t curl_off_t;
typedef void CURL;
typedef void CURLM;

#define CURL_ERROR_SIZE 256
#define CURL_GLOBAL_ALL 3L

typedef enum {
    CURLE_OK = 0,
    CURLE_UNSUPPORTED_PROTOCOL = 1,
    CURLE_FAILED_INIT = 2,
    CURLE_URL_MALFORMAT = 3,
    CURLE_COULDNT_RESOLVE_PROXY = 5,
    CURLE_COULDNT_RESOLVE_HOST = 6,
    CURLE_COULDNT_CONNECT = 7,
    CURLE_FTP_COULDNT_RETR_FILE = 19,
    CURLE_HTTP_RETURNED_ERROR = 22,
    CURLE_WRITE_ERROR = 23,
    CURLE_READ_ERROR = 26,
    CURLE_OUT_OF_MEMORY = 27,
    CURLE_OPERATION_TIMEDOUT = 28,
    CURLE_FILE_COULDNT_READ_FILE = 37,
    CURLE_ABORTED_BY_CALLBACK = 42,
    CURLE_TOO_MANY_REDIRECTS = 47,
    CURLE_GOT_NOTHING = 52,
    CURLE_SEND_ERROR = 55,
    CURLE_RECV_ERROR = 56,
    CURLE_SSL_CERTPROBLEM = 58,
    CURLE_SSL_CIPHER = 59,
    CURLE_SSL_CACERT = 60,
    CURLE_SSL_PINNEDPUBKEYNOTMATCH = 90
} CURLcode;

typedef enum {
    CURLM_CALL_MULTI_PERFORM = -1,
    CURLM_OK = 0,
    CURLM_BAD_HANDLE = 1,
    CURLM_BAD_EASY_HANDLE = 2,
    CURLM_OUT_OF_MEMORY = 3,
    CURLM_INTERNAL_ERROR = 4
} CURLMcode;

typedef enum {
    CURLMSG_NONE = 0,
    CURLMSG_DONE = 1
} CURLMSG;

typedef struct {
    CURLMSG msg;
    CURL *easy_handle;
    union {
        void *whatever;
        CURLcode result;
    } data;
} CURLMsg;

struct curl_slist {
    char *data;
    struct curl_slist *next;
};

typedef enum {
    CURLINFO_TEXT = 0,
    CURLINFO_HEADER_IN = 1,
    CURLINFO_HEADER_OUT = 2,
    CURLINFO_DATA_IN = 3,
    CURLINFO_DATA_OUT = 4,
    CURLINFO_SSL_DATA_IN = 5,
    CURLINFO_SSL_DATA_OUT = 6
} curl_infotype;

typedef size_t (*curl_write_callback)(char *buffer, size_t size, size_t nitems, void *outstream);
typedef size_t (*curl_read_callback)(char *buffer, size_t size, size_t nitems, void *instream);
typedef int (*curl_seek_callback)(void *instream, curl_off_t offset, int origin);
typedef int (*curl_debug_callback)(CURL *handle, curl_infotype type, char *data, size_t size, void *userptr);

#define CURL_SEEKFUNC_OK   0
#define CURL_SEEKFUNC_FAIL 1

#define CURLAUTH_NONE         0UL
#define CURLAUTH_BASIC        (1UL << 0)
#define CURLAUTH_DIGEST       (1UL << 1)
#define CURLAUTH_GSSNEGOTIATE (1UL << 2)
#define CURLAUTH_NTLM         (1UL << 3)
#define CURLAUTH_DIGEST_IE    (1UL << 4)
#define CURLAUTH_ANY          (~CURLAUTH_DIGEST_IE)

#define CURLPROTO_HTTP  (1L << 0)
#define CURLPROTO_HTTPS (1L << 1)
#define CURLPROTO_FTP   (1L << 2)
#define CURLPROTO_FTPS  (1L << 3)

#define CURL_IPRESOLVE_WHATEVER 0L
#define CURL_IPRESOLVE_V4       1L
#define CURL_IPRESOLVE_V6       2L

#define CURL_HTTP_VERSION_NONE 0L
#define CURL_HTTP_VERSION_1_0  1L
#define CURL_HTTP_VERSION_1_1  2L
#define CURL_HTTP_VERSION_2    3L

#define CURL_NETRC_OPTIONAL 1L
#define CURL_REDIR_POST_ALL 7L
#define CURLUSESSL_TRY      1L
#define CURLSSLOPT_NO_REVOKE (1L << 1)

#define CURLGSSAPI_DELEGATION_NONE        0L
#define CURLGSSAPI_DELEGATION_POLICY_FLAG (1L << 0)
#define CURLGSSAPI_DELEGATION_FLAG        (1L << 1)

#define CURLPROXY_HTTP            0L
#define CURLPROXY_HTTPS           2L
#define CURLPROXY_SOCKS4          4L
#define CURLPROXY_SOCKS5          5L
#define CURLPROXY_SOCKS4A         6L
#define CURLPROXY_SOCKS5_HOSTNAME 7L

#define CURL_SSLVERSION_DEFAULT 0L
#define CURL_SSLVERSION_TLSv1   1L
#define CURL_SSLVERSION_SSLv2   2L
#define CURL_SSLVERSION_SSLv3   3L
#define CURL_SSLVERSION_TLSv1_0 4L
#define CURL_SSLVERSION_TLSv1_1 5L
#define CURL_SSLVERSION_TLSv1_2 6L
#define CURL_SSLVERSION_TLSv1_3 7L

typedef enum {
    CURLOPT_URL = 1,
    CURLOPT_PORT,
    CURLOPT_PROXY,
    CURLOPT_USERPWD,
    CURLOPT_PROXYUSERPWD,
    CURLOPT_RANGE,
    CURLOPT_INFILE,
    CURLOPT_ERRORBUFFER,
    CURLOPT_WRITEFUNCTION,
    CURLOPT_READFUNCTION,
    CURLOPT_TIMEOUT,
    CURLOPT_INFILESIZE,
    CURLOPT_POSTFIELDS,
    CURLOPT_REFERER,
    CURLOPT_USERAGENT,
    CURLOPT_LOW_SPEED_LIMIT,
    CURLOPT_LOW_SPEED_TIME,
    CURLOPT_RESUME_FROM,
    CURLOPT_COOKIE,
    CURLOPT_HTTPHEADER,
    CURLOPT_SSLCERT,
    CURLOPT_KEYPASSWD,
    CURLOPT_CRLF,
    CURLOPT_QUOTE,
    CURLOPT_HEADERDATA,
    CURLOPT_COOKIEFILE,
    CURLOPT_SSLVERSION,
    CURLOPT_TIMECONDITION,
    CURLOPT_TIMEVALUE,
    CURLOPT_CUSTOMREQUEST,
    CURLOPT_STDERR,
    CURLOPT_POSTQUOTE,
    CURLOPT_VERBOSE,
    CURLOPT_HEADER,
    CURLOPT_NOPROGRESS,
    CURLOPT_NOBODY,
    CURLOPT_FAILONERROR,
    CURLOPT_UPLOAD,
    CURLOPT_POST,
    CURLOPT_DIRLISTONLY,
    CURLOPT_APPEND,
    CURLOPT_NETRC,
    CURLOPT_FOLLOWLOCATION,
    CURLOPT_TRANSFERTEXT,
    CURLOPT_PUT,
    CURLOPT_PROGRESSFUNCTION,
    CURLOPT_PROGRESSDATA,
    CURLOPT_AUTOREFERER,
    CURLOPT_PROXYPORT,
    CURLOPT_POSTFIELDSIZE,
    CURLOPT_HTTPPROXYTUNNEL,
    CURLOPT_INTERFACE,
    CURLOPT_KRBLEVEL,
    CURLOPT_SSL_VERIFYPEER,
    CURLOPT_CAINFO,
    CURLOPT_MAXREDIRS,
    CURLOPT_FILETIME,
    CURLOPT_TELNETOPTIONS,
    CURLOPT_MAXCONNECTS,
    CURLOPT_FRESH_CONNECT,
    CURLOPT_FORBID_REUSE,
    CURLOPT_RANDOM_FILE,
    CURLOPT_EGDSOCKET,
    CURLOPT_CONNECTTIMEOUT,
    CURLOPT_HEADERFUNCTION,
    CURLOPT_HTTPGET,
    CURLOPT_SSL_VERIFYHOST,
    CURLOPT_COOKIEJAR,
    CURLOPT_SSL_CIPHER_LIST,
    CURLOPT_HTTP_VERSION,
    CURLOPT_FTP_USE_EPSV,
    CURLOPT_SSLCERTTYPE,
    CURLOPT_SSLKEY,
    CURLOPT_SSLKEYTYPE,
    CURLOPT_SSLENGINE,
    CURLOPT_SSLENGINE_DEFAULT,
    CURLOPT_DNS_USE_GLOBAL_CACHE,
    CURLOPT_DNS_CACHE_TIMEOUT,
    CURLOPT_PREQUOTE,
    CURLOPT_DEBUGFUNCTION,
    CURLOPT_DEBUGDATA,
    CURLOPT_COOKIESESSION,
    CURLOPT_CAPATH,
    CURLOPT_BUFFERSIZE,
    CURLOPT_NOSIGNAL,
    CURLOPT_SHARE,
    CURLOPT_PROXYTYPE,
    CURLOPT_ENCODING,
    CURLOPT_PRIVATE,
    CURLOPT_HTTP200ALIASES,
    CURLOPT_UNRESTRICTED_AUTH,
    CURLOPT_FTP_USE_EPRT,
    CURLOPT_HTTPAUTH,
    CURLOPT_SSL_CTX_FUNCTION,
    CURLOPT_SSL_CTX_DATA,
    CURLOPT_FTP_CREATE_MISSING_DIRS,
    CURLOPT_PROXYAUTH,
    CURLOPT_SERVER_RESPONSE_TIMEOUT,
    CURLOPT_IPRESOLVE,
    CURLOPT_MAXFILESIZE,
    CURLOPT_INFILESIZE_LARGE,
    CURLOPT_RESUME_FROM_LARGE,
    CURLOPT_MAXFILESIZE_LARGE,
    CURLOPT_NETRC_FILE,
    CURLOPT_USE_SSL,
    CURLOPT_POSTFIELDSIZE_LARGE,
    CURLOPT_TCP_NODELAY,
    CURLOPT_SEEKFUNCTION,
    CURLOPT_SEEKDATA,
    CURLOPT_PROTOCOLS,
    CURLOPT_REDIR_PROTOCOLS,
    CURLOPT_POSTREDIR,
    CURLOPT_USERNAME,
    CURLOPT_PASSWORD,
    CURLOPT_PROXYUSERNAME,
    CURLOPT_PROXYPASSWORD,
    CURLOPT_NOPROXY,
    CURLOPT_GSSAPI_DELEGATION,
    CURLOPT_RESOLVE,
    CURLOPT_TCP_KEEPALIVE,
    CURLOPT_TCP_KEEPIDLE,
    CURLOPT_TCP_KEEPINTVL,
    CURLOPT_SSL_OPTIONS,
    CURLOPT_PROXYHEADER,
    CURLOPT_PINNEDPUBLICKEY,
    CURLOPT_PROXY_CAINFO,
    CURLOPT_PROXY_SSLCERT,
    CURLOPT_PROXY_SSLKEY,
    CURLOPT_PROXY_KEYPASSWD,
    CURLOPT_WRITEDATA
} CURLoption;

typedef enum {
    CURLINFO_NONE = 0,
    CURLINFO_EFFECTIVE_URL,
    CURLINFO_RESPONSE_CODE,
    CURLINFO_HTTP_CONNECTCODE,
    CURLINFO_HTTPAUTH_AVAIL,
    CURLINFO_CONTENT_TYPE,
    CURLINFO_PRIVATE
} CURLINFO;

#define CURLINFO_HTTP_CODE CURLINFO_RESPONSE_CODE

typedef struct {
    int age;
    const char *version;
    unsigned int version_num;
    const char *host;
    int features;
    const char *ssl_version;
    long ssl_version_num;
    const char *libz_version;
    const char * const *protocols;
} curl_version_info_data;

#define CURLVERSION_NOW 4

typedef struct {
    int id;
    const char *name;
} curl_ssl_backend;

typedef enum {
    CURLSSLSET_OK = 0,
    CURLSSLSET_UNKNOWN_BACKEND = 1,
    CURLSSLSET_TOO_LATE = 2,
    CURLSSLSET_NO_BACKENDS = 3
} CURLsslset;

#ifdef __cplusplus
extern "C" {
#endif

void curl_free(void *p);
CURLcode curl_global_init(long flags);
void curl_global_cleanup(void);
CURLsslset curl_global_sslset(int id, const char *name, const curl_ssl_backend ***avail);
curl_version_info_data *curl_version_info(int age);

CURL *curl_easy_init(void);
void curl_easy_cleanup(CURL *curl);
CURL *curl_easy_duphandle(CURL *curl);
CURLcode curl_easy_setopt(CURL *curl, CURLoption option, ...);
CURLcode curl_easy_perform(CURL *curl);
CURLcode curl_easy_getinfo(CURL *curl, CURLINFO info, ...);
const char *curl_easy_strerror(CURLcode code);

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *data);
void curl_slist_free_all(struct curl_slist *list);

CURLM *curl_multi_init(void);
CURLMcode curl_multi_cleanup(CURLM *multi_handle);
CURLMcode curl_multi_add_handle(CURLM *multi_handle, CURL *curl_handle);
CURLMcode curl_multi_remove_handle(CURLM *multi_handle, CURL *curl_handle);
CURLMcode curl_multi_perform(CURLM *multi_handle, int *running_handles);
CURLMcode curl_multi_fdset(CURLM *multi_handle, fd_set *read_fd_set, fd_set *write_fd_set, fd_set *exc_fd_set, int *max_fd);
CURLMcode curl_multi_timeout(CURLM *multi_handle, long *milliseconds);
CURLMsg *curl_multi_info_read(CURLM *multi_handle, int *msgs_in_queue);
const char *curl_multi_strerror(CURLMcode code);

#ifdef __cplusplus
}
#endif

#endif
