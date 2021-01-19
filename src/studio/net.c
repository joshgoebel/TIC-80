// MIT License

// Copyright (c) 2017 Vadim Grigoruk @nesbox // grigoruk@gmail.com

// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:

// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.

// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include "net.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define URL_SIZE 2048

#if defined(__EMSCRIPTEN__)

#include <emscripten/fetch.h>

typedef struct
{
    HttpGetCallback callback;
    void* calldata;
} FetchData;

struct Net
{
    emscripten_fetch_attr_t attr;
};

static void downloadSucceeded(emscripten_fetch_t *fetch) 
{
    FetchData* data = (FetchData*)fetch->userData;

    HttpGetData getData = 
    {
        .type = HttpGetDone,
        .done = 
        {
            .size = fetch->numBytes,
            .data = (u8*)fetch->data,
        },
        .calldata = data->calldata,
        .url = fetch->url,
    };

    data->callback(&getData);

    free(data);

    emscripten_fetch_close(fetch);
}

static void downloadFailed(emscripten_fetch_t *fetch) 
{
    FetchData* data = (FetchData*)fetch->userData;

    HttpGetData getData = 
    {
        .type = HttpGetError,
        .error = 
        {
            .code = fetch->status,
        },
        .calldata = data->calldata,
        .url = fetch->url,
    };

    data->callback(&getData);

    free(data);

    emscripten_fetch_close(fetch);
}

static void downloadProgress(emscripten_fetch_t *fetch) 
{
    FetchData* data = (FetchData*)fetch->userData;

    HttpGetData getData = 
    {
        .type = HttpGetProgress,
        .progress = 
        {
            .size = fetch->dataOffset + fetch->numBytes,
            .total = fetch->totalBytes,
        },
        .calldata = data->calldata,
        .url = fetch->url,
    };

    data->callback(&getData);
}

void netGet(Net* net, const char* path, HttpGetCallback callback, void* calldata)
{
    FetchData* data = calloc(1, sizeof(FetchData));
    *data = (FetchData)
    {
        .callback = callback,
        .calldata = calldata,
    };

    net->attr.userData = data;
    emscripten_fetch(&net->attr, path);
}

void* netGetSync(Net* net, const char* path, s32* size)
{
    return NULL;
}

void netTickStart(Net *net) {}
void netTickEnd(Net *net) {}

Net* netCreate(const char* host)
{
    Net* net = (Net*)malloc(sizeof(Net));

    emscripten_fetch_attr_init(&net->attr);
    strcpy(net->attr.requestMethod, "GET");
    net->attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY;
    net->attr.onsuccess = downloadSucceeded;
    net->attr.onerror = downloadFailed;
    net->attr.onprogress = downloadProgress;

    return net;
}

void netClose(Net* net)
{
    free(net);
}

#elif defined(_3DS)

#include <3ds.h>

struct Net
{
    LightLock tick_lock;
    const char* host;
};

typedef struct {
    char url[URL_SIZE];

    Net *net;
    httpcContext httpc;
    HttpGetData data;
    HttpGetCallback callback;

    void *buffer;
    s32 size;
} net_ctx;

// #define DEBUG

static int n3ds_net_setup_context(httpcContext *httpc, const char *url) {
    if (httpcOpenContext(httpc, HTTPC_METHOD_GET, url, 1)) return -101;
    if (httpcSetSSLOpt(httpc, SSLCOPT_DisableVerify)) { httpcCloseContext(httpc); return -102; }
    if (httpcSetKeepAlive(httpc, HTTPC_KEEPALIVE_ENABLED)) { httpcCloseContext(httpc); return -103; }
    if (httpcAddRequestHeaderField(httpc, "User-Agent", "tic80-n3ds/1.0.0 (httpc)")) { httpcCloseContext(httpc); return -104; }
    if (httpcAddRequestHeaderField(httpc, "Connection", "Keep-Alive")) { httpcCloseContext(httpc); return -105; }
    return 0;
}

#define NET_PAGE_SIZE 4096

static inline bool ctx_resize_buf(net_ctx *ctx, s32 new_size) {
    if (ctx->buffer == NULL) {
        ctx->buffer = malloc(new_size);
        if (ctx->buffer == NULL) {
            return false;
        }
    } else if (ctx->size != new_size) {
        void *old_buf = ctx->buffer;
        ctx->buffer = realloc(ctx->buffer, new_size);
        if (ctx->buffer == NULL) {
            free(old_buf);
            return false;
        }
    }

    ctx->size = new_size;
    return true;
}

#define NET_EXEC_ERROR_CHECK \
    if (status_code != 200) { \
        printf("net_httpc: error %d\n", status_code); \
        if (ctx->callback != NULL) { \
            ctx->data.type = HttpGetError; \
            ctx->data.error.code = status_code; \
            if (!ignore_lock) LightLock_Lock(&ctx->net->tick_lock); \
            ctx->callback(&ctx->data); \
            if (!ignore_lock) LightLock_Unlock(&ctx->net->tick_lock); \
        } \
        httpcCloseContext(&ctx->httpc); \
        if (ctx->buffer != NULL) { free(ctx->buffer); ctx->size = 0; } \
        return; \
    }

static void n3ds_net_execute(net_ctx *ctx, bool ignore_lock) {
    bool redirecting = true;
    s32 status_code = -1;

    ctx->data.url = ctx->url;
    while (redirecting) {
#ifdef DEBUG
        printf("url: %s\n", ctx->url);
#endif
        redirecting = false;

        status_code = n3ds_net_setup_context(&ctx->httpc, ctx->url);
        if (status_code < 0) {
            break;
        }

        if (httpcBeginRequest(&ctx->httpc)) {
            status_code = -2;
            break;
        }

        if (httpcGetResponseStatusCode(&ctx->httpc, &status_code)) {
            status_code = -3;
            break;
        }

        if ((status_code >= 301 && status_code <= 303) || (status_code >= 307 && status_code <= 308)) {
            if (httpcGetResponseHeader(&ctx->httpc, "Location", ctx->url, URL_SIZE - 1)) {
                status_code = -4;
                break;
            }

            redirecting = true;
            httpcCloseContext(&ctx->httpc);
        }
    }

    NET_EXEC_ERROR_CHECK;

    s32 state = HTTPC_RESULTCODE_DOWNLOADPENDING;
    s32 read_size;
    while (state == HTTPC_RESULTCODE_DOWNLOADPENDING) {
        s32 old_size = ctx->size;
        if (!ctx_resize_buf(ctx, ctx->size + NET_PAGE_SIZE)) {
            httpcCloseContext(&ctx->httpc);
            status_code = -5;
            break;
        }
        u8 *old_ptr = ((u8*) ctx->buffer) + old_size;
        state = httpcDownloadData(&ctx->httpc, old_ptr, NET_PAGE_SIZE, &read_size);
        if (state == HTTPC_RESULTCODE_DOWNLOADPENDING || state == 0) {
            ctx_resize_buf(ctx, old_size + read_size);
            if (ctx->callback != NULL) {
                if (ignore_lock || !LightLock_TryLock(&ctx->net->tick_lock)) {
                    ctx->data.type = HttpGetProgress;
                    if (!httpcGetDownloadSizeState(&ctx->httpc, &ctx->data.progress.size, &ctx->data.progress.total)) {
                        if (ctx->data.progress.total < ctx->data.progress.size) {
                            ctx->data.progress.total = ctx->data.progress.size;
                        }
                        ctx->callback(&ctx->data);
                    }
                    if (!ignore_lock) LightLock_Unlock(&ctx->net->tick_lock);
                }
            }
        }
    }

#ifdef DEBUG
    printf("downloaded: %d bytes\n", ctx->size);
#endif

    if (status_code == 200 && state != 0) {
        status_code = -6;
    }
    NET_EXEC_ERROR_CHECK;

    if (ctx->callback != NULL) {
        ctx->data.type = HttpGetDone;
        ctx->data.done.data = ctx->buffer;
        ctx->data.done.size = ctx->size;
        if (!ignore_lock) LightLock_Lock(&ctx->net->tick_lock);
        ctx->callback(&ctx->data);
        if (!ignore_lock) LightLock_Unlock(&ctx->net->tick_lock);
    }
    httpcCloseContext(&ctx->httpc);
}

static void n3ds_net_init(Net *net) {
    httpcInit(0);

    memset(net, 0, sizeof(Net));
    LightLock_Init(&net->tick_lock);
}

static void n3ds_net_free(Net *net) {
    httpcExit();
}

static void n3ds_net_get_thread(net_ctx *ctx) {
    n3ds_net_execute(ctx, false);

    if (ctx->buffer != NULL) {
        free(ctx->buffer);
    }
    free(ctx);
}

static void n3ds_net_apply_url(net_ctx *ctx, const char *url) {
    strncpy(ctx->url, ctx->net->host, URL_SIZE - 1);
    strncat(ctx->url, url, URL_SIZE - 1);
}

static void n3ds_net_get(Net *net, const char *url, HttpGetCallback callback, void *calldata) {
    s32 priority;
    net_ctx *ctx;

    ctx = malloc(sizeof(net_ctx));
    memset(ctx, 0, sizeof(net_ctx));

    n3ds_net_apply_url(ctx, url);
    ctx->net = net;
    ctx->callback = callback;
    ctx->data.calldata = calldata;

    svcGetThreadPriority(&priority, CUR_THREAD_HANDLE);
    threadCreate((ThreadFunc) n3ds_net_get_thread, ctx, 16 * 1024, priority - 1, -1, true);
}

static void* n3ds_net_get_sync(Net *net, const char *url, s32 *size) {
    net_ctx ctx;
    memset(&ctx, 0, sizeof(net_ctx));

    n3ds_net_apply_url(&ctx, url);
    ctx.net = net;
    n3ds_net_execute(&ctx, true);

    if (size != NULL) {
        *size = ctx.size;
    }
    return ctx.buffer;
}

Net* netCreate(const char* host)
{
    Net* net = (Net*)malloc(sizeof(Net));

    n3ds_net_init(net);
    net->host = host;

    return net;
}

void* netGetSync(Net* net, const char* path, s32* size)
{
    return n3ds_net_get_sync(net, path, size);
}

void netGet(Net* net, const char* url, HttpGetCallback callback, void* calldata)
{
    n3ds_net_get(net, url, callback, calldata);
}

void netClose(Net* net)
{
    n3ds_net_free(net);
    free(net);
}

void netTickStart(Net *net)
{
    LightLock_Lock(&net->tick_lock);
}

void netTickEnd(Net *net)
{
    LightLock_Unlock(&net->tick_lock);
}

#elif defined(BAREMETALPI)

Net* netCreate(const char* host) {return NULL;}
void* netGetSync(Net* net, const char* path, s32* size) {}
void netGet(Net* net, const char* url, HttpGetCallback callback, void* calldata) {}
void netClose(Net* net) {}
void netTickStart(Net *net) {}
void netTickEnd(Net *net) {}

#else

#include <curl/curl.h>

typedef struct
{
    u8* buffer;
    s32 size;

    struct Curl_easy* async;
    HttpGetCallback callback;
    void* calldata;
    char url[URL_SIZE];
} CurlData;

struct Net
{
    const char* host;
    CURLM* multi;
    struct Curl_easy* sync;
};

static size_t writeCallbackSync(void *contents, size_t size, size_t nmemb, void *userp)
{
    CurlData* data = (CurlData*)userp;

    const size_t total = size * nmemb;
    u8* newBuffer = realloc(data->buffer, data->size + total);
    if (newBuffer == NULL)
    {
        free(data->buffer);
        return 0;
    }
    data->buffer = newBuffer;
    memcpy(data->buffer + data->size, contents, total);
    data->size += (s32)total;

    return total;
}

static size_t writeCallback(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    CurlData* data = (CurlData*)userdata;

    const size_t total = size * nmemb;
    u8* newBuffer = realloc(data->buffer, data->size + total);
    if (newBuffer == NULL)
    {
        free(data->buffer);
        return 0;
    }
    data->buffer = newBuffer;
    memcpy(data->buffer + data->size, ptr, total);
    data->size += (s32)total;

    double cl;
    curl_easy_getinfo(data->async, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &cl);

    if(cl > 0.0)
    {
        HttpGetData getData = 
        {
            .type = HttpGetProgress,
            .progress = 
            {
                .size = data->size,
                .total = (s32)cl,
            },
            .calldata = data->calldata,
            .url = data->url,
        };

        data->callback(&getData);
    }

    return total;
}

void netGet(Net* net, const char* path, HttpGetCallback callback, void* calldata)
{
    struct Curl_easy* curl = curl_easy_init();

    CurlData* data = calloc(1, sizeof(CurlData));
    *data = (CurlData)
    {
        .async = curl,
        .callback = callback,
        .calldata = calldata,
    };

    strcpy(data->url, path);

    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);

    {
        char url[URL_SIZE];
        strcpy(url, net->host);
        strcat(url, path);

        curl_easy_setopt(curl, CURLOPT_URL, url);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, data);
        curl_easy_setopt(curl, CURLOPT_PRIVATE, data);

        curl_multi_add_handle(net->multi, curl);
    }
}

void* netGetSync(Net* net, const char* path, s32* size)
{
    CurlData data = {NULL, 0};

    if(net->sync)
    {
        char url[URL_SIZE];
        strcpy(url, net->host);
        strcat(url, path);

        curl_easy_setopt(net->sync, CURLOPT_URL, url);
        curl_easy_setopt(net->sync, CURLOPT_WRITEDATA, &data);

        if(curl_easy_perform(net->sync) == CURLE_OK)
        {
            long httpCode = 0;
            curl_easy_getinfo(net->sync, CURLINFO_RESPONSE_CODE, &httpCode);
            if(httpCode != 200) return NULL;
        }
        else return NULL;
    }

    *size = data.size;

    return data.buffer;
}

void netTickStart(Net *net)
{
    {
        s32 running = 0;
        curl_multi_perform(net->multi, &running);
    }

    s32 pending = 0;
    CURLMsg* msg = NULL;

    while((msg = curl_multi_info_read(net->multi, &pending)))
    {
        if(msg->msg == CURLMSG_DONE)
        {
            CurlData* data = NULL;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_PRIVATE, (char**)&data);

            long httpCode = 0;
            curl_easy_getinfo(msg->easy_handle, CURLINFO_RESPONSE_CODE, &httpCode);

            if(httpCode == 200)
            {
                HttpGetData getData = 
                {
                    .type = HttpGetDone,
                    .done = 
                    {
                        .size = data->size,
                        .data = data->buffer,
                    },
                    .calldata = data->calldata,
                    .url = data->url,
                };

                data->callback(&getData);

                free(data->buffer);
            }
            else
            {
                HttpGetData getData = 
                {
                    .type = HttpGetError,
                    .error = 
                    {
                        .code = httpCode,
                    },
                    .calldata = data->calldata,
                    .url = data->url,
                };

                data->callback(&getData);
            }

            free(data);
            
            curl_multi_remove_handle(net->multi, msg->easy_handle);
            curl_easy_cleanup(msg->easy_handle);
        }
    }
}

void netTickEnd(Net *net) {}

Net* netCreate(const char* host)
{
    Net* net = (Net*)malloc(sizeof(Net));

    if (net != NULL)
    {
        *net = (Net)
        {
            .sync = curl_easy_init(),
            .multi = curl_multi_init(),
            .host = host,
        };

        curl_easy_setopt(net->sync, CURLOPT_WRITEFUNCTION, writeCallbackSync);
    }

    return net;
}

void netClose(Net* net)
{
    if(net->sync)
        curl_easy_cleanup(net->sync);

    if(net->multi)
        curl_multi_cleanup(net->multi);

    free(net);
}

#endif