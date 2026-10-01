// Native libsoup owns parsing, scoping, expiry and SQLite persistence.
// This synchronous JSON boundary never logs cookie/header contents.
#include <libsoup/soup.h>
#include <json-glib/json-glib.h>
#include <glib/gstdio.h>
#include <pthread.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static SoupCookieJar *jar;
static const char *string(JsonObject *o,const char *key) {
    JsonNode *n=json_object_get_member(o,key);
    return n && json_node_get_value_type(n)==G_TYPE_STRING?json_node_get_string(n):NULL;
}
static gint64 number(JsonObject *o,const char *key) {
    JsonNode *n=json_object_get_member(o,key);if(!n)return 0;
    return json_node_get_value_type(n)==G_TYPE_BOOLEAN?json_node_get_boolean(n):json_node_get_int(n);
}
static JsonObject *properties(SoupCookie *c) {
    JsonObject *o=json_object_new();
    json_object_set_string_member(o,"Name",soup_cookie_get_name(c));
    json_object_set_string_member(o,"Value",soup_cookie_get_value(c));
    json_object_set_string_member(o,"Domain",soup_cookie_get_domain(c));
    json_object_set_string_member(o,"Path",soup_cookie_get_path(c));
    json_object_set_boolean_member(o,"Secure",soup_cookie_get_secure(c));
    json_object_set_boolean_member(o,"HTTPOnly",soup_cookie_get_http_only(c));
    json_object_set_int_member(o,"Version",0);
    json_object_set_int_member(o,"SameSite",soup_cookie_get_same_site_policy(c));
    GDateTime *date=soup_cookie_get_expires(c);
    if(date)json_object_set_int_member(o,"Expires",g_date_time_to_unix(date));
    return o;
}
static SoupCookie *cookie(JsonObject *o) {
    const char *name=string(o,"Name"),*value=string(o,"Value"),*domain=string(o,"Domain"),*path=string(o,"Path");
    if(!name || !*name || !value || !domain || !*domain || !path || *path!='/')return NULL;
    SoupCookie *c=soup_cookie_new(name,value,domain,path,-1);if(!c)return NULL;
    soup_cookie_set_secure(c,number(o,"Secure"));soup_cookie_set_http_only(c,number(o,"HTTPOnly"));
    if(json_object_has_member(o,"Expires")) {
        GDateTime *date=g_date_time_new_from_unix_utc(number(o,"Expires"));
        if(date){soup_cookie_set_expires(c,date);g_date_time_unref(date);}
    }
    if(json_object_has_member(o,"Max-Age"))soup_cookie_set_max_age(c,(int)number(o,"Max-Age"));
    if(number(o,"Discard"))soup_cookie_set_expires(c,NULL);
    int same=number(o,"SameSite");if(same>=0 && same<=2)soup_cookie_set_same_site_policy(c,same);
    return c;
}
static gboolean origin_matches(SoupCookie *c,GUri *origin) {
    const char *domain=soup_cookie_get_domain(c);
    if(origin && !soup_cookie_domain_matches(c,g_uri_get_host(origin)))return FALSE;
    return !(*domain=='.' && soup_tld_domain_is_public_suffix(domain+1));
}
static GUri *uri(const char *text) {
    GUri *u=text?g_uri_parse(text,SOUP_HTTP_URI_FLAGS,NULL):NULL;
    if(u && (!g_uri_get_host(u) || (g_strcmp0(g_uri_get_scheme(u),"https") && g_strcmp0(g_uri_get_scheme(u),"http")))){g_uri_unref(u);return NULL;}
    return u;
}
static void append_header(SoupMessageHeaders *headers,JsonNode *node) {
    if(JSON_NODE_HOLDS_ARRAY(node)) {
        JsonArray *a=json_node_get_array(node);for(guint i=0;i<json_array_get_length(a);i++)append_header(headers,json_array_get_element(a,i));
    } else if(json_node_get_value_type(node)==G_TYPE_STRING) {
        const char *s=json_node_get_string(node);
        if(strchr(s,'\r') || strchr(s,'\n'))return;
        // Foundation may combine repeated headers. Only split before a new name=,
        // leaving Expires dates and quoted values for libsoup to parse unchanged.
        const char *start=s;gboolean quoted=FALSE;
        for(const char *p=s;*p;p++) {
            if(quoted && *p=='\\' && p[1]){p++;continue;}
            if(*p=='"')quoted=!quoted;
            if(*p!=',' || quoted)continue;
            const char *q=p+1;while(*q==' ' || *q=='\t')q++;
            const char *name=q;
            while(*q && (g_ascii_isalnum(*q) || strchr("!#$%&'*+-.^_`|~",*q)))q++;
            if(q==name || *q!='=')continue;
            char *part=g_strndup(start,p-start);soup_message_headers_append(headers,"Set-Cookie",part);g_free(part);start=p+1;
        }
        soup_message_headers_append(headers,"Set-Cookie",start);
    }
}
char *rbx_cookies_call(const char *request) {
    pthread_mutex_lock(&lock);
    JsonParser *parser=json_parser_new();JsonArray *result=json_array_new();const char *error=NULL;
    GUri *origin=NULL,*first=NULL;GSList *list=NULL;SoupMessage *message=NULL;
    if(!request || !json_parser_load_from_data(parser,request,-1,NULL) || !JSON_NODE_HOLDS_OBJECT(json_parser_get_root(parser))){error="Invalid cookie request";goto done;}
    JsonObject *o=json_node_get_object(json_parser_get_root(parser));const char *op=string(o,"op");
    if(!op){error="Missing cookie operation";goto done;}
    origin=uri(string(o,"url"));first=uri(string(o,"first"));
    if((string(o,"url") && !origin) || (string(o,"first") && !first))goto done;
    if(!strcmp(op,"parse")) {
        JsonNode *headers=json_object_get_member(o,"headers");
        if(!origin || !headers || !JSON_NODE_HOLDS_OBJECT(headers))goto done;
        message=soup_message_new_from_uri("GET",origin);
        JsonObject *h=json_node_get_object(headers);GList *keys=json_object_get_members(h);
        for(GList *i=keys;i;i=i->next)if(!g_ascii_strcasecmp(i->data,"Set-Cookie"))append_header(soup_message_get_response_headers(message),json_object_get_member(h,i->data));
        g_list_free(keys);list=soup_cookies_from_response(message);
    } else {
        if(!jar) {
            const char *path=string(o,"directory");
            if(!path || !g_path_is_absolute(path) || g_mkdir_with_parents(path,0700) || g_chmod(path,0700)){error="Cannot open private cookie directory";goto done;}
            char *file=g_build_filename(path,"native.sqlite",NULL);
            int fd=g_open(file,O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);struct stat st;
            if(fd<0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || fchmod(fd,0600)){if(fd>=0)close(fd);g_free(file);error="Cannot open private cookie database";goto done;}
            close(fd);jar=soup_cookie_jar_db_new(file,FALSE);g_free(file);
            if(!jar){error="Cannot open persistent cookie store";goto done;}
        }
        if(!strcmp(op,"get"))list=origin?soup_cookie_jar_get_cookie_list(jar,origin,TRUE):soup_cookie_jar_all_cookies(jar);
        else if(!strcmp(op,"set") || !strcmp(op,"delete")) {
            JsonNode *values=json_object_get_member(o,"cookies");if(!values || !JSON_NODE_HOLDS_ARRAY(values))goto done;
            int policy=number(o,"policy");soup_cookie_jar_set_accept_policy(jar,policy>=0 && policy<=2?policy:0);
            JsonArray *a=json_node_get_array(values);
            for(guint i=0;i<json_array_get_length(a);i++) {
                JsonNode *n=json_array_get_element(a,i);if(!JSON_NODE_HOLDS_OBJECT(n))continue;
                SoupCookie *c=cookie(json_node_get_object(n));if(!c)continue;
                if(!strcmp(op,"delete")){soup_cookie_jar_delete_cookie(jar,c);soup_cookie_free(c);}
                else if(policy==1 || !origin_matches(c,origin))soup_cookie_free(c);
                else soup_cookie_jar_add_cookie_full(jar,c,origin,first);
            }
        } else error="Unknown cookie operation";
    }
    for(GSList *i=list;i;i=i->next) {
        SoupCookie *c=i->data;
        if(origin_matches(c,!strcmp(op,"parse")?origin:NULL))json_array_add_object_element(result,properties(c));
    }
done:
    if(list)soup_cookies_free(list);
    if(message)g_object_unref(message);if(origin)g_uri_unref(origin);if(first)g_uri_unref(first);
    JsonObject *reply=json_object_new();json_object_set_array_member(reply,"cookies",result);
    if(error)json_object_set_string_member(reply,"error",error);
    JsonNode *node=json_node_new(JSON_NODE_OBJECT);json_node_take_object(node,reply);char *text=json_to_string(node,FALSE);
    json_node_free(node);g_object_unref(parser);pthread_mutex_unlock(&lock);return text;
}
void rbx_cookies_free(void *text){g_free(text);}
