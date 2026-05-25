#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>
#include <shellapi.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_HANDLES 100
#define MAX_CONTESTS 500
#define MAX_PROBS 15
#define MAX_SUBMISSIONS 2000
#define MAX_SOLVED 2000

typedef struct { char handle[64]; char avatar[256]; int curRating, maxRating; char title[64]; int contestCount, cnt180, max180; } UserInfo;
typedef struct { int contestId; char contestName[256]; int64_t startTime; int durationSeconds; int oldRating, newRating, rank; int problemCount; char labels[MAX_PROBS][4]; int status[MAX_PROBS]; } Entry;
typedef struct { int contestId; int64_t time; char index[4]; } Sub;
typedef struct { char problemId[32]; int rating; int64_t time; } SolvedProb;

typedef struct { char *buf; size_t len; } Buf;
static char last_error[1024] = {0};
static void set_last_error(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(last_error, sizeof(last_error), fmt, ap); va_end(ap);
}
/* forward declare winhttp fallback used by http_get */
static char *winhttp_get(const char *url);
static FILE *dbg_fp = NULL;
static size_t write_cb(void *p, size_t sz, size_t n, void *u) {
    Buf *b = u; size_t r = sz * n;
    char *q = realloc(b->buf, b->len + r + 1); if (!q) return 0;
    memcpy(q + b->len, p, r); b->len += r; q[b->len] = 0; b->buf = q; return r;
}

static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) { fprintf(stderr, "curl_easy_init failed\n"); return NULL; }
    Buf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    /* Set a default User-Agent and accept compressed responses */
    curl_easy_setopt(c, CURLOPT_USERAGENT, "cf_tool/1.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode r = curl_easy_perform(c);
    if (r != CURLE_OK) {
        set_last_error("curl error for '%s': %s", url, curl_easy_strerror(r));
        fprintf(stderr, "%s\n", last_error);
        curl_easy_cleanup(c);
        if (b.buf) free(b.buf);
        /* Fall back to WinHTTP on Windows */
        return winhttp_get(url);
    }
    long http_code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(c);
    if (http_code != 200) {
        set_last_error("HTTP %ld for '%s'", http_code, url);
        fprintf(stderr, "%s\n", last_error);
        if (b.buf) free(b.buf);
        return winhttp_get(url);
    }
    return b.buf;
}

/* Minimal WinHTTP fallback for environments where libcurl fails */
/* no WinHTTP fallback on mingw for now; keep http_get using libcurl only */
static char *winhttp_get(const char *url) { (void)url; return NULL; }

static char *cf_api(const char *fmt, ...) {
    char url[512]; va_list ap; va_start(ap, fmt); vsnprintf(url, 512, fmt, ap); va_end(ap);
    return http_get(url);
}

static const char *rcol(int r) {
    if (r >= 2400) return "#FF0000";
    if (r >= 2100) return "#FF8C00";
    if (r >= 1900) return "#AA00AA";
    if (r >= 1600) return "#0000FF";
    if (r >= 1400) return "#03A89E";
    if (r >= 1200) return "#008000";
    return "#808080";
}

static void copy_str(char *dst, const char *src, size_t dst_size) {
    if (!dst || dst_size == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, dst_size - 1);
    dst[dst_size - 1] = '\0';
}

static const char *translate_title(const char *title) {
    if (!title || !title[0]) return "";
    char t[64];
    copy_str(t, title, sizeof(t));
    for (int i = 0; t[i]; i++) if (t[i] >= 'A' && t[i] <= 'Z') t[i] += 32;
    if (strstr(t, "world top")) return "世界顶尖";
    if (strstr(t, "legendary grandmaster")) return "传奇特级大师";
    if (strstr(t, "international grandmaster")) return "国际特级大师";
    if (strstr(t, "grandmaster")) return "特级大师";
    if (strstr(t, "international master")) return "国际大师";
    if (strstr(t, "candidate master")) return "候选大师";
    if (strstr(t, "master")) return "大师";
    if (strstr(t, "expert")) return "行家";
    if (strstr(t, "specialist")) return "专家";
    if (strstr(t, "pupil")) return "学徒";
    if (strstr(t, "newbie")) return "新手";
    return "";
}

static void fmt_time(char *buf, int64_t t) { strftime(buf, 64, "%Y-%m-%d %H:%M", localtime((time_t*)&t)); }

static const char *jstr_safe(cJSON *o, const char *k) {
    if (!o || !k) return "";
    cJSON *it = cJSON_GetObjectItemCaseSensitive(o, k);
    if (it && cJSON_IsString(it) && it->valuestring) return it->valuestring;
    return "";
}
#define JNUM(o,k) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(o,k)) ? cJSON_GetObjectItemCaseSensitive(o,k)->valueint : 0)
#define JSTR(o,k) jstr_safe(o,k)


static void parse_handles(char handles[][64], int *n, const char *src) {
    char buf[4096]; copy_str(buf, src, sizeof(buf));
    char *tok = strtok(buf, " ");
    while (tok && *n < MAX_HANDLES) {
        copy_str(handles[*n], tok, 64);
        if (handles[*n][0]) (*n)++;
        tok = strtok(NULL, " ");
    }
}

int main(int argc, char **argv) {
    SetConsoleOutputCP(CP_UTF8);
    curl_global_init(CURL_GLOBAL_ALL);
    dbg_fp = fopen("cf_tool_debug.log", "w");
    if (dbg_fp) {
        time_t t = time(NULL); fprintf(dbg_fp, "cf_tool start: %s\n", ctime(&t)); fflush(dbg_fp);
    }

    char handles[MAX_HANDLES][64];
    int nhandles = 0;

    if (argc > 1) {
        for (int i = 1; i < argc && nhandles < MAX_HANDLES; i++) {
            copy_str(handles[nhandles], argv[i], 64);
            if (handles[nhandles][0]) nhandles++;
        }
        printf("已读取到 %d 个用户\n", nhandles);
    } else {
        printf("Codeforces用户信息查询 \n");
        printf("(支持多用户查询，如有多个用户，用户名之间使用空格间隔)\n");
        printf("输入 Codeforces 用户名: \n");

        char buf[4096];
        if (!fgets(buf, sizeof(buf), stdin)) return 1;
        buf[strcspn(buf, "\n")] = 0;
        parse_handles(handles, &nhandles, buf);
    }
    if (nhandles == 0) { printf("未识别到有效的用户名\n"); return 1; }

    printf("\n=== 获取用户基本信息 ===\n");
    char handle_list[4096] = "";
    for (int i = 0; i < nhandles; i++) { if (i) strcat(handle_list, ";"); strcat(handle_list, handles[i]); }
    if (dbg_fp) { fprintf(dbg_fp, "handles: %s\n", handle_list); fflush(dbg_fp); }

    char *s = NULL;
    /* try aggregated user.info with retries */
    for (int attempt = 0; attempt < 3 && !s; attempt++) {
        s = cf_api("https://codeforces.com/api/user.info?handles=%s", handle_list);
        if (!s) { Sleep(1000 * (attempt + 1)); }
    }

    UserInfo users[MAX_HANDLES];
    int nusers = 0;

    if (s) {
        cJSON *root = cJSON_Parse(s);
        if (!root) { free(s); fprintf(stderr, "解析 user.info 失败\n"); s = NULL; }
        else {
            cJSON *result = cJSON_GetObjectItemCaseSensitive(root, "result");
            cJSON *status = cJSON_GetObjectItemCaseSensitive(root, "status");
            if (status && cJSON_IsString(status) && strcmp(status->valuestring, "OK") == 0 && result && cJSON_IsArray(result)) {
                int cnt = cJSON_GetArraySize(result); if (cnt > nhandles) cnt = nhandles;
                for (int i = 0; i < cnt; i++) {
                    cJSON *u = cJSON_GetArrayItem(result, i);
                    const char *h = JSTR(u, "handle");
                    if (h[0]) {
                        UserInfo *p = &users[nusers++];
                        memset(p, 0, sizeof(UserInfo));
                        copy_str(p->handle, h, sizeof(p->handle));
                        copy_str(p->avatar, JSTR(u, "titlePhoto"), sizeof(p->avatar));
                        if (!p->avatar[0]) copy_str(p->avatar, JSTR(u, "avatar"), sizeof(p->avatar));
                        p->curRating = JNUM(u, "rating");
                        p->maxRating = JNUM(u, "maxRating");
                        copy_str(p->title, JSTR(u, "rank"), sizeof(p->title));
                    }
                }
            } else {
                fprintf(stderr, "user.info 返回格式异常\n");
            }
            free(s); cJSON_Delete(root);
            if (dbg_fp) { fprintf(dbg_fp, "user.info parsed: %d users\n", nusers); fflush(dbg_fp); }
        }
    }

    /* If aggregated call failed, try per-handle requests as a fallback */
    if (!s && nusers == 0) {
        for (int hi = 0; hi < nhandles; hi++) {
            char *si = NULL;
            for (int attempt = 0; attempt < 3 && !si; attempt++) {
                si = cf_api("https://codeforces.com/api/user.info?handles=%s", handles[hi]);
                if (!si) Sleep(500);
            }
            if (!si) { fprintf(stderr, "无法获取用户 %s 信息: %s\n", handles[hi], last_error); continue; }
            cJSON *r = cJSON_Parse(si);
            if (!r) { free(si); continue; }
            cJSON *status = cJSON_GetObjectItemCaseSensitive(r, "status");
            cJSON *result = cJSON_GetObjectItemCaseSensitive(r, "result");
            if (status && cJSON_IsString(status) && strcmp(status->valuestring, "OK") == 0 && result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
                cJSON *u = cJSON_GetArrayItem(result, 0);
                const char *h = JSTR(u, "handle");
                if (h[0]) {
                    UserInfo *p = &users[nusers++]; memset(p, 0, sizeof(UserInfo)); copy_str(p->handle, h, sizeof(p->handle));
                    copy_str(p->avatar, JSTR(u, "titlePhoto"), sizeof(p->avatar));
                    if (!p->avatar[0]) copy_str(p->avatar, JSTR(u, "avatar"), sizeof(p->avatar));
                    p->curRating = JNUM(u, "rating"); p->maxRating = JNUM(u, "maxRating"); copy_str(p->title, JSTR(u, "rank"), sizeof(p->title));
                }
            } else {
                fprintf(stderr, "user.info 非 OK 或无结果: %s\n", handles[hi]);
            }
            free(si); cJSON_Delete(r);
            if (dbg_fp) { fprintf(dbg_fp, "fetched per-user %s, now nusers=%d\n", handles[hi], nusers); fflush(dbg_fp); }
        }
    }
    /* Report any requested handles that were not found by user.info */
    char missing_handles[MAX_HANDLES][64]; int nmissing = 0;
    for (int hi = 0; hi < nhandles; hi++) {
        int found = 0;
        for (int ui = 0; ui < nusers; ui++) if (strcmp(handles[hi], users[ui].handle) == 0) { found = 1; break; }
        if (!found) {
            printf("未找到用户: %s\n", handles[hi]);
            copy_str(missing_handles[nmissing], handles[hi], sizeof(missing_handles[nmissing])); nmissing++;
        }
    }
    if (nusers == 0) { printf("未找到任何用户，请检查用户名\n"); return 1; }

    printf("\n=== 获取题目信息 ===\n");
    char *ps_json = cf_api("https://codeforces.com/api/problemset.problems");
    int *pmap_ids = NULL, *pmap_ratings = NULL, pmap_n = 0;
    char (*pmap_indices)[4] = NULL;
    if (ps_json) {
        cJSON *ps = cJSON_Parse(ps_json);
        if (ps) {
            cJSON *res = cJSON_GetObjectItemCaseSensitive(ps, "result");
            cJSON *probs = res ? cJSON_GetObjectItemCaseSensitive(res, "problems") : NULL;
            if (probs && cJSON_IsArray(probs)) {
                int sz = cJSON_GetArraySize(probs);
                pmap_ids = malloc(sizeof(int) * sz);
                pmap_ratings = malloc(sizeof(int) * sz);
                pmap_indices = malloc(sizeof(char[4]) * sz);
                for (int i = 0; i < sz; i++) {
                    cJSON *p = cJSON_GetArrayItem(probs, i);
                    int cid = JNUM(p, "contestId");
                    const char *idx = JSTR(p, "index");
                    int rat = JNUM(p, "rating");
                    if (cid && idx[0] && rat) {
                        pmap_ids[pmap_n] = cid; pmap_ratings[pmap_n] = rat;
                        copy_str(pmap_indices[pmap_n], idx, 4);
                        pmap_n++;
                    }
                }
            } else {
                printf("problemset.problems 返回格式异常\n");
            }
            cJSON_Delete(ps);
        } else {
            printf("解析 problemset.problems 失败\n");
        }
        free(ps_json);
    }

    /* 获取 contest.list 一次性数据并保留到后面使用 */
    char *cl_json_all = cf_api("https://codeforces.com/api/contest.list?gym=false");
    cJSON *cl_root = NULL; cJSON *cl_arr = NULL; int ncl = 0;
    if (cl_json_all) {
        cl_root = cJSON_Parse(cl_json_all);
        if (cl_root) {
            cJSON *cres = cJSON_GetObjectItemCaseSensitive(cl_root, "result");
            if (cres && cJSON_IsArray(cres)) { cl_arr = cres; ncl = cJSON_GetArraySize(cres); }
            else printf("contest.list 返回格式异常\n");
        } else {
            printf("解析 contest.list 失败\n");
        }
        free(cl_json_all);
    }

    printf("\n=== 获取用户详细数据 ===\n");
    Entry *entries[MAX_HANDLES]; int nentries[MAX_HANDLES];
    SolvedProb *solved[MAX_HANDLES]; int nsolved[MAX_HANDLES];
    Sub *user_subs_list[MAX_HANDLES]; int user_subs_cnt[MAX_HANDLES];

    for (int ui = 0; ui < nusers; ui++) {
        UserInfo *u = &users[ui];
        printf("[%d/%d] %s\n", ui + 1, nusers, u->handle);
        if (dbg_fp) { fprintf(dbg_fp, "--- start user %s (%d/%d) ---\n", u->handle, ui+1, nusers); fflush(dbg_fp); }

        /* user.rating */
        s = cf_api("https://codeforces.com/api/user.rating?handle=%s", u->handle);
        cJSON *rt = NULL; cJSON *rt_arr = NULL; int n = 0;
        if (s) {
            if (dbg_fp) { fprintf(dbg_fp, "user.rating raw len=%d\n", (int)strlen(s)); fwrite(s, 1, strlen(s) < 200 ? strlen(s) : 200, dbg_fp); fprintf(dbg_fp, "\n----\n"); fflush(dbg_fp); }
            rt = cJSON_Parse(s);
            if (rt) {
                cJSON *rres = cJSON_GetObjectItemCaseSensitive(rt, "result");
                if (rres && cJSON_IsArray(rres)) { rt_arr = rres; n = cJSON_GetArraySize(rres); if (n > MAX_CONTESTS) n = MAX_CONTESTS; }
                else { printf("user.rating 数据不包含 result 数组: %s\n", u->handle); }
            } else { printf("解析 user.rating 失败: %s\n", u->handle); }
            free(s);
        }
        if (dbg_fp) { fprintf(dbg_fp, "user.rating entries=%d\n", n); fflush(dbg_fp); }

        /* user.status (submissions) */
        char *sub_s = cf_api("https://codeforces.com/api/user.status?handle=%s&from=1&count=%d", u->handle, MAX_SUBMISSIONS);
        cJSON *sub = NULL; cJSON *sub_arr = NULL; int nsub = 0;
        if (sub_s) {
            if (dbg_fp) { fprintf(dbg_fp, "user.status raw len=%d\n", (int)strlen(sub_s)); fwrite(sub_s, 1, strlen(sub_s) < 200 ? strlen(sub_s) : 200, dbg_fp); fprintf(dbg_fp, "\n----\n"); fflush(dbg_fp); }
            sub = cJSON_Parse(sub_s);
            if (sub) {
                cJSON *sres = cJSON_GetObjectItemCaseSensitive(sub, "result");
                if (sres && cJSON_IsArray(sres)) { sub_arr = sres; nsub = cJSON_GetArraySize(sres); }
                else { printf("user.status 没有 result 数组: %s\n", u->handle); }
            } else { printf("解析 user.status 失败: %s\n", u->handle); }
            free(sub_s);
        }
        if (dbg_fp) { fprintf(dbg_fp, "user.status entries=%d\n", nsub); fflush(dbg_fp); }

        Sub *subs = malloc(sizeof(Sub) * (nsub > 0 ? nsub : 1)); int sub_cnt = 0;
        if (!subs) { fprintf(stderr, "malloc subs failed\n"); if (dbg_fp) fprintf(dbg_fp, "malloc subs failed for %s\n", u->handle); continue; }
        for (int i = 0; i < nsub; i++) {
            cJSON *x = cJSON_GetArrayItem(sub_arr, i); if (!x) continue;
            int cid = JNUM(x, "contestId");
            cJSON *ct = cJSON_GetObjectItemCaseSensitive(x, "creationTimeSeconds");
            int64_t tm = (ct && cJSON_IsNumber(ct)) ? (int64_t)ct->valuedouble : 0;
            const char *vd = JSTR(x, "verdict");
            cJSON *pr = cJSON_GetObjectItemCaseSensitive(x, "problem");
            const char *ix = pr ? JSTR(pr, "index") : "";
            if (cid && tm && vd[0] && ix[0] && strcmp(vd, "OK") == 0) {
                subs[sub_cnt].contestId = cid; subs[sub_cnt].time = tm;
                copy_str(subs[sub_cnt].index, ix, sizeof(subs[sub_cnt].index)); sub_cnt++;
            }
        }
        if (dbg_fp) { fprintf(dbg_fp, "built subs count=%d\n", sub_cnt); fflush(dbg_fp); }
        user_subs_list[ui] = subs;
        user_subs_cnt[ui] = sub_cnt;

        solved[ui] = calloc(MAX_SOLVED, sizeof(SolvedProb));
        nsolved[ui] = 0;
        for (int i = 0; i < nsub; i++) {
            cJSON *x = cJSON_GetArrayItem(sub_arr, i); if (!x) continue;
            if (strcmp(JSTR(x, "verdict"), "OK") != 0) continue;
            int cid = JNUM(x, "contestId");
            cJSON *pr = cJSON_GetObjectItemCaseSensitive(x, "problem");
            const char *ix = pr ? JSTR(pr, "index") : "";
            if (!cid || !ix[0]) continue;
            char pid[32]; snprintf(pid, 32, "%d_%s", cid, ix);
            int dup = 0;
            for (int j = 0; j < nsolved[ui]; j++) if (strcmp(solved[ui][j].problemId, pid) == 0) { dup = 1; break; }
            if (dup) continue;
            copy_str(solved[ui][nsolved[ui]].problemId, pid, sizeof(solved[ui][nsolved[ui]].problemId));
            cJSON *ct2 = cJSON_GetObjectItemCaseSensitive(x, "creationTimeSeconds");
            solved[ui][nsolved[ui]].time = (ct2 && cJSON_IsNumber(ct2)) ? (int64_t)ct2->valuedouble : 0;
            nsolved[ui]++;
        }

        entries[ui] = calloc(n > 0 ? n : 1, sizeof(Entry));
        nentries[ui] = n;

        int64_t now = time(NULL);
        u->contestCount = n; u->cnt180 = 0; u->max180 = 0;

        for (int i = 0; i < n; i++) {
            if (!rt_arr) break;
            cJSON *x = cJSON_GetArrayItem(rt_arr, i); if (!x) continue;
            Entry *e = &entries[ui][i]; memset(e, 0, sizeof(Entry));
            int cid = JNUM(x, "contestId");
            e->contestId = cid;
            copy_str(e->contestName, JSTR(x, "contestName"), sizeof(e->contestName));
            e->oldRating = JNUM(x, "oldRating");
            e->newRating = JNUM(x, "newRating");
            e->rank = JNUM(x, "rank");
            cJSON *rt_time = cJSON_GetObjectItemCaseSensitive(x, "ratingUpdateTimeSeconds");
            e->startTime = (rt_time && cJSON_IsNumber(rt_time)) ? (int64_t)rt_time->valuedouble : 0;
            e->durationSeconds = 7200;

            for (int k = 0; k < ncl; k++) {
                cJSON *cl = cJSON_GetArrayItem(cl_arr, k);
                if (!cl) continue;
                if (JNUM(cl, "id") == cid) {
                    cJSON *dur = cJSON_GetObjectItemCaseSensitive(cl, "durationSeconds");
                    cJSON *stm = cJSON_GetObjectItemCaseSensitive(cl, "startTimeSeconds");
                    if (dur && cJSON_IsNumber(dur)) e->durationSeconds = dur->valueint;
                    if (stm && cJSON_IsNumber(stm)) e->startTime = (int64_t)stm->valuedouble;
                    break;
                }
            }

            e->problemCount = 0;
            for (int k = 0; k < pmap_n && e->problemCount < MAX_PROBS; k++) {
                if (pmap_ids[k] == cid) {
                    copy_str(e->labels[e->problemCount], pmap_indices[k], sizeof(e->labels[e->problemCount]));
                    e->problemCount++;
                }
            }

            if (e->startTime >= now - 180 * 86400LL) { u->cnt180++; if (e->newRating > u->max180) u->max180 = e->newRating; }
            printf("  %d/%d\r", i + 1, n); fflush(stdout);
        }
        printf("\n");
        if (rt) cJSON_Delete(rt);
    }

    printf("\n=== 汇总过题状态与赛后补题 ===\n");
    for (int ui = 0; ui < nusers; ui++) {
        Entry *eu = entries[ui];
        Sub *subs = user_subs_list[ui];
        int sub_cnt = user_subs_cnt[ui];
        for (int i = 0; i < nentries[ui]; i++) {
            Entry *e = &eu[i];
            int64_t end = e->startTime + e->durationSeconds;
            for (int p = 0; p < e->problemCount; p++) e->status[p] = 0;
            for (int p = 0; p < sub_cnt; p++) {
                if (subs[p].contestId != e->contestId) continue;
                for (int k = 0; k < e->problemCount; k++) {
                    if (strcmp(subs[p].index, e->labels[k]) == 0) { e->status[k] = (subs[p].time <= end) ? 1 : 2; break; }
                }
            }
        }
        free(subs);
    }

    printf("\n=== 生成报告 ===\n");
    FILE *fp = fopen("index.html", "w");
    if (!fp) { printf("无法写入 index.html\n"); return 1; }

    fprintf(fp, "<html><head><meta charset='utf-8'><title>Codeforces 用户列表</title><style>"
        "*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#f3f4f6;color:#1f2937;padding:40px 20px}"
        ".container{max-width:1200px;margin:0 auto}"
        ".card{background:#ffffff;border-radius:16px;padding:32px;margin-bottom:24px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1);border:1px solid #e5e7eb}"
        "h1{font-size:32px;font-weight:700;margin-bottom:8px;color:#111827}table{width:100%%;border-collapse:separate;border-spacing:0;font-size:15px}"
        "th{background:#f9fafb;padding:16px;text-align:left;font-weight:600;color:#374151;border-bottom:2px solid #e5e7eb;text-transform:uppercase;font-size:12px;letter-spacing:.05em}"
        "td{padding:16px;border-bottom:1px solid #e5e7eb;vertical-align:middle}tr:last-child td{border-bottom:none}tr:hover td{background:#f9fafb}"
        "td a{color:#2563eb;text-decoration:none;font-weight:600}td a:hover{color:#1d4ed8;text-decoration:underline}</style></head><body>"
        "<div class='container'><div class='card'><h1>Codeforces 用户列表</h1><p style='color:#6b7280;font-size:15px'>共 %d 个用户 | 数据来源: Codeforces API</p></div>"
        "<div class='card' style='overflow-x:auto'><table><thead><tr>"
        "<th>头像</th><th>用户</th><th>当前 Rating</th><th>最高 Rating</th><th>头衔</th><th>参赛次数</th><th>近180天参赛</th><th>近180天最高</th>"
        "</tr></thead><tbody>", nusers);

    for (int i = 0; i < nusers; i++) {
        UserInfo *u = &users[i];
        fprintf(fp, "<tr><td><img src='%s' width='40' height='40' style='border-radius:50%%;border:2px solid %s;object-fit:cover'></td>"
            "<td><a href='%s_report.html' style='color:%s;font-size:16px'>%s</a></td>"
            "<td style='color:%s;font-weight:600'>%d</td><td style='color:%s;font-weight:600'>%d</td><td style='color:%s;font-weight:600'>%s %s</td><td>%d</td><td>%d</td><td style='color:%s;font-weight:600'>%d</td></tr>\n",
            u->avatar, rcol(u->curRating),
            u->handle, rcol(u->curRating), u->handle,
            rcol(u->curRating), u->curRating, rcol(u->maxRating), u->maxRating,
            rcol(u->curRating), u->title, translate_title(u->title), u->contestCount, u->cnt180, rcol(u->max180), u->max180);
    }
    fprintf(fp, "</tbody></table></div></div></body></html>"); fclose(fp);
    printf("index.html 已生成\n");

    int ranges[] = {0, 800, 1000, 1200, 1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3500};
    int nbins = sizeof(ranges)/sizeof(int) - 1;
    char *range_labels[] = {"<800", "800-999", "1000-1199", "1200-1399", "1400-1599", "1600-1799", "1800-1999", "2000-2199", "2200-2399", "2400-2599", "2600-2799", "2800-2999", "3000+"};

    for (int ui = 0; ui < nusers; ui++) {
        UserInfo *u = &users[ui];
        Entry *eu = entries[ui];
        int n = nentries[ui];
        char fn[128]; snprintf(fn, 128, "%s_report.html", u->handle);
        fp = fopen(fn, "w"); if (!fp) continue;

        SolvedProb *su = solved[ui];
        int ns = nsolved[ui];
        for (int i = 0; i < ns; i++) {
            int cid = 0; char idx[8] = ""; sscanf(su[i].problemId, "%d_%s", &cid, idx);
            for (int k = 0; k < pmap_n; k++) {
                char pc[32]; snprintf(pc, 32, "%d_%s", pmap_ids[k], idx);
                if (strcmp(su[i].problemId, pc) == 0) { su[i].rating = pmap_ratings[k]; break; }
            }
        }

        int64_t now = time(NULL), cy = now - 365*86400LL, c180 = now - 180*86400LL, cm = now - 30*86400LL;
        int ba[20]={0}, by[20]={0}, b180[20]={0}, bm[20]={0};
        for (int i = 0; i < ns; i++) {
            if (!su[i].rating) continue;
            for (int b = 0; b < nbins; b++) {
                if (su[i].rating >= ranges[b] && su[i].rating < ranges[b+1]) {
                    ba[b]++; if (su[i].time >= cy) by[b]++; if (su[i].time >= c180) b180[b]++; if (su[i].time >= cm) bm[b]++; break;
                }
            }
        }

        fprintf(fp, "<html><head><meta charset='utf-8'><title>%s - Codeforces Data</title><script src='https://cdn.jsdelivr.net/npm/echarts'></script><style>"
            "*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#f3f4f6;color:#1f2937;padding:40px 20px}"
            ".container{max-width:1200px;margin:0 auto}"
            ".card{background:#ffffff;border-radius:16px;padding:32px;margin-bottom:24px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1);border:1px solid #e5e7eb}"
            "h1{font-size:32px;font-weight:700;margin-bottom:8px}h2{font-size:20px;font-weight:600;color:#374151;margin-bottom:20px}"
            ".stats{display:grid;grid-template-columns:repeat(auto-fit,minmax(180px,1fr));gap:20px;margin-top:24px}"
            ".stat{background:#f8fafc;padding:24px;border-radius:12px;text-align:center;border:1px solid #e2e8f0;transition:transform 0.2s,box-shadow 0.2s}"
            ".stat:hover{transform:translateY(-2px);box-shadow:0 10px 15px -3px rgba(0,0,0,.1)}"
            ".stat .val{font-size:28px;font-weight:700;margin-bottom:4px}.stat .lbl{font-size:13px;color:#64748b;font-weight:500;text-transform:uppercase;letter-spacing:.05em}"
            "table{width:100%%;border-collapse:separate;border-spacing:0;font-size:14px}"
            "th{background:#f9fafb;padding:14px;text-align:left;font-weight:600;position:sticky;top:0;color:#374151;border-bottom:2px solid #e5e7eb}"
            "td{padding:12px;border-bottom:1px solid #e5e7eb;vertical-align:middle}tr:last-child td{border-bottom:none}tr:hover td{background:#f9fafb}"
            ".wrap{display:inline-block;padding:4px 10px;border-radius:6px;font-weight:600;font-size:13px}"
            ".green{background:#dcfce7;color:#166534}.red{background:#fee2e2;color:#991b1b}.gray{background:#f3f4f6;color:#4b5563}"
            ".prob{padding:4px;border-radius:4px;font-size:12px;margin:2px;display:inline-flex;flex-direction:column;align-items:center;min-width:44px;font-weight:500;border:1px solid transparent;line-height:1.2;gap:2px}"
            ".prob.ok{background:#dcfce7;color:#166534;border-color:#bbf7d0}.prob.after{background:#fef3c7;color:#92400e;border-color:#fde68a}.prob.no{background:#f3f4f6;color:#4b5563;border-color:#e5e7eb}"
            "td a{color:#2563eb;text-decoration:none;font-weight:500}td a:hover{color:#1d4ed8;text-decoration:underline}#chart,#hist{width:100%%;height:400px}"
            ".btn-group{display:flex;gap:8px;margin-bottom:16px;flex-wrap:wrap}"
            ".btn{padding:8px 16px;border:none;border-radius:6px;font-size:14px;font-weight:500;cursor:pointer;transition:all 0.2s}"
            ".btn-active{background:#2563eb;color:#fff;box-shadow:0 2px 4px rgba(37,99,235,.2)}.btn-idle{background:#f1f5f9;color:#475569;border:1px solid #e2e8f0}.btn-idle:hover{background:#e2e8f0}"
            "</style></head><body><div class='container'>", u->handle);

        fprintf(fp, "<div class='card'><div style='display:flex;align-items:center;gap:20px;margin-bottom:12px'>"
            "<img src='%s' width='80' height='80' style='border-radius:50%%;border:3px solid %s;object-fit:cover'>"
            "<div><h1 style='color:%s;margin:0'>%s</h1><p style='color:%s;font-size:16px;margin-top:4px;font-weight:600'>%s %s</p></div></div><div class='stats'>"
            "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>当前 Rating</div></div>"
            "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>最高 Rating</div></div>"
            "<div class='stat'><div class='val'>%d</div><div class='lbl'>参赛次数</div></div>"
            "<div class='stat'><div class='val'>%d</div><div class='lbl'>近180天参赛</div></div>"
            "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>近180天最高</div></div>"
            "<div class='stat'><div class='val'>%d</div><div class='lbl'>通过题目数</div></div></div></div>",
            u->avatar, rcol(u->curRating), rcol(u->curRating), u->handle, rcol(u->curRating), u->title, translate_title(u->title), rcol(u->curRating), u->curRating, rcol(u->maxRating), u->maxRating,
            u->contestCount, u->cnt180, rcol(u->max180), u->max180, ns);

        fprintf(fp, "<div class='card'><h2>Rating 变化</h2><div id='chart'></div></div><script>"
            "var chart=echarts.init(document.getElementById('chart'));chart.setOption({backgroundColor:'transparent',"
            "tooltip:{trigger:'axis'},grid:{left:60,right:20,bottom:30,top:20},"
            "xAxis:{type:'category',data:[");
        for (int i = 0; i < n; i++) {
            char tb[64]; fmt_time(tb, eu[i].startTime);
            fprintf(fp, "'%s',", tb);
        }
        fprintf(fp, "],axisLabel:{color:'#6b7280'},splitLine:{show:false}},yAxis:{type:'value',axisLabel:{color:'#6b7280'},splitLine:{color:'#f3f4f6'}},"
            "series:[{type:'line',smooth:true,data:[");
        for (int i = 0; i < n; i++) fprintf(fp, "%d,", eu[i].newRating);
        fprintf(fp, "],lineStyle:{width:3},symbol:'circle',symbolSize:6,itemStyle:{color:'#2563eb'},areaStyle:{color:{type:'linear',x:0,y:0,x2:0,y2:1,"
            "colorStops:[{offset:0,color:'rgba(37,99,235,.2)'},{offset:1,color:'rgba(37,99,235,0)'}]}}}]});</script>");

        fprintf(fp, "<div class='card'><h2>通过题目难度分布</h2><div class='btn-group'>"
            "<button id='btn0' class='btn btn-active' onclick='switchHist(0)'>全部</button>"
            "<button id='btn1' class='btn btn-idle' onclick='switchHist(1)'>最近一年</button>"
            "<button id='btn2' class='btn btn-idle' onclick='switchHist(2)'>最近180天</button>"
            "<button id='btn3' class='btn btn-idle' onclick='switchHist(3)'>最近1个月</button>"
            "</div><div id='hist'></div></div><script>var histData = {all:[");
        for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", ba[i]);
        fprintf(fp, "],year:["); for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", by[i]);
        fprintf(fp, "],d180:["); for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", b180[i]);
        fprintf(fp, "],month:["); for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", bm[i]);
        fprintf(fp, "],labels:["); for (int i = 0; i < nbins; i++) fprintf(fp, "'%s',", range_labels[i]);
        fprintf(fp, "],current:'all'};function switchHist(p){"
            "for(var i=0;i<4;i++){var b=document.getElementById('btn'+i);"
            "b.className=i===p?'btn btn-active':'btn btn-idle';}"
            "var k=['all','year','d180','month'][p];"
            "var h=echarts.init(document.getElementById('hist'));h.setOption({backgroundColor:'transparent',"
            "tooltip:{trigger:'axis'},grid:{left:60,right:20,bottom:40,top:20},"
            "xAxis:{type:'category',data:histData.labels,axisLabel:{color:'#6b7280',rotate:30},splitLine:{show:false}},"
            "yAxis:{type:'value',axisLabel:{color:'#6b7280'},splitLine:{color:'#f3f4f6'}},"
            "series:[{type:'bar',data:histData[k],itemStyle:{color:'#3b82f6',borderRadius:[4,4,0,0]}}]});}switchHist(0);</script>");

        fprintf(fp, "<div class='card' style='overflow-x:auto'><h2>比赛记录</h2><table><thead><tr>"
            "<th>#</th><th>比赛</th><th>日期</th><th>排名</th><th>赛前</th><th>赛后</th><th>变化</th><th>题目</th>"
            "</tr></thead><tbody>");
        for (int i = n - 1; i >= 0; i--) {
            int d = eu[i].newRating - eu[i].oldRating; char tb[64]; fmt_time(tb, eu[i].startTime);
            fprintf(fp, "<tr><td>%d</td><td><a href='https://codeforces.com/contest/%d' target='_blank'>%s</a></td>"
                "<td>%s</td><td>%d</td><td style='color:%s;font-weight:600'>%d</td><td style='color:%s;font-weight:600'>%d</td>"
                "<td><span class='wrap %s'>%+d</span></td><td style='display:flex;flex-wrap:wrap;gap:4px'>",
                n - i, eu[i].contestId, eu[i].contestName, tb, eu[i].rank,
                rcol(eu[i].oldRating), eu[i].oldRating, rcol(eu[i].newRating), eu[i].newRating,
                d > 0 ? "green" : d < 0 ? "red" : "gray", d);
            for (int p = 0; p < eu[i].problemCount; p++) {
                const char *cls = eu[i].status[p]==1?"ok":eu[i].status[p]==2?"after":"no";
                fprintf(fp, "<div class='prob %s' title='%s'><div style='font-weight:700'>%s</div></div>",
                    cls, eu[i].labels[p], eu[i].labels[p]);
            }
            fprintf(fp, "</td></tr>\n");
        }
        fprintf(fp, "</tbody></table></div></div></body></html>"); fclose(fp);
        printf("  %s 已生成\n", fn);
    }

    /* If there were missing handles, generate a small report notifying which were not found */
    if (nmissing > 0) {
        fp = fopen("missing_users.html", "w"); if (fp) {
            fprintf(fp, "<html><head><meta charset='utf-8'><title>Missing Users</title><style>"
                        "*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#f3f4f6;color:#1f2937;padding:40px}"
                        ".card{max-width:600px;margin:0 auto;background:#fff;padding:32px;border-radius:16px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1);border:1px solid #e5e7eb}"
                        "h1{font-size:24px;margin-bottom:16px;color:#ef4444}ul{list-style-type:disc;padding-left:24px}li{margin-bottom:8px;font-size:16px;color:#4b5563}"
                        "</style></head><body><div class='card'>");
            fprintf(fp, "<h1>以下用户未找到</h1><ul>");
            for (int i = 0; i < nmissing; i++) fprintf(fp, "<li>%s</li>", missing_handles[i]);
            fprintf(fp, "</ul></div></body></html>"); fclose(fp);
            printf("missing_users.html 已生成\n");
        }
    }

    /* Open index.html in default browser (use cmd start to avoid depending on shellapi header) */
    {
        char _cmd[256];
        if (nusers == 1) {
            snprintf(_cmd, sizeof(_cmd), "start \"\" \"%s_report.html\"", users[0].handle);
        } else {
            snprintf(_cmd, sizeof(_cmd), "start \"\" \"%s\"", "index.html");
        }
        system(_cmd);
    }

    for (int i = 0; i < nusers; i++) { free(entries[i]); free(solved[i]); }
    free(pmap_ids); free(pmap_ratings); free(pmap_indices);
    curl_global_cleanup();
    if (dbg_fp) { fprintf(dbg_fp, "cf_tool exiting\n"); fclose(dbg_fp); dbg_fp = NULL; }
    printf("\n完成！打开 index.html 查看用户列表\n");
    return 0;
}
