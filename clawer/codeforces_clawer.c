#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <time.h>
#include <windows.h>
#include <curl/curl.h>
#include "cJSON.h"

#define MAX_HANDLES     100
#define MAX_CONTESTS    500
#define MAX_PROBS        15
#define MAX_SUBMISSIONS 20000
#define MAX_SOLVED      20000

// 存储单个用户的基本信息：用户名、头像、头衔及参赛统计
typedef struct { char handle[64], avatar[256], title[64]; int curRating, maxRating, contestCount, cnt180, max180; } UserInfo;
// 存储用户单场比赛的记录：各项数据、题目列表及各题状态
typedef struct { int contestId, durationSeconds, oldRating, newRating, rank, problemCount; int64_t startTime; char contestName[256], labels[MAX_PROBS][4]; int status[MAX_PROBS]; } Entry;
// 存储单次提交记录的关键信息：比赛ID、提交时间和题目编号
typedef struct { int contestId; int64_t time; char index[4]; } Sub;
// 存储已解决题目的信息：拼接后的唯一题目ID、难度分及通过时间
typedef struct { char problemId[32]; int rating; int64_t time; } SolvedProb;
// 用于 cURL 动态内存分配的缓冲区结构
typedef struct { char *buf; size_t len; } Buf;

// libcurl 的写回调函数
static size_t write_cb(void *p, size_t sz, size_t n, void *u) {
    Buf *b = u; size_t r = sz * n;
    char *q = realloc(b->buf, b->len + r + 1); if (!q) return 0;
    memcpy(q + b->len, p, r); b->len += r; q[b->len] = 0; b->buf = q; return r;
}

// 封装的 HTTP GET 函数：拉取指定URL内容并返回分配的字符串，失败返回 NULL
static char *http_get(const char *url) {
    CURL *c = curl_easy_init(); if (!c) return NULL;
    Buf b = {NULL, 0};
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &b);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "cf_tool/1.0");
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    CURLcode rc = curl_easy_perform(c);
    long code = 0; curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_easy_cleanup(c);
    if (rc != CURLE_OK || code != 200) { free(b.buf); return NULL; }
    return b.buf;
}

// 封装的 Codeforces API 调用
static char *cf_api(const char *fmt, ...) {
    char url[512]; va_list ap; va_start(ap, fmt); vsnprintf(url, 512, fmt, ap); va_end(ap);
    return http_get(url);
}

// 获取分数对应的颜色代码（用于HTML渲染和等级区分）
static const char *rcol(int r) {
    if (r >= 2400) return "#FF0000"; if (r >= 2100) return "#FF8C00";
    if (r >= 1900) return "#AA00AA"; if (r >= 1600) return "#0000FF";
    if (r >= 1400) return "#03A89E"; if (r >= 1200) return "#008000";
    return "#808080";
}

// 在控制台输出带颜色的文本段落
static void print_colored(const char *text, WORD color) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO info;
    WORD old = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    if (h != INVALID_HANDLE_VALUE && GetConsoleScreenBufferInfo(h, &info)) old = info.wAttributes;
    if (h != INVALID_HANDLE_VALUE) SetConsoleTextAttribute(h, color);
    fputs(text, stdout);
    if (h != INVALID_HANDLE_VALUE) SetConsoleTextAttribute(h, old);
}

// 安全的字符串复制函数：防止越界，并确保总以 '\0' 结尾
static void cstr(char *dst, const char *src, size_t n) {
    if (!dst || !n) return;
    if (!src) { dst[0] = 0; return; }
    strncpy(dst, src, n - 1); dst[n - 1] = 0;
}

// 将秒级时间戳格式化为可读字符串
static void fmt_time(char *buf, int64_t t) {
    time_t tt = (time_t)t; struct tm tmv;
    localtime_s(&tmv, &tt); strftime(buf, 64, "%Y-%m-%d %H:%M", &tmv);
}

// 解析以空格隔开的用户名字符串到 handles 数组中
static void parse_handles(char handles[][64], int *n, const char *src) {
    char buf[4096]; cstr(buf, src, sizeof(buf));
    for (char *tok = strtok(buf, " "); tok && *n < MAX_HANDLES; tok = strtok(NULL, " ")) {
        cstr(handles[*n], tok, 64); if (handles[*n][0]) (*n)++;
    }
}

// 从 cJSON 对象中提取字符串类型字段
static const char *jstr(cJSON *o, const char *k) {
    cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return (it && cJSON_IsString(it) && it->valuestring) ? it->valuestring : "";
}

// 从 cJSON 对象中提取数值类型字段
static int jnum(cJSON *o, const char *k) {
    cJSON *it = o ? cJSON_GetObjectItemCaseSensitive(o, k) : NULL;
    return (it && cJSON_IsNumber(it)) ? it->valueint : 0;
}

// 将 Codeforces 常用英文头衔翻译为中文词汇
static const char *translate_title(const char *title) {
    if (!title || !title[0]) return "";
    char t[64]; cstr(t, title, sizeof(t));
    for (int i = 0; t[i]; i++) if (t[i] >= 'A' && t[i] <= 'Z') t[i] += 32;
    const char *en[] = {"world top", "legendary grandmaster", "international grandmaster", "grandmaster", "international master", "candidate master", "master", "expert", "specialist", "pupil", "newbie"};
    const char *zh[] = {"世界顶尖", "传奇特级大师", "国际特级大师", "特级大师", "国际大师", "候选大师", "大师", "行家", "专家", "学徒", "新手"};
    for (int i = 0; i < 11; i++) if (strstr(t, en[i])) return zh[i];
    return "";
}

static int fill_user(UserInfo *p, cJSON *u) {
    const char *h = jstr(u, "handle"); if (!h[0]) return 0;
    memset(p, 0, sizeof(*p));
    cstr(p->handle, h, sizeof(p->handle));
    cstr(p->avatar, jstr(u, "titlePhoto"), sizeof(p->avatar));
    if (!p->avatar[0]) cstr(p->avatar, jstr(u, "avatar"), sizeof(p->avatar));
    p->curRating = jnum(u, "rating"); p->maxRating = jnum(u, "maxRating");
    cstr(p->title, jstr(u, "rank"), sizeof(p->title));
    return 1;
}

static int parse_user_array(const char *json, UserInfo *users, int max) {
    if (!json) return 0;
    cJSON *root = cJSON_Parse(json); if (!root) return 0;
    int added = 0;
    cJSON *st  = cJSON_GetObjectItemCaseSensitive(root, "status");
    cJSON *res = cJSON_GetObjectItemCaseSensitive(root, "result");
    if (st && cJSON_IsString(st) && strcmp(st->valuestring, "OK") == 0 && res && cJSON_IsArray(res)) {
        int cnt = cJSON_GetArraySize(res); if (cnt > max) cnt = max;
        for (int i = 0; i < cnt; i++) { cJSON *u = cJSON_GetArrayItem(res, i); if (u && fill_user(&users[added], u)) added++; }
    }
    cJSON_Delete(root); return added;
}

static int add_label(char labels[MAX_PROBS][4], int count, const char *label) {
    if (!label || !label[0] || count >= MAX_PROBS) return count;
    for (int i = 0; i < count; i++) if (strcmp(labels[i], label) == 0) return count;
    cstr(labels[count], label, sizeof(labels[count])); return count + 1;
}

static void sort_labels(char labels[MAX_PROBS][4], int count) {
    for (int i = 0; i < count - 1; i++)
        for (int j = i + 1; j < count; j++)
            if (strcmp(labels[i], labels[j]) > 0) {
                char tmp[4]; memcpy(tmp, labels[i], 4); memcpy(labels[i], labels[j], 4); memcpy(labels[j], tmp, 4);
            }
}


int main(int argc, char **argv) {
    // === 初始化：设定控制台编码UTF-8，以及全局初始化cURL ===
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Codeforces 用户查询");
    curl_global_init(CURL_GLOBAL_ALL);

    char handles[MAX_HANDLES][64]; int nhandles = 0;

    // === 获取与解析输入的用户名 ===
    if (argc > 1) {
        for (int i = 1; i < argc && nhandles < MAX_HANDLES; i++) { cstr(handles[nhandles], argv[i], 64); if (handles[nhandles][0]) nhandles++; }
        printf("已读取到 %d 个用户\n", nhandles);
    } else {
        print_colored("═══════════════════════════════════════════\n", FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        print_colored("          Codeforces 用户信息查询        \n", FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        print_colored("═══════════════════════════════════════════\n", FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        print_colored("支持多用户查询，用户名之间使用空格间隔\n",   FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        print_colored("最多可同时查询100名用户\n",                  FOREGROUND_RED);
        print_colored("用户名不区分大小写\n",                       FOREGROUND_BLUE | FOREGROUND_INTENSITY);
        print_colored("请输入 Codeforces 用户名：\n\n",             FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        char buf[4096];
        if (!fgets(buf, sizeof(buf), stdin)) return 1;
        buf[strcspn(buf, "\n")] = 0;
        parse_handles(handles, &nhandles, buf);
    }
    if (!nhandles) { printf("未识别到有效的用户名\n"); return 1; }

    printf("\n=== 获取用户基本信息 ===\n");
    // === 请求 user.info 取用户基本数据 ===
    char handle_list[4096] = "";
    for (int i = 0; i < nhandles; i++) { if (i) strcat(handle_list, ";"); strcat(handle_list, handles[i]); }

    UserInfo users[MAX_HANDLES]; int nusers = 0;
    char *s = NULL;

    // 首先尝试拼接所有用户名进行一次批量请求
    for (int i = 0; i < 3 && !s; i++) { s = cf_api("https://codeforces.com/api/user.info?handles=%s", handle_list); if (!s) Sleep(1000 * (i + 1)); }
    nusers = parse_user_array(s, users, nhandles);
    free(s); s = NULL;

    // 批量失败或结果不完整时，进行逐一请求
    if (!nusers) {
        for (int hi = 0; hi < nhandles; hi++) {
            for (int i = 0; i < 3 && !s; i++) { s = cf_api("https://codeforces.com/api/user.info?handles=%s", handles[hi]); if (!s) Sleep(500); }
            nusers += parse_user_array(s, users + nusers, 1);
            free(s); s = NULL;
        }
    }

    char missing[MAX_HANDLES][64]; int nmissing = 0;
    for (int hi = 0; hi < nhandles; hi++) {
        int found = 0;
        for (int ui = 0; ui < nusers; ui++) if (_stricmp(handles[hi], users[ui].handle) == 0) { found = 1; break; }
        if (!found) { printf("未找到用户: %s\n", handles[hi]); cstr(missing[nmissing++], handles[hi], 64); }
    }
    if (!nusers) { printf("未找到任何用户，请检查用户名\n"); return 1; }

    printf("\n=== 获取题目信息 ===\n");
    // === 拉取 problemset.problems，建立题目对应其难度分数的映射 ===
    int *pmap_ids = NULL, *pmap_ratings = NULL, pmap_n = 0;
    char (*pmap_indices)[4] = NULL;
    s = cf_api("https://codeforces.com/api/problemset.problems");
    if (s) {
        cJSON *ps = cJSON_Parse(s); free(s); s = NULL;
        if (ps) {
            cJSON *res = cJSON_GetObjectItemCaseSensitive(ps, "result");
            cJSON *probs = res ? cJSON_GetObjectItemCaseSensitive(res, "problems") : NULL;
            if (probs && cJSON_IsArray(probs)) {
                int sz = cJSON_GetArraySize(probs);
                pmap_ids = malloc(sizeof(int) * sz); pmap_ratings = malloc(sizeof(int) * sz); pmap_indices = malloc(sizeof(char[4]) * sz);
                for (int i = 0; i < sz; i++) {
                    cJSON *p = cJSON_GetArrayItem(probs, i);
                    int cid = jnum(p, "contestId"); const char *idx = jstr(p, "index");
                    if (cid && idx[0]) { pmap_ids[pmap_n] = cid; pmap_ratings[pmap_n] = jnum(p, "rating"); cstr(pmap_indices[pmap_n], idx, 4); pmap_n++; }
                }
            }
            cJSON_Delete(ps);
        }
    }

    // === 拉取 contest.list，映射每场比赛的时长与开始时间属性 ===
    cJSON *cl_root = NULL, *cl_arr = NULL; int ncl = 0;
    s = cf_api("https://codeforces.com/api/contest.list?gym=false");
    if (s) {
        cl_root = cJSON_Parse(s); free(s); s = NULL;
        if (cl_root) { cJSON *res = cJSON_GetObjectItemCaseSensitive(cl_root, "result"); if (res && cJSON_IsArray(res)) { cl_arr = res; ncl = cJSON_GetArraySize(res); } }
    }

    printf("\n=== 获取用户详细数据 ===\n");
    // === 按用户分别拉取 rating 记录与状态提交记录 ===
    Entry *entries[MAX_HANDLES]; int nentries[MAX_HANDLES];
    SolvedProb *solved[MAX_HANDLES]; int nsolved[MAX_HANDLES];

    for (int ui = 0; ui < nusers; ui++) {
        UserInfo *u = &users[ui];
        printf("[%d/%d] %s\n", ui + 1, nusers, u->handle);

        s = cf_api("https://codeforces.com/api/user.rating?handle=%s", u->handle);
        cJSON *rt = s ? cJSON_Parse(s) : NULL; free(s); s = NULL;
        cJSON *rt_arr = NULL; int n = 0;
        if (rt) { cJSON *res = cJSON_GetObjectItemCaseSensitive(rt, "result"); if (res && cJSON_IsArray(res)) { rt_arr = res; n = cJSON_GetArraySize(res); if (n > MAX_CONTESTS) n = MAX_CONTESTS; } }

        s = cf_api("https://codeforces.com/api/user.status?handle=%s&from=1&count=%d", u->handle, MAX_SUBMISSIONS);
        cJSON *subj = s ? cJSON_Parse(s) : NULL; free(s); s = NULL;
        cJSON *sub_arr = NULL; int nsub = 0;
        if (subj) { cJSON *res = cJSON_GetObjectItemCaseSensitive(subj, "result"); if (res && cJSON_IsArray(res)) { sub_arr = res; nsub = cJSON_GetArraySize(res); } }

        // 解析并筛选出 verdict 为 "OK(通过)" 的提交；
        // 同时构建：subs[]（用于判断某题是在赛内还是赛后过题）和 solved[]（用于统计各题难度分和图表去重）
        Sub *subs = malloc(sizeof(Sub) * (nsub > 0 ? nsub : 1)); int sub_cnt = 0;
        solved[ui] = calloc(MAX_SOLVED, sizeof(SolvedProb)); nsolved[ui] = 0;
        for (int i = 0; i < nsub; i++) {
            cJSON *x = cJSON_GetArrayItem(sub_arr, i);
            if (!x || strcmp(jstr(x, "verdict"), "OK") != 0) continue;
            int cid = jnum(x, "contestId");
            cJSON *pr = cJSON_GetObjectItemCaseSensitive(x, "problem");
            const char *ix = pr ? jstr(pr, "index") : "";
            cJSON *ct = cJSON_GetObjectItemCaseSensitive(x, "creationTimeSeconds");
            int64_t tm = (ct && cJSON_IsNumber(ct)) ? (int64_t)ct->valuedouble : 0;
            if (!cid || !ix[0]) continue;
            if (tm) { subs[sub_cnt].contestId = cid; subs[sub_cnt].time = tm; cstr(subs[sub_cnt].index, ix, 4); sub_cnt++; }
            char pid[32]; snprintf(pid, 32, "%d_%s", cid, ix);
            int dup = 0; for (int j = 0; j < nsolved[ui]; j++) if (strcmp(solved[ui][j].problemId, pid) == 0) { dup = 1; break; }
            if (!dup && nsolved[ui] < MAX_SOLVED) { cstr(solved[ui][nsolved[ui]].problemId, pid, 32); solved[ui][nsolved[ui]].time = tm; nsolved[ui]++; }
        }

        entries[ui] = calloc(n > 0 ? n : 1, sizeof(Entry)); nentries[ui] = n;
        int64_t now = time(NULL);
        u->contestCount = n; u->cnt180 = 0; u->max180 = 0;

        // 生成该用户参加过的比赛的 Entry 数组数据
        for (int i = 0; i < n; i++) {
            cJSON *x = rt_arr ? cJSON_GetArrayItem(rt_arr, i) : NULL; if (!x) continue;
            Entry *e = &entries[ui][i]; memset(e, 0, sizeof(*e));
            int cid = jnum(x, "contestId");
            e->contestId = cid; e->oldRating = jnum(x, "oldRating"); e->newRating = jnum(x, "newRating"); e->rank = jnum(x, "rank");
            cstr(e->contestName, jstr(x, "contestName"), sizeof(e->contestName));
            cJSON *rt_time = cJSON_GetObjectItemCaseSensitive(x, "ratingUpdateTimeSeconds");
            e->startTime = (rt_time && cJSON_IsNumber(rt_time)) ? (int64_t)rt_time->valuedouble : 0;
            e->durationSeconds = 7200;

            for (int k = 0; k < ncl; k++) {
                cJSON *cl = cJSON_GetArrayItem(cl_arr, k); if (!cl || jnum(cl, "id") != cid) continue;
                cJSON *dur = cJSON_GetObjectItemCaseSensitive(cl, "durationSeconds");
                cJSON *stm = cJSON_GetObjectItemCaseSensitive(cl, "startTimeSeconds");
                if (dur && cJSON_IsNumber(dur)) e->durationSeconds = dur->valueint;
                if (stm && cJSON_IsNumber(stm)) e->startTime = (int64_t)stm->valuedouble;
                break;
            }

            for (int k = 0; k < pmap_n && e->problemCount < MAX_PROBS; k++)
                if (pmap_ids[k] == cid) e->problemCount = add_label(e->labels, e->problemCount, pmap_indices[k]);
            for (int p = 0; p < sub_cnt && e->problemCount < MAX_PROBS; p++)
                if (subs[p].contestId == cid) e->problemCount = add_label(e->labels, e->problemCount, subs[p].index);
            sort_labels(e->labels, e->problemCount);

            if (e->startTime >= now - 180 * 86400LL) { u->cnt180++; if (e->newRating > u->max180) u->max180 = e->newRating; }
            printf("  %d/%d\r", i + 1, n); fflush(stdout);
        }
        printf("\n");

        // 汇总赛内/赛后补题状态（根据比赛结束时间区分 1=赛中, 2=赛后）
        for (int i = 0; i < n; i++) {
            Entry *e = &entries[ui][i]; int64_t end = e->startTime + e->durationSeconds;
            for (int p = 0; p < sub_cnt; p++) {
                if (subs[p].contestId != e->contestId) continue;
                for (int k = 0; k < e->problemCount; k++)
                    if (strcmp(subs[p].index, e->labels[k]) == 0) { e->status[k] = (subs[p].time <= end) ? 1 : 2; break; }
            }
        }
        free(subs);

        if (rt)   cJSON_Delete(rt);
        if (subj) cJSON_Delete(subj);
    }

    printf("\n=== 生成报告 ===\n");
    // === 生成用户列表主页 index.html ===
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
                "<div class='container'><div class='card'><h1>Codeforces 用户列表</h1><p style='color:#6b7280;font-size:15px'>共 %d 个用户 | 数据来源: Codeforces API</p><p style='color:#6b7280;font-size:15px'>点击用户名查看详情</p></div>"
                "<div class='card' style='overflow-x:auto'><table><thead><tr>"
                "<th>头像</th><th>用户</th><th>当前等级分</th><th>最高等级分</th><th>头衔</th><th>参赛次数</th><th>近180天参赛</th><th>近180天最高</th>"
                "</tr></thead><tbody>", nusers);

    for (int i = 0; i < nusers; i++) {
        UserInfo *u = &users[i];
        fprintf(fp, "<tr><td><img src='%s' width='40' height='40' style='border-radius:50%%;border:2px solid %s;object-fit:cover'></td>"
                    "<td><a href='%s_report.html' style='color:%s;font-size:16px'>%s</a></td>"
                    "<td style='color:%s;font-weight:600'>%d</td><td style='color:%s;font-weight:600'>%d</td>"
                    "<td style='color:%s;font-weight:600'>%s %s</td><td>%d</td><td>%d</td><td style='color:%s;font-weight:600'>%d</td></tr>\n",
                u->avatar, rcol(u->curRating), u->handle, rcol(u->curRating), u->handle,
                rcol(u->curRating), u->curRating, rcol(u->maxRating), u->maxRating,
                rcol(u->curRating), u->title, translate_title(u->title), u->contestCount, u->cnt180, rcol(u->max180), u->max180);
    }
    fprintf(fp, "</tbody></table></div></div></body></html>"); fclose(fp);
    printf("index.html 已生成\n");

    // 取值分布：将题目难度划分为特定区间用于柱状图展示
    int ranges[] = {0, 800, 1000, 1200, 1400, 1600, 1800, 2000, 2200, 2400, 2600, 2800, 3000, 3500};
    int nbins = sizeof(ranges)/sizeof(int) - 1;
    char *range_labels[] = {"<800","800-999","1000-1199","1200-1399","1400-1599","1600-1799","1800-1999","2000-2199","2200-2399","2400-2599","2600-2799","2800-2999","3000+"};

    // === 为每位用户单独生成对应的数据报告 HTML (用 ECharts) ===
    for (int ui = 0; ui < nusers; ui++) {
        UserInfo *u = &users[ui]; Entry *eu = entries[ui]; int n = nentries[ui];
        char fn[128]; snprintf(fn, 128, "%s_report.html", u->handle);
        fp = fopen(fn, "w"); if (!fp) continue;

        // 匹配已解决题目的难度分
        SolvedProb *su = solved[ui]; int ns = nsolved[ui];
        for (int i = 0; i < ns; i++) {
            char *us = strchr(su[i].problemId, '_'); if (!us) continue;
            int cid = (int)strtol(su[i].problemId, NULL, 10); const char *idx = us + 1;
            for (int k = 0; k < pmap_n; k++)
                if (pmap_ids[k] == cid && strcmp(pmap_indices[k], idx) == 0) { su[i].rating = pmap_ratings[k]; break; }
        }

        // 根据时间区间统计不同难度区间内的过题数量
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

        // 输出单用户报告的头部基础结构和统计面板数据
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

        fprintf(fp, "<div class='card' style='display:flex;flex-wrap:wrap;gap:40px;align-items:center;'><div style='display:flex;flex-direction:column;align-items:center;gap:12px;min-width:160px;'>"
                    "<img src='%s' width='120' height='120' style='border-radius:50%%;border:3px solid %s;object-fit:cover'>"
                    "<div style='text-align:center;'><h1 style='color:%s;margin:0;font-size:28px;'>%s</h1><p style='color:%s;font-size:16px;margin-top:4px;font-weight:600'>%s %s</p></div></div>"
                    "<div class='stats' style='flex:1;margin-top:0;grid-template-columns:repeat(3,1fr);min-width:400px;gap:16px;'>"
                    "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>当前等级分</div></div>"
                    "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>最高等级分</div></div>"
                    "<div class='stat'><div class='val'>%d</div><div class='lbl'>参赛次数</div></div>"
                    "<div class='stat'><div class='val'>%d</div><div class='lbl'>近180天参赛</div></div>"
                    "<div class='stat'><div class='val' style='color:%s'>%d</div><div class='lbl'>近180天最高</div></div>"
                    "<div class='stat'><div class='val'>%d</div><div class='lbl'>通过题目数</div></div></div></div>",
                u->avatar, rcol(u->curRating), rcol(u->curRating), u->handle, rcol(u->curRating), u->title, translate_title(u->title),
                rcol(u->curRating), u->curRating, rcol(u->maxRating), u->maxRating, u->contestCount, u->cnt180, rcol(u->max180), u->max180, ns);

        // 使用 ECharts 绘制用户等级分变化的折线图
        fprintf(fp, "<div class='card'><h2>等级分变化</h2><div id='chart'></div></div><script>"
                    "var chart=echarts.init(document.getElementById('chart'));chart.setOption({backgroundColor:'transparent',"
                    "tooltip:{trigger:'axis'},grid:{left:60,right:20,bottom:30,top:20},"
                    "xAxis:{type:'category',data:[");
        for (int i = 0; i < n; i++) { char tb[64]; fmt_time(tb, eu[i].startTime); fprintf(fp, "'%s',", tb); }
        fprintf(fp, "],axisLabel:{color:'#6b7280'},splitLine:{show:false}},yAxis:{type:'value',axisLabel:{color:'#6b7280'},splitLine:{color:'#f3f4f6'}},"
                    "series:[{type:'line',smooth:true,data:[");
        for (int i = 0; i < n; i++) fprintf(fp, "%d,", eu[i].newRating);
        fprintf(fp, "],lineStyle:{width:3},symbol:'circle',symbolSize:6,itemStyle:{color:'#2563eb'},areaStyle:{color:{type:'linear',x:0,y:0,x2:0,y2:1,"
                    "colorStops:[{offset:0,color:'rgba(37,99,235,.2)'},{offset:1,color:'rgba(37,99,235,0)'}]}}}]});</script>");

        // 使用 ECharts 绘制各时间段下已解决题目难度分布的柱状图
        fprintf(fp, "<div class='card'><h2>通过题目难度分布</h2><div class='btn-group'>"
                    "<button id='btn0' class='btn btn-active' onclick='switchHist(0)'>全部</button>"
                    "<button id='btn1' class='btn btn-idle' onclick='switchHist(1)'>最近一年</button>"
                    "<button id='btn2' class='btn btn-idle' onclick='switchHist(2)'>最近180天</button>"
                    "<button id='btn3' class='btn btn-idle' onclick='switchHist(3)'>最近1个月</button>"
                    "</div><div id='hist'></div></div><script>var histData={all:[");
        for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", ba[i]);
        fprintf(fp, "],year:[");  for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", by[i]);
        fprintf(fp, "],d180:[");  for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", b180[i]);
        fprintf(fp, "],month:["); for (int i = 0; i < nbins; i++) fprintf(fp, "%d,", bm[i]);
        fprintf(fp, "],labels:["); for (int i = 0; i < nbins; i++) fprintf(fp, "'%s',", range_labels[i]);
        fprintf(fp, "]};function switchHist(p){"
                    "for(var i=0;i<4;i++){var b=document.getElementById('btn'+i);"
                    "b.className=i===p?'btn btn-active':'btn btn-idle';}"
                    "var k=['all','year','d180','month'][p];"
                    "var h=echarts.init(document.getElementById('hist'));h.setOption({backgroundColor:'transparent',"
                    "tooltip:{trigger:'axis'},grid:{left:60,right:20,bottom:40,top:20},"
                    "xAxis:{type:'category',data:histData.labels,axisLabel:{color:'#6b7280',rotate:30},splitLine:{show:false}},"
                    "yAxis:{type:'value',axisLabel:{color:'#6b7280'},splitLine:{color:'#f3f4f6'}},"
                    "series:[{type:'bar',data:histData[k],itemStyle:{color:'#3b82f6',borderRadius:[4,4,0,0]}}]});}switchHist(0);</script>");

        // 输出历次比赛记录，通过颜色区分题目：赛内通过(绿色)、赛后补题(黄色)、未通过(灰色)
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
                const char *cls = eu[i].status[p]==1 ? "ok" : eu[i].status[p]==2 ? "after" : "no";
                fprintf(fp, "<div class='prob %s'><div style='font-weight:700'>%s</div></div>", cls, eu[i].labels[p]);
            }
            fprintf(fp, "</td></tr>\n");
        }
        fprintf(fp, "</tbody></table></div></div></body></html>"); fclose(fp);
        printf("  %s 已生成\n", fn);
    }

    // === 如有未找到的用户，生成 missing_users.html 报告未找到的用户名单 ===
    if (nmissing > 0) {
        fp = fopen("missing_users.html", "w");
        if (fp) {
            fprintf(fp, "<html><head><meta charset='utf-8'><title>Missing Users</title><style>"
                        "*{margin:0;padding:0;box-sizing:border-box}body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,Helvetica,Arial,sans-serif;background:#f3f4f6;color:#1f2937;padding:40px}"
                        ".card{max-width:600px;margin:0 auto;background:#fff;padding:32px;border-radius:16px;box-shadow:0 4px 6px -1px rgba(0,0,0,.1);border:1px solid #e5e7eb}"
                        "h1{font-size:24px;margin-bottom:16px;color:#ef4444}ul{list-style-type:disc;padding-left:24px}li{margin-bottom:8px;font-size:16px;color:#4b5563}"
                        "</style></head><body><div class='card'><h1>以下用户未找到</h1><ul>");
            for (int i = 0; i < nmissing; i++) fprintf(fp, "<li>%s</li>", missing[i]);
            fprintf(fp, "</ul></div></body></html>"); fclose(fp);
            printf("missing_users.html 已生成\n");
        }
    }

    // === 打开生成的默认主页报告页面 ===
    char cmd[256];
    if (nusers == 1) snprintf(cmd, sizeof(cmd), "start \"\" \"%s_report.html\"", users[0].handle);
    else cstr(cmd, "start \"\" \"index.html\"", sizeof(cmd));
    system(cmd);

    // === 内存释放及资源清理 ===
    for (int i = 0; i < nusers; i++) { free(entries[i]); free(solved[i]); }
    free(pmap_ids); free(pmap_ratings); free(pmap_indices);
    if (cl_root) cJSON_Delete(cl_root);
    curl_global_cleanup();
    printf("\n完成！打开 index.html 查看用户列表\n");
    return 0;
}


