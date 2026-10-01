// GTK owns the chrome; the Mach-O client and WebKit remain separate processes.
#include <gtk/gtk.h>
#include <gtk/gtkx.h>
#include <webkit2/webkit2.h>
#include <json-glib/json-glib.h>
#include <glib-unix.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <signal.h>
#include <deque>
#include <string>
#include <map>
#include "client-url.h"

static GtkWidget *window,*header,*stack,*socket_view,*back,*forward,*reload,*done;
static GSubprocess *guest;
static WebKitWebContext *context;
static std::string browser_data;
static unsigned long guest_xid;
static int exit_status,listener=-1;
static std::string socket_path;
struct Peer {int fd;std::string input;std::deque<std::string> output;guint write_watch=0;};
static Peer *peer;
struct Page {long id;WebKitWebView *view;bool delegate=false;std::string panel_title;};
static std::map<long,Page*> pages;
static std::map<long,WebKitPolicyDecision*> decisions;
static long decision_id;
static bool native_wayland;
static bool close_requested;
static void (*close_input)(void);
static void request_close(){if(close_input)close_input();close_requested=true;}
static gboolean expire_policy(gpointer);
static Page *current;
static std::string compatible_user_agent(const char *agent) {
    std::string result=agent?agent:"";
    // Apple's WebKit accepts Roblox's concatenated products; WebKitGTK's
    // HTTP parser rejects the second slash and silently retains its desktop UA.
    const char *joined="Roblox/DarwinRobloxApp/";
    auto at=result.find(joined);
    if(at!=std::string::npos)result.insert(at+strlen("Roblox/Darwin")," ");
    // Embedded pages must select their macOS native bridge, not a Linux/browser
    // fallback. Match the guest platform only for Roblox's app-tagged views.
    if(result.find("RobloxApp/")!=std::string::npos) {
        const char *host="(X11; Linux x86_64)";at=result.find(host);
        if(at!=std::string::npos)result.replace(at,strlen(host),"(Macintosh; Intel Mac OS X 10_15_7)");
    }
    return result;
}
static void set_user_agent(Page *page,const char *agent) {
    bool valid=agent && *agent;
    for(const unsigned char *p=(const unsigned char*)agent;p && *p;p++)if(*p<32 || *p>126)valid=false;
    auto compatible=compatible_user_agent(agent);
    webkit_settings_set_user_agent(webkit_web_view_get_settings(page->view),valid?compatible.c_str():nullptr);
}
static const char *str(JsonObject *o,const char *key,const char *fallback="") {
    return json_object_has_member(o,key) && JSON_NODE_HOLDS_VALUE(json_object_get_member(o,key)) &&
        json_node_get_value_type(json_object_get_member(o,key))==G_TYPE_STRING ? json_object_get_string_member(o,key) : fallback;
}
static long num(JsonObject *o,const char *key,long fallback=0) {
    if(json_object_has_member(o,key) && json_node_get_value_type(json_object_get_member(o,key))==G_TYPE_BOOLEAN)return json_object_get_boolean_member(o,key);
    return json_object_has_member(o,key) && JSON_NODE_HOLDS_VALUE(json_object_get_member(o,key)) &&
        json_node_get_value_type(json_object_get_member(o,key))==G_TYPE_INT64 ? json_object_get_int_member(o,key) : fallback;
}
static gboolean flush_output(gint,gpointer);
static gboolean writable(gint fd,GIOCondition,gpointer data) {return flush_output(fd,data);}
static gboolean flush_output(gint fd,gpointer data) {
    auto p=(Peer*)data;
    while(!p->output.empty()) {
        auto &s=p->output.front();ssize_t n=send(fd,s.data(),s.size(),MSG_NOSIGNAL);
        if(n<0 && (errno==EAGAIN || errno==EINTR))return G_SOURCE_CONTINUE;
        if(n<=0){p->output.clear();break;}
        s.erase(0,n);if(s.empty())p->output.pop_front();
    }
    p->write_watch=0;return G_SOURCE_REMOVE;
}
static void send_object(JsonObject *o) {
    JsonNode *node=json_node_new(JSON_NODE_OBJECT);json_node_take_object(node,o);
    char *data=json_to_string(node,FALSE);
    if(peer && peer->fd>=0 && peer->output.size()<128) {
        peer->output.emplace_back(std::string(data)+"\n");
        if(!peer->write_watch)peer->write_watch=g_unix_fd_add(peer->fd,G_IO_OUT,writable,peer);
    }
    g_free(data);json_node_free(node);
}
static JsonObject *event(Page *p,const char *type) {
    auto o=json_object_new();json_object_set_int_member(o,"view",p?p->id:0);json_object_set_string_member(o,"event",type);return o;
}
static bool web_url(const char *url) {
    if(!url || strlen(url)>16384)return false;
    for(const unsigned char *p=(const unsigned char*)url;*p;p++)if(*p<32 || *p==127)return false;
    GUri *uri=g_uri_parse(url,G_URI_FLAGS_NONE,nullptr);
    bool ok=uri && g_uri_get_host(uri) && !g_uri_get_userinfo(uri) &&
        (!g_strcmp0(g_uri_get_scheme(uri),"https") || !g_strcmp0(g_uri_get_scheme(uri),"http"));
    if(uri)g_uri_unref(uri);
    return ok;
}
static bool request_header(const char *name,const char *value) {
    if(!*name || strlen(name)>256 || strlen(value)>16384)return false;
    for(const unsigned char *p=(const unsigned char*)name;*p;p++)
        if(!g_ascii_isalnum(*p) && !strchr("!#$%&'*+-.^_`|~",*p))return false;
    for(const unsigned char *p=(const unsigned char*)value;*p;p++)if((*p<32 && *p!='\t') || *p==127)return false;
    for(auto reserved:{"Host","Content-Length","Connection","Transfer-Encoding"})if(!g_ascii_strcasecmp(name,reserved))return false;
    return true;
}
static void controls() {
    bool browsing=current!=nullptr;
    bool panel=browsing && !current->panel_title.empty();
    for(auto w:{back,forward,reload})gtk_widget_set_visible(w,browsing && !panel);
    gtk_widget_set_visible(done,browsing);gtk_button_set_label(GTK_BUTTON(done),panel?"Close":"Back to Roblox");
    if(browsing) {
        gtk_widget_set_sensitive(back,webkit_web_view_can_go_back(current->view));
        gtk_widget_set_sensitive(forward,webkit_web_view_can_go_forward(current->view));
        const char *title=panel?current->panel_title.c_str():webkit_web_view_get_title(current->view);
        gtk_header_bar_set_title(GTK_HEADER_BAR(header),title && *title?title:"Roblox — Browser");
        const char *url=webkit_web_view_get_uri(current->view);GUri *uri=url?g_uri_parse(url,G_URI_FLAGS_NONE,nullptr):nullptr;
        gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),!panel && uri?g_uri_get_host(uri):nullptr);
        if(uri)g_uri_unref(uri);
    } else {gtk_header_bar_set_title(GTK_HEADER_BAR(header),"Roblox");gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),nullptr);}
}
static void return_to_game(GtkWidget*,gpointer) {
    if(current)send_object(event(current,"closed"));
    current=nullptr;
    gtk_stack_set_visible_child(GTK_STACK(stack),socket_view);controls();
    gtk_widget_grab_focus(socket_view);
}
static void state(WebKitWebView *view,GParamSpec*,gpointer data) {
    Page *p=(Page*)data;auto o=event(p,"state");
    json_object_set_string_member(o,"url",webkit_web_view_get_uri(view)?webkit_web_view_get_uri(view):"");
    json_object_set_string_member(o,"title",webkit_web_view_get_title(view)?webkit_web_view_get_title(view):"");
    json_object_set_boolean_member(o,"back",webkit_web_view_can_go_back(view));
    json_object_set_boolean_member(o,"forward",webkit_web_view_can_go_forward(view));
    json_object_set_boolean_member(o,"loading",webkit_web_view_is_loading(view));send_object(o);if(current==p)controls();
}
static void loaded(WebKitWebView *view,WebKitLoadEvent stage,gpointer data) {
    auto p=(Page*)data;auto o=event(p,"load");json_object_set_int_member(o,"stage",stage);send_object(o);state(view,nullptr,data);
}
static gboolean failed(WebKitWebView*,WebKitLoadEvent,const char*,GError*,gpointer data) {
    // Never log the URL: authentication flows may put transient secrets in it.
    auto o=event((Page*)data,"error");json_object_set_string_member(o,"message","The embedded page could not be loaded.");send_object(o);return FALSE;
}
static gboolean policy(WebKitWebView *view,WebKitPolicyDecision *d,WebKitPolicyDecisionType type,gpointer data) {
    auto p=(Page*)data;
    if(type==WEBKIT_POLICY_DECISION_TYPE_RESPONSE)return FALSE;
    auto action=webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    const char *url=webkit_uri_request_get_uri(webkit_navigation_action_get_request(action));
    if(rbx_client_url(url)) {
        // Server IDs, follow-user IDs and authentication tickets belong to the
        // running client's URL handler. Never send these through an OS launcher.
        auto o=event(p,"launch-url");json_object_set_string_member(o,"url",url);
        send_object(o);webkit_policy_decision_ignore(d);return TRUE;
    }
    if(!web_url(url) && g_strcmp0(url,"about:blank")){webkit_policy_decision_ignore(d);return TRUE;}
    if(type==WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) {webkit_policy_decision_ignore(d);webkit_web_view_load_uri(view,url);return TRUE;}
    if(!p->delegate)return FALSE;
    long id=++decision_id;decisions[id]=(WebKitPolicyDecision*)g_object_ref(d);
    g_timeout_add_seconds(15,expire_policy,(gpointer)id);
    auto o=event(p,"navigation");json_object_set_int_member(o,"decision",id);json_object_set_string_member(o,"url",url);
    json_object_set_int_member(o,"type",webkit_navigation_action_get_navigation_type(action));send_object(o);return TRUE;
}
struct Message {Page *page;std::string name;};
static void message(WebKitUserContentManager*,WebKitJavascriptResult *result,gpointer data) {
    auto m=(Message*)data;char *body=jsc_value_to_json(webkit_javascript_result_get_js_value(result),0);
    auto o=event(m->page,"message");json_object_set_string_member(o,"name",m->name.c_str());
    if(body){auto n=json_from_string(body,nullptr);if(n)json_object_set_member(o,"body",n);g_free(body);}send_object(o);
}
static void terminated(WebKitWebView*,WebKitWebProcessTerminationReason,gpointer data) {
    auto o=event((Page*)data,"error");json_object_set_string_member(o,"message","The embedded browser process stopped unexpectedly.");send_object(o);
    g_printerr("roblox-mac: embedded browser process terminated\n");
}
static Page *page(long id) {
    if(pages.count(id))return pages[id];
    if(!context) {
        std::string base=browser_data;
        auto manager=webkit_website_data_manager_new("base-data-directory",(base+"/data").c_str(),"base-cache-directory",(base+"/cache").c_str(),nullptr);
        context=webkit_web_context_new_with_website_data_manager(manager);g_object_unref(manager);
        webkit_cookie_manager_set_persistent_storage(webkit_web_context_get_cookie_manager(context),(base+"/cookies.sqlite").c_str(),WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }
    auto manager=webkit_user_content_manager_new();
    auto p=new Page{id,WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,"web-context",context,"user-content-manager",manager,nullptr))};
    g_object_unref(manager);pages[id]=p;
    gtk_stack_add_named(GTK_STACK(stack),GTK_WIDGET(p->view),std::to_string(id).c_str());
    g_signal_connect(p->view,"load-changed",G_CALLBACK(loaded),p);
    g_signal_connect(p->view,"load-failed",G_CALLBACK(failed),p);
    g_signal_connect(p->view,"web-process-terminated",G_CALLBACK(terminated),p);
    g_signal_connect(p->view,"notify::title",G_CALLBACK(state),p);
    g_signal_connect(p->view,"notify::uri",G_CALLBACK(state),p);
    g_signal_connect(p->view,"decide-policy",G_CALLBACK(policy),p);
    gtk_widget_show(GTK_WIDGET(p->view));return p;
}
struct Reply {long request;Page *p;};
static void evaluated(GObject *source,GAsyncResult *result,gpointer data) {
    auto r=(Reply*)data;GError *error=nullptr;
    auto value=webkit_web_view_evaluate_javascript_finish(WEBKIT_WEB_VIEW(source),result,&error);
    auto o=event(r->p,"reply");json_object_set_int_member(o,"request",r->request);
    if(error){json_object_set_string_member(o,"error","JavaScript evaluation failed");g_error_free(error);}
    else if(value){char *json=jsc_value_to_json(value,0);if(json){auto n=json_from_string(json,nullptr);if(n)json_object_set_member(o,"value",n);g_free(json);}g_object_unref(value);}
    send_object(o);delete r;
}
static void cookies_received(GObject *source,GAsyncResult *result,gpointer data) {
    auto r=(Reply*)data;GError *error=nullptr;
    auto cookies=webkit_cookie_manager_get_all_cookies_finish(WEBKIT_COOKIE_MANAGER(source),result,&error);
    auto o=event(nullptr,"reply");json_object_set_int_member(o,"request",r->request);
    if(error){json_object_set_string_member(o,"error","Could not read browser cookies");g_error_free(error);}
    else {
        auto array=json_array_new();
        for(auto item=cookies;item;item=item->next) {
            auto c=(SoupCookie*)item->data;auto value=json_object_new();
            json_object_set_string_member(value,"name",soup_cookie_get_name(c));json_object_set_string_member(value,"value",soup_cookie_get_value(c));
            json_object_set_string_member(value,"domain",soup_cookie_get_domain(c));json_object_set_string_member(value,"path",soup_cookie_get_path(c));
            json_object_set_boolean_member(value,"secure",soup_cookie_get_secure(c));json_object_set_boolean_member(value,"httpOnly",soup_cookie_get_http_only(c));
            if(auto date=soup_cookie_get_expires(c))json_object_set_int_member(value,"expires",g_date_time_to_unix(date));
            json_array_add_object_element(array,value);
        }
        json_object_set_array_member(o,"value",array);
    }
    g_list_free_full(cookies,(GDestroyNotify)soup_cookie_free);send_object(o);delete r;
}
static void cookie_added(GObject *source,GAsyncResult *result,gpointer data) {
    auto r=(Reply*)data;GError *error=nullptr;
    auto o=event(nullptr,"reply");json_object_set_int_member(o,"request",r->request);
    if(!webkit_cookie_manager_add_cookie_finish(WEBKIT_COOKIE_MANAGER(source),result,&error))json_object_set_string_member(o,"error","Could not write browser cookie");
    if(error)g_error_free(error);
    send_object(o);delete r;
}
static gboolean expire_policy(gpointer data) {
    long id=(long)data;auto i=decisions.find(id);
    if(i!=decisions.end()){webkit_policy_decision_ignore(i->second);g_object_unref(i->second);decisions.erase(i);}return G_SOURCE_REMOVE;
}
static void handle(JsonObject *o) {
    const char *op=str(o,"op");long id=num(o,"view");
    if(!strcmp(op,"return-to-game")){return_to_game(nullptr,nullptr);return;}
    if(!strcmp(op,"attach")) {
        if(!guest_xid){guest_xid=num(o,"window");if(!native_wayland)gtk_socket_add_id(GTK_SOCKET(socket_view),guest_xid);gtk_widget_show_all(window);controls();}
        return;
    }
    if(!strcmp(op,"policy")) {
        long d=num(o,"decision");if(decisions.count(d)){auto p=decisions[d];if(num(o,"allow"))webkit_policy_decision_use(p);else webkit_policy_decision_ignore(p);g_object_unref(p);decisions.erase(d);}return;
    }
    if(!strcmp(op,"close")){
        if(current && current->id==id){current=nullptr;return_to_game(nullptr,nullptr);}
        if(pages.count(id) && pages[id]->view){webkit_web_view_stop_loading(pages[id]->view);gtk_widget_destroy(GTK_WIDGET(pages[id]->view));pages[id]->view=nullptr;}
        return;
    }
    if(id<0 || id>1000000)return;
    Page *p=page(id);if(!p->view)return;
    if(!strcmp(op,"cookies-get")) {
        webkit_cookie_manager_get_all_cookies(webkit_web_context_get_cookie_manager(context),nullptr,cookies_received,new Reply{num(o,"request"),nullptr});return;
    }
    if(!strcmp(op,"cookie-set")) {
        if(!json_object_has_member(o,"cookie") || !JSON_NODE_HOLDS_OBJECT(json_object_get_member(o,"cookie")))return;
        auto c=json_object_get_object_member(o,"cookie");
        auto cookie=soup_cookie_new(str(c,"name"),str(c,"value"),str(c,"domain"),str(c,"path","/"),-1);
        if(!cookie)return;
        soup_cookie_set_secure(cookie,num(c,"secure"));soup_cookie_set_http_only(cookie,num(c,"httpOnly"));
        if(json_object_has_member(c,"expires")){auto date=g_date_time_new_from_unix_utc(json_object_get_double_member(c,"expires"));if(date){soup_cookie_set_expires(cookie,date);g_date_time_unref(date);}}
        webkit_cookie_manager_add_cookie(webkit_web_context_get_cookie_manager(context),cookie,nullptr,cookie_added,new Reply{num(o,"request"),nullptr});soup_cookie_free(cookie);return;
    }
    if(!strcmp(op,"load")) {
        const char *url=str(o,"url");if(!web_url(url))return;
        p->delegate=num(o,"delegate")!=0;current=p;
        p->panel_title=str(o,"panelTitle");
        set_user_agent(p,str(o,"agent"));
        gtk_stack_set_visible_child(GTK_STACK(stack),GTK_WIDGET(p->view));controls();
        gtk_widget_grab_focus(GTK_WIDGET(p->view));
        auto request=webkit_uri_request_new(url);
        if(json_object_has_member(o,"headers") && JSON_NODE_HOLDS_OBJECT(json_object_get_member(o,"headers"))) {
            auto fields=json_object_get_object_member(o,"headers");auto names=json_object_get_members(fields);
            auto headers=webkit_uri_request_get_http_headers(request);unsigned count=0;
            for(auto item=names;item && headers && count++<128;item=item->next) {
                const char *name=(const char*)item->data,*value=str(fields,name);
                if(request_header(name,value))soup_message_headers_replace(headers,name,value);
            }
            g_list_free(names);
        }
        webkit_web_view_load_request(p->view,request);g_object_unref(request);
    } else if(!strcmp(op,"user-agent")) {
        set_user_agent(p,str(o,"agent"));
    } else if(!strcmp(op,"eval")) {
        webkit_web_view_evaluate_javascript(p->view,str(o,"script"),-1,nullptr,nullptr,nullptr,evaluated,new Reply{num(o,"request"),p});
    } else if(!strcmp(op,"back"))webkit_web_view_go_back(p->view);
    else if(!strcmp(op,"forward"))webkit_web_view_go_forward(p->view);
    else if(!strcmp(op,"reload"))webkit_web_view_reload(p->view);
    else if(!strcmp(op,"stop"))webkit_web_view_stop_loading(p->view);
    else if(!strcmp(op,"handler")) {
        const char *name=str(o,"name");if(!*name || strlen(name)>128)return;
        auto manager=webkit_web_view_get_user_content_manager(p->view);
        if(webkit_user_content_manager_register_script_message_handler(manager,name)) {
            auto m=new Message{p,name};std::string signal="script-message-received::"+m->name;
            g_signal_connect_data(manager,signal.c_str(),G_CALLBACK(message),m,[](gpointer d,GClosure*){delete (Message*)d;},GConnectFlags(0));
        }
    } else if(!strcmp(op,"script")) {
        auto script=webkit_user_script_new(str(o,"script"),num(o,"mainOnly")?WEBKIT_USER_CONTENT_INJECT_TOP_FRAME:WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
            num(o,"atEnd")?WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END:WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,nullptr,nullptr);
        webkit_user_content_manager_add_script(webkit_web_view_get_user_content_manager(p->view),script);webkit_user_script_unref(script);
    }
}
static gboolean incoming(gint fd,GIOCondition condition,gpointer data) {
    auto p=(Peer*)data;char buf[8192];ssize_t n;
    while((n=recv(fd,buf,sizeof(buf),MSG_DONTWAIT))>0) {
        p->input.append(buf,n);if(p->input.size()>1024*1024)break;
        size_t end;
        while((end=p->input.find('\n'))!=std::string::npos) {
            std::string line=p->input.substr(0,end);p->input.erase(0,end+1);
            auto parser=json_parser_new();if(json_parser_load_from_data(parser,line.data(),line.size(),nullptr)) {
                auto root=json_parser_get_root(parser);if(JSON_NODE_HOLDS_OBJECT(root))handle(json_node_get_object(root));
            }g_object_unref(parser);
        }
    }
    if(n==0 || (condition&(G_IO_HUP|G_IO_ERR)) || p->input.size()>1024*1024) {
        for(auto [id,d]:decisions){webkit_policy_decision_ignore(d);g_object_unref(d);}decisions.clear();
        if(p->write_watch)g_source_remove(p->write_watch);
        close(fd);if(peer==p)peer=nullptr;delete p;return G_SOURCE_REMOVE;
    }return G_SOURCE_CONTINUE;
}
static gboolean accept_peer(gint fd,GIOCondition,gpointer) {
    int client=accept4(fd,nullptr,nullptr,SOCK_NONBLOCK|SOCK_CLOEXEC);
    if(client<0)return G_SOURCE_CONTINUE;
    if(peer){close(client);return G_SOURCE_CONTINUE;}
    peer=new Peer{client,{}, {}};g_unix_fd_add(client,GIOCondition(G_IO_IN|G_IO_HUP|G_IO_ERR),incoming,peer);return G_SOURCE_CONTINUE;
}
static gboolean closed(GtkWidget*,GdkEvent*,gpointer) {
    if(native_wayland){request_close();return TRUE;}
    if(guest_xid) {
        Display *d=gdk_x11_display_get_xdisplay(gdk_display_get_default());XEvent event{};
        event.xclient.type=ClientMessage;event.xclient.window=guest_xid;event.xclient.format=32;
        event.xclient.message_type=XInternAtom(d,"WM_PROTOCOLS",False);
        event.xclient.data.l[0]=XInternAtom(d,"WM_DELETE_WINDOW",False);event.xclient.data.l[1]=CurrentTime;
        XSendEvent(d,guest_xid,False,NoEventMask,&event);XFlush(d);
    } else if(guest)g_subprocess_send_signal(guest,SIGTERM);
    return TRUE;
}
static void guest_done(GObject *object,GAsyncResult *result,gpointer) {
    g_subprocess_wait_finish(G_SUBPROCESS(object),result,nullptr);
    exit_status=g_subprocess_get_if_exited(guest)?g_subprocess_get_exit_status(guest):1;gtk_main_quit();
}
int main(int argc,char **argv) {
    if(argc<2 || !g_getenv("ROBLOX_MAC_STAGE") || !g_getenv("ROBLOX_MAC_DATA"))return 2;
    // The current Roblox surface is X11. GTK/WebKit use that same display for embedding.
    browser_data=g_getenv("ROBLOX_MAC_BROWSER_DATA")?g_getenv("ROBLOX_MAC_BROWSER_DATA"):std::string(g_getenv("ROBLOX_MAC_DATA"))+"/browser";
    std::string cache=browser_data+"/system-cache";
    g_mkdir_with_parents(cache.c_str(),0700);g_setenv("XDG_CACHE_HOME",cache.c_str(),TRUE);
    g_setenv("GST_REGISTRY_1_0",(cache+"/gstreamer-registry.bin").c_str(),TRUE);
    std::string temp=browser_data+"/tmp";g_mkdir_with_parents(temp.c_str(),0700);g_setenv("TMPDIR",temp.c_str(),TRUE);
    // WebKit GBM buffers fail on this NVIDIA/X11 path; keep the game GPU path intact.
    g_setenv("WEBKIT_DISABLE_DMABUF_RENDERER","1",FALSE);
    native_wayland=g_getenv("ROBLOX_MAC_WAYLAND") && !strcmp(g_getenv("ROBLOX_MAC_WAYLAND"),"1");
    if(native_wayland)g_setenv("GDK_BACKEND","wayland",TRUE);
    gdk_set_allowed_backends(native_wayland?"wayland":"x11");g_set_prgname("roblox-mac");g_set_application_name("Roblox");
    if(!gtk_init_check(nullptr,nullptr)){g_printerr("roblox-mac: GTK could not open the display\n");return 1;}
    if(native_wayland)g_printerr("WAYLAND GTK initialized\n");
    auto settings=webkit_settings_new();
    g_setenv("ROBLOX_MAC_WEBKIT_USER_AGENT",webkit_settings_get_user_agent(settings),TRUE);g_object_unref(settings);
    if(native_wayland)g_printerr("WAYLAND WebKit settings initialized\n");
    window=gtk_window_new(GTK_WINDOW_TOPLEVEL);gtk_window_set_title(GTK_WINDOW(window),"Roblox");gtk_window_set_default_size(GTK_WINDOW(window),1100,800);
    header=gtk_header_bar_new();gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header),!native_wayland);gtk_window_set_titlebar(GTK_WINDOW(window),header);
    // The host's Glycin image-loader subprocess hangs inside the Darwin process
    // during symbolic-icon decoding. GTK text buttons need no image loader;
    // keep WebKit's normal image handling and sandbox unchanged.
    back=native_wayland?gtk_button_new_with_label("←"):gtk_button_new_from_icon_name("go-previous-symbolic",GTK_ICON_SIZE_BUTTON);
    forward=native_wayland?gtk_button_new_with_label("→"):gtk_button_new_from_icon_name("go-next-symbolic",GTK_ICON_SIZE_BUTTON);
    reload=native_wayland?gtk_button_new_with_label("↻"):gtk_button_new_from_icon_name("view-refresh-symbolic",GTK_ICON_SIZE_BUTTON);done=gtk_button_new_with_label("Back to Roblox");
    if(native_wayland) {
        auto close=gtk_button_new_with_label("×"),maximize=gtk_button_new_with_label("□"),minimize=gtk_button_new_with_label("−");
        for(auto item:{std::pair{close,"Close"},std::pair{maximize,"Maximize or restore"},std::pair{minimize,"Minimize"}}) {
            gtk_widget_set_tooltip_text(item.first,item.second);atk_object_set_name(gtk_widget_get_accessible(item.first),item.second);
            gtk_widget_set_can_focus(item.first,FALSE);gtk_header_bar_pack_end(GTK_HEADER_BAR(header),item.first);
        }
        g_signal_connect(close,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){request_close();}),nullptr);
        g_signal_connect(maximize,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){if(gtk_window_is_maximized(GTK_WINDOW(window)))gtk_window_unmaximize(GTK_WINDOW(window));else gtk_window_maximize(GTK_WINDOW(window));}),nullptr);
        g_signal_connect(minimize,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){gtk_window_iconify(GTK_WINDOW(window));}),nullptr);
    }
    for(auto w:{back,forward,reload})gtk_header_bar_pack_start(GTK_HEADER_BAR(header),w);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(header),done);
    gtk_widget_set_tooltip_text(back,"Back");gtk_widget_set_tooltip_text(forward,"Forward");gtk_widget_set_tooltip_text(reload,"Reload");
    g_signal_connect(back,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){if(current)webkit_web_view_go_back(current->view);}),nullptr);
    g_signal_connect(forward,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){if(current)webkit_web_view_go_forward(current->view);}),nullptr);
    g_signal_connect(reload,"clicked",G_CALLBACK(+[](GtkWidget*,gpointer){if(current)webkit_web_view_reload(current->view);}),nullptr);
    g_signal_connect(done,"clicked",G_CALLBACK(return_to_game),nullptr);
    stack=gtk_stack_new();socket_view=native_wayland?gtk_drawing_area_new():gtk_socket_new();gtk_stack_add_named(GTK_STACK(stack),socket_view,"game");gtk_container_add(GTK_CONTAINER(window),stack);
    if(!native_wayland)g_signal_connect(socket_view,"plug-removed",G_CALLBACK(+[](GtkSocket*,gpointer)->gboolean{return TRUE;}),nullptr);
    g_signal_connect(window,"delete-event",G_CALLBACK(closed),nullptr);
    if(native_wayland)g_printerr("WAYLAND widgets initialized\n");
    gtk_widget_realize(window);gtk_widget_realize(socket_view);controls();
    if(native_wayland)g_printerr("WAYLAND widgets realized\n");
    socket_path=std::string(g_getenv("ROBLOX_MAC_STAGE"))+"/ui.sock";sockaddr_un address{};address.sun_family=AF_UNIX;
    if(socket_path.size()+strlen("/Volumes/SystemRoot")>=sizeof(address.sun_path)){g_printerr("roblox-mac: UI socket path too long\n");return 1;}
    strcpy(address.sun_path,socket_path.c_str());listener=socket(AF_UNIX,SOCK_STREAM|SOCK_NONBLOCK|SOCK_CLOEXEC,0);
    if(listener<0 || bind(listener,(sockaddr*)&address,sizeof(address)) || listen(listener,4)){g_printerr("roblox-mac: UI socket creation failed\n");return 1;}
    chmod(socket_path.c_str(),0600);g_unix_fd_add(listener,G_IO_IN,accept_peer,nullptr);
    std::string guest_path="/Volumes/SystemRoot"+socket_path;g_setenv("ROBLOX_MAC_UI_SOCKET",guest_path.c_str(),TRUE);
#ifdef RBX_WAYLAND_LIBRARY
    g_printerr("WAYLAND IPC ready\n");gtk_widget_show_all(window);g_printerr("WAYLAND window shown\n");controls();return 0;
#endif
    GError *error=nullptr;auto launcher=g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_NONE);
    const char *original=g_getenv("ROBLOX_MAC_ORIGINAL_LD_LIBRARY_PATH");
    if(original && *original)g_subprocess_launcher_setenv(launcher,"LD_LIBRARY_PATH",original,TRUE);
    else g_subprocess_launcher_unsetenv(launcher,"LD_LIBRARY_PATH");
    for(auto key:{"GIO_MODULE_DIR","GSETTINGS_SCHEMA_DIR","WEBKIT_INJECTED_BUNDLE_PATH"})g_subprocess_launcher_unsetenv(launcher,key);
    guest=g_subprocess_launcher_spawnv(launcher,(const gchar*const*)(argv+1),&error);g_object_unref(launcher);
    if(!guest){g_printerr("roblox-mac: client launch failed\n");unlink(socket_path.c_str());return 1;}
    g_subprocess_wait_async(guest,nullptr,guest_done,nullptr);gtk_main();
    for(auto [id,d]:decisions){webkit_policy_decision_ignore(d);g_object_unref(d);}
    gtk_widget_destroy(window);close(listener);unlink(socket_path.c_str());g_object_unref(guest);return exit_status;
}
