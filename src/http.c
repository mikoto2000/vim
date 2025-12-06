/* vi:set ts=8 sts=4 sw=4 noet:
 *
 * VIM - Vi IMproved	by Bram Moolenaar
 *
 * Do ":help uganda"  in Vim to read copying and usage conditions.
 * Do ":help credits" in Vim to see a list of people who contributed.
 * See README.txt for an overview of the Vim source code.
 */

#include "vim.h"

/*
 * http.c: http functions
 */
#ifdef HAVE_HTTP

struct Memory {
    char *response;
    size_t size;
};

static int xferinfo_cb(void *clientp UNUSED, curl_off_t dltotal UNUSED, curl_off_t dlnow UNUSED,
                       curl_off_t ultotal UNUSED, curl_off_t ulnow UNUSED)
{
    ui_breakcheck();
    return got_int ? 1 : 0;  // 非0→CURLE_ABORTED_BY_CALLBACK
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct Memory *mem = (struct Memory *)userp;

    char *ptr = realloc(mem->response, mem->size + realsize + 1);
    if(ptr == NULL) return 0; /* out of memory! */

    mem->response = ptr;
    memcpy(&(mem->response[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->response[mem->size] = 0;

    return realsize;
}

static size_t header_callback(char *buffer, size_t size, size_t nitems, void *userdata)
{
    size_t realsize = size * nitems;
    dict_T *dict = (dict_T *)userdata;

    // "Key: Value\r\n" 形式
    char *sep = memchr(buffer, ':', realsize);
    if (!sep) return realsize;

    size_t key_len = (size_t)(sep - buffer);
    size_t value_len = realsize - key_len - 1; // ':' を除いた長さ（この後で前後空白と CRLF を削る）

    // 値の先頭/末尾トリム（": " や CRLF を除去）
    char *val_start = sep + 1;
    while (value_len > 0 && (*val_start == ' ' || *val_start == '\t')) {
        val_start++; value_len--;
    }
    while (value_len > 0 && (val_start[value_len-1] == '\r' || val_start[value_len-1] == '\n')) {
        value_len--;
    }

    // key を小文字化して保存（HTTP は case-insensitive）
    char_u *key = vim_strnsave((char_u *)buffer, (int)key_len);
    for (int i = 0; i < (int)key_len; ++i) key[i] = (char_u)TOLOWER_ASC(key[i]);

    // 値を Vim のメモリで確保
    char_u *val = vim_strnsave((char_u *)val_start, (int)value_len);

    // 既存エントリを探す
    dictitem_T *di = dict_find(dict, key, (int)key_len);

    if (di == NULL) {
        // 初出 → 文字列で追加
        dict_add_string(dict, key, val);
    } else {
        // 既に同名キーがある → リストに畳み込む
        typval_T *tv = &di->di_tv;
        if (tv->v_type != VAR_LIST) {
            // 既存の文字列をリストへ昇格
            list_T *l = list_alloc();
            if (l != NULL) {
                list_append_string(l, tv->vval.v_string, -1);  // 既存値
                clear_tv(tv);
                tv->v_type = VAR_LIST;
                tv->vval.v_list = l;
                l->lv_refcount++;
            }
        }
        if (tv->v_type == VAR_LIST) {
            list_append_string(tv->vval.v_list, val, -1);     // 新しい値を追加
        }
        // tv に取り込んだので val は解放不要（list_append_string が取り込む）
        vim_free(val);
    }

    vim_free(key);
    return realsize;
}

    void
f_httprequest(typval_T *argvars, typval_T *rettv)
{
    char_u *method;
    dict_T *request_headers_dict = NULL;
    char_u *url;
    char_u *body = NUL;

    if (rettv_dict_alloc(rettv) == FAIL)
	return;

    method = tv_get_string_chk(&argvars[0]);
    url = tv_get_string_chk(&argvars[1]);
    if (method == NULL || url == NULL)
	return;
    if (argvars[2].v_type == VAR_DICT)
	request_headers_dict = argvars[2].vval.v_dict;
    else if (argvars[2].v_type == VAR_SPECIAL
	    && argvars[2].vval.v_number == VVAL_NULL)
	request_headers_dict = NULL; // v:null → ヘッダーなし扱い
    else
    {
	semsg(_(e_dict_required_for_argument_nr), 3);
	return;
    }
    if (check_for_opt_string_arg(argvars, 3) != FAIL)
    {
	body = tv_get_string_chk(&argvars[3]);
    } else {
	body = vim_strsave((char_u *)"");
    }

    // リクエストヘッダ組み立て
    hashitem_T *hi;
    dictitem_T *di;
    int todo;
    struct curl_slist *request_headers = NULL;

    if (request_headers_dict != NULL) {
	hashitem_T *hi;
	dictitem_T *di;
	int todo = (int)request_headers_dict->dv_hashtab.ht_used;

	for (hi = request_headers_dict->dv_hashtab.ht_array; todo > 0; ++hi) {
	    if (!HASHITEM_EMPTY(hi)) {
		--todo;
		di = HI2DI(hi);

		char *key = (char *)di->di_key;
		typval_T *tv = &di->di_tv;

		if (tv->v_type == VAR_STRING && tv->vval.v_string != NULL) {
		    /* 1 値 */
		    size_t len = strlen(key) + 2 + strlen((char*)tv->vval.v_string) + 1;
		    char *hdr = malloc(len);
		    snprintf(hdr, len, "%s: %s", key, (char*)tv->vval.v_string);
		    request_headers = curl_slist_append(request_headers, hdr);
		    free(hdr);
		} else if (tv->v_type == VAR_LIST && tv->vval.v_list != NULL) {
		    /* 複数値 */
		    for (listitem_T *li = tv->vval.v_list->lv_first; li; li = li->li_next) {
			char_u *sv = tv_get_string_chk(&li->li_tv);
			if (sv == NULL) continue;
			size_t len = strlen(key) + 2 + strlen((char*)sv) + 1;
			char *hdr = malloc(len);
			snprintf(hdr, len, "%s: %s", key, (char*)sv);
			request_headers = curl_slist_append(request_headers, hdr);
			free(hdr);
		    }
		} else {
		    semsg(_(e_invalid_argument_str),
			    "header values must be string or list of string");  /* 値が文字列/リスト以外ならエラー */
		}
	    }
	}
    }

    if (url != NULL && *url != NUL)
    {
	CURL *curl;
	CURLcode res;

	long http_code = 0;
	dict_T *response_headers = dict_alloc();

	struct Memory chunk = {0};
	chunk.response = vim_strsave((char_u *)"");  // 最初から空文字を入れる
	chunk.size = 0;

	curl = curl_easy_init();
	if(curl) {
	    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
	    if (request_headers != NULL)
	    {
		curl_easy_setopt(curl, CURLOPT_HTTPHEADER, request_headers);
	    }
	    curl_easy_setopt(curl, CURLOPT_URL, url);
	    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
	    if (body != NULL && *body != NUL)
	    {
		curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
		curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(body));
	    }


	    // 進捗コールバック
	    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
	    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
	    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, NULL);

	    // リクエストボディ受信コールバック
	    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
	    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)&chunk);

	    // ヘッダー受信コールバック
	    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
	    curl_easy_setopt(curl, CURLOPT_HEADERDATA, (void *)response_headers);

	    // リクエスト送信
	    res = curl_easy_perform(curl);
	    if (res != CURLE_OK) {
		// 失敗時は body を空文字で返す（NULL は絶対に入れない）
		dict_add_number(rettv->vval.v_dict, "success", 0);
		dict_add_number(rettv->vval.v_dict, "status", 0);
		dict_add_dict(rettv->vval.v_dict, "headers", response_headers);
		dict_add_string(rettv->vval.v_dict, "body", vim_strsave((char_u *)""));
		if (request_headers) curl_slist_free_all(request_headers);
		vim_free(chunk.response);
		curl_easy_cleanup(curl);
		return;
	    }

	    // ステータスコード取得
	    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

	    if (request_headers) curl_slist_free_all(request_headers);
	    curl_easy_cleanup(curl);

	    // 成功フラグを立てる
	    dict_add_number(rettv->vval.v_dict, "success", 1);

	    // ステータスコード
	    dict_add_number(rettv->vval.v_dict, "status", http_code);

	    // ヘッダー
	    dict_add_dict(rettv->vval.v_dict, "headers", response_headers);

	    // レスポンスボディ
	    dict_add_string(rettv->vval.v_dict, "body", vim_strsave(chunk.response));
	    free(chunk.response);
	}
    }
}
#endif // HAVE_HTTP
