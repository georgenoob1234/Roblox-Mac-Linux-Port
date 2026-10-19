/* lstat(2), O_NOFOLLOW and O_CLOEXEC are exposed by _GNU_SOURCE; the UI build
 * compiles with -std=c11, which otherwise hides them. */
#define _GNU_SOURCE

#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>

#define UI_DELAY_MS 450
#define DIALOG_TIMEOUT_SECONDS 30
#define CANCEL_POLL_MS 250

static const char *UI_CSS =
    "window { background-color: @theme_bg_color; color: @theme_fg_color; }"
    ".sidebar { background-color: @theme_base_color; padding: 18px 12px; }"
    ".brand { font-weight: 700; font-size: 15px; padding: 4px 8px 18px; }"
    ".nav { min-height: 38px; border-radius: 9px; padding: 7px 12px; margin: 2px 0; }"
    ".nav:checked { background-color: @theme_selected_bg_color; color: @theme_fg_color; }"
    ".content { padding: 30px 42px; }"
    ".title { font-size: 25px; font-weight: 700; padding: 5px 0; }"
    ".section-title { font-size: 18px; font-weight: 700; padding: 4px 0; }"
    ".subtitle { font-size: 14px; color: @theme_unfocused_fg_color; }"
    ".update-header { background-color: @theme_base_color; border-bottom: 1px solid @theme_unfocused_fg_color; padding: 22px 42px 18px; }"
    ".update-value { color: @theme_fg_color; font-size: 14px; }"
    ".status { color: @theme_fg_color; font-weight: 700; padding: 5px 0; }"
    ".accent { background-color: @theme_selected_bg_color; color: @theme_fg_color; border-radius: 10px; padding: 9px 20px; font-weight: 700; }"
    ".muted { color: @theme_unfocused_fg_color; }"
    "progressbar trough { min-height: 10px; border-radius: 6px; }"
    "progressbar progress { min-height: 10px; border-radius: 6px; }";

static void apply_css(void) {
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider, UI_CSS, -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

typedef struct {
    char *root, *cancel_file, *cancel_state;
    GtkWidget *window, *phase, *status, *progress, *cancel;
    guint show_id, close_id, cancel_poll_id;
    gboolean done, cancellable, cancelled, closed, authoritative;
    int result;
} ProgressState;

typedef struct {
    char *root;
    GtkWidget *window, *stack, *play_version, *play_status;
    GtkWidget *settings_current, *settings_latest, *settings_checked, *settings_skipped, *info_client;
    GtkWidget *mode, *interval, *update, *message;
    GtkWidget *port_current, *port_latest, *port_checked, *port_check, *port_update, *port_message;
    GtkWidget *decorations_combo, *cursor_combo, *appearance_hint;
    GtkWidget *check;
    GSubprocess *check_process, *port_process;
    GPid launch_pid;
    guint launch_check_id;
    gboolean running, update_available, check_in_progress, handoff;
    gboolean port_update_available, port_check_in_progress, port_handoff;
} LauncherState;

typedef struct { GtkWidget *window; int result; } DialogState;

static gchar **host_environment(void) {
    gchar **environment = g_get_environ();
    const char *paths[] = {"LD_LIBRARY_PATH", "GIO_MODULE_DIR", "GDK_PIXBUF_MODULE_FILE",
        "GSETTINGS_SCHEMA_DIR", "WEBKIT_INJECTED_BUNDLE_PATH", "GST_PLUGIN_SYSTEM_PATH_1_0",
        "GST_PLUGIN_PATH_1_0", "GST_PLUGIN_SCANNER_1_0", "APPDIR", "APPIMAGE",
        "APPIMAGE_EXTRACT_AND_RUN", "APPIMAGE_COMMAND", "APPIMAGE_SILENT_INSTALL", NULL};
    for (const char **path = paths; *path; ++path)
        environment = g_environ_unsetenv(environment, *path);
    return environment;
}

static gboolean spawn_sync(char **argv, char **out, int *status) {
    GError *error = NULL;
    gchar **environment = host_environment();
    gboolean ok = g_spawn_sync(NULL, argv, environment, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, out, NULL, status, &error);
    g_strfreev(environment);
    if (!ok) { g_clear_error(&error); if (out) *out = NULL; if (status) *status = -1; }
    return ok;
}

static gboolean run_release(const char *root, const char *command, const char *key,
                            const char *value, char **out) {
    char *argv[7] = {(char *)"sh", NULL, NULL, NULL, NULL, NULL, NULL};
    int status = -1;
    argv[1] = g_build_filename(root, "run.sh", NULL); argv[2] = (char *)command;
    int n = 3; if (key) { argv[n++] = (char *)key; if (value) argv[n++] = (char *)value; }
    argv[n] = NULL; gboolean ok = spawn_sync(argv, out, &status) && status == 0;
    g_free(argv[1]); return ok;
}

static JsonObject *parse_json(const char *text) {
    if (!text) return NULL;
    JsonParser *parser = json_parser_new(); GError *error = NULL;
    if (!json_parser_load_from_data(parser, text, -1, &error)) {
        g_clear_error(&error); g_object_unref(parser); return NULL;
    }
    JsonNode *node = json_parser_get_root(parser);
    JsonObject *object = JSON_NODE_HOLDS_OBJECT(node) ? json_node_get_object(node) : NULL;
    if (object) json_object_ref(object);
    g_object_unref(parser);
    return object;
}
static const char *json_string(JsonObject *object, const char *name) {
    return object && json_object_has_member(object, name) && JSON_NODE_HOLDS_VALUE(json_object_get_member(object, name))
        ? json_object_get_string_member(object, name) : "";
}
static gboolean json_bool(JsonObject *object, const char *name) { return object && json_object_has_member(object, name) && json_object_get_boolean_member(object, name); }
static gint64 json_int(JsonObject *object, const char *name) { return object && json_object_has_member(object, name) ? json_object_get_int_member(object, name) : 0; }
static void label_text(GtkWidget *widget, const char *text) { gtk_label_set_text(GTK_LABEL(widget), text ? text : ""); }
static GtkWidget *label(const char *text, const char *class_name) {
    GtkWidget *widget = gtk_label_new(text); gtk_widget_set_halign(widget, GTK_ALIGN_START);
    if (class_name) gtk_style_context_add_class(gtk_widget_get_style_context(widget), class_name);
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE); return widget;
}

static void progress_close(ProgressState *state, int result) {
    if (state->closed) return;
    state->closed = TRUE; state->result = result;
    if (state->show_id) { g_source_remove(state->show_id); state->show_id = 0; }
    if (state->close_id) { g_source_remove(state->close_id); state->close_id = 0; }
    if (state->cancel_poll_id) { g_source_remove(state->cancel_poll_id); state->cancel_poll_id = 0; }
    if (state->window) gtk_widget_destroy(state->window);
    gtk_main_quit();
}
static gboolean progress_hide(gpointer data) { ProgressState *state = data; progress_close(state, state->result); return G_SOURCE_REMOVE; }
/* Shared cancellation handshake. The launcher writes "open" before an
 * operation and the installer flips it to "committing" at the commit boundary,
 * both under <state>.lock. The UI takes the same lock around its
 * check-and-request, so it can never record -- and therefore never show -- an
 * accepted cancellation after the commit has begun. The protocol fails closed:
 * an unusable lock or an unreadable, unsafe, or malformed state is never
 * mistaken for a safe "open". Only a genuinely absent protocol path (no
 * launcher protocol at all) keeps the marker-only contract. */
typedef enum { CANCEL_STATE_OPEN, CANCEL_STATE_BUSY, CANCEL_STATE_UNKNOWN } CancelState;
enum { CANCEL_ACCEPTED, CANCEL_REFUSED, CANCEL_UNCERTAIN };

static CancelState cancel_state_read(const char *state_path) {
    if (!state_path || !*state_path) return CANCEL_STATE_OPEN;
    struct stat st;
    if (lstat(state_path, &st) != 0) return CANCEL_STATE_UNKNOWN;
    if (S_ISLNK(st.st_mode) || !S_ISREG(st.st_mode)) return CANCEL_STATE_UNKNOWN;
    if (st.st_uid != getuid()) return CANCEL_STATE_UNKNOWN;
    int fd = open(state_path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return CANCEL_STATE_UNKNOWN;
    char buffer[64];
    ssize_t n = read(fd, buffer, sizeof buffer - 1);
    close(fd);
    if (n <= 0) return CANCEL_STATE_UNKNOWN;
    buffer[n] = '\0';
    char *value = g_strstrip(buffer);
    if (!g_strcmp0(value, "open")) return CANCEL_STATE_OPEN;
    if (!g_strcmp0(value, "committing") || !g_strcmp0(value, "committed") ||
        !g_strcmp0(value, "cancelled")) return CANCEL_STATE_BUSY;
    return CANCEL_STATE_UNKNOWN;
}
static int cancel_state_lock_fd(const char *state_path) {
    if (!state_path || !*state_path) return -1;
    char *path = g_strdup_printf("%s.lock", state_path);
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd >= 0) {
        struct stat st;
        if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) { close(fd); fd = -1; }
    }
    g_free(path);
    return fd;
}
/* Record a cancellation request only while the state is still "open", under the
 * shared lock. Returns CANCEL_ACCEPTED only when the lock was taken, a valid
 * "open" state was read, and the marker was written; CANCEL_REFUSED when the
 * commit already owns the transition; CANCEL_UNCERTAIN when the lock or state
 * could not be used safely, so the caller never claims the request was accepted
 * and never writes a marker on an unverifiable state. */
static int progress_try_request_cancel(ProgressState *state) {
    if (!state->cancel_state || !*state->cancel_state)
        return g_file_set_contents(state->cancel_file, "cancel\n", -1, NULL) ? CANCEL_ACCEPTED : CANCEL_UNCERTAIN;
    int fd = cancel_state_lock_fd(state->cancel_state);
    if (fd < 0) return CANCEL_UNCERTAIN;
    int result = CANCEL_UNCERTAIN;
    if (flock(fd, LOCK_EX) == 0) {
        CancelState current = cancel_state_read(state->cancel_state);
        if (current == CANCEL_STATE_OPEN)
            result = g_file_set_contents(state->cancel_file, "cancel\n", -1, NULL) ? CANCEL_ACCEPTED : CANCEL_UNCERTAIN;
        else if (current == CANCEL_STATE_BUSY)
            result = CANCEL_REFUSED;
        flock(fd, LOCK_UN);
    }
    close(fd);
    return result;
}
static void progress_request_cancel(ProgressState *state, GtkWidget *button) {
    if (!state->cancellable || state->cancelled) return;
    int result = progress_try_request_cancel(state);
    if (result == CANCEL_ACCEPTED) {
        state->cancelled = TRUE; state->cancellable = FALSE;
        if (button) { gtk_button_set_label(GTK_BUTTON(button), "Cancelling…"); gtk_widget_set_sensitive(button, FALSE); }
        label_text(state->status, "Cancelling…");
        return;
    }
    /* Never claim the request was accepted unless it was recorded under a valid
     * "open" state. Disable further attempts and report the real outcome. */
    state->cancellable = FALSE;
    if (button) gtk_widget_set_sensitive(button, FALSE);
    if (result == CANCEL_REFUSED)
        label_text(state->status, "This update is already being applied and can no longer be cancelled.");
    else
        label_text(state->status, "Cancellation is unavailable; the update status could not be verified safely.");
}
/* The state file is authoritative, so poll it instead of trusting that the
 * non-cancellable progress event has already been processed: the commit may
 * have begun while that event is still queued. An unreadable or invalid state
 * is never treated as proof that cancellation is still safe. */
static gboolean progress_poll_cancel(gpointer data) {
    ProgressState *state = data;
    if (state->closed || state->done) return G_SOURCE_REMOVE;
    if (state->cancel && !state->cancelled && state->cancellable) {
        CancelState current = cancel_state_read(state->cancel_state);
        if (current != CANCEL_STATE_OPEN) {
            state->cancellable = FALSE;
            gtk_widget_set_sensitive(state->cancel, FALSE);
            if (current == CANCEL_STATE_UNKNOWN)
                label_text(state->status, "Cancellation is unavailable; the update status could not be verified safely.");
        }
    }
    return G_SOURCE_CONTINUE;
}
static void progress_cancel(GtkButton *button, gpointer data) { progress_request_cancel(data, GTK_WIDGET(button)); }
static gboolean progress_delete(GtkWidget *window, GdkEvent *event, gpointer data) {
    ProgressState *state = data;
    if (state->cancellable && !state->cancelled)
        progress_request_cancel(state, state->cancel);
    else if (!state->cancelled)
        label_text(state->status, "Please wait; this phase cannot be cancelled safely.");
    return TRUE;
}
static char *find_icon_path(const char *root) {
    const char *env_icon = g_getenv("ROBLOX_MAC_ICON_PATH");
    if (env_icon && *env_icon && g_file_test(env_icon, G_FILE_TEST_IS_REGULAR))
        return g_strdup(env_icon);

    if (root && *root) {
        char *p = g_build_filename(root, "icon.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);

        p = g_build_filename(root, "roblox-mac-port.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);

        p = g_build_filename(root, "..", "icon.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);
    }

    const char *appdir = g_getenv("APPDIR");
    if (appdir && *appdir) {
        char *p = g_build_filename(appdir, "roblox-mac-port.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);

        p = g_build_filename(appdir, "icon.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);

        p = g_build_filename(appdir, "usr", "share", "icons", "hicolor", "256x256", "apps", "roblox-mac-port.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);
    }

    const char *data_dir = g_get_user_data_dir();
    if (data_dir && *data_dir) {
        char *p = g_build_filename(data_dir, "icons", "hicolor", "256x256", "apps", "roblox-mac-port.png", NULL);
        if (g_file_test(p, G_FILE_TEST_IS_REGULAR)) return p;
        g_free(p);
    }

    return NULL;
}

static void setup_ui_identity(GtkWidget *win, const char *root) {
    gdk_set_program_class("roblox-mac-port");
    gtk_window_set_default_icon_name("roblox-mac-port");
    if (!win) return;
    gtk_window_set_icon_name(GTK_WINDOW(win), "roblox-mac-port");
    char *icon = find_icon_path(root);
    if (icon) {
        GError *error = NULL;
        GdkPixbuf *pb = gdk_pixbuf_new_from_file(icon, &error);
        if (pb) {
            gtk_window_set_icon(GTK_WINDOW(win), pb);
            g_object_unref(pb);
        }
        g_clear_error(&error);
        g_free(icon);
    }
}

static gboolean progress_show(gpointer data) {
    ProgressState *state = data; state->show_id = 0; if (state->done) return G_SOURCE_REMOVE;
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(state->window), "Roblox");
    setup_ui_identity(state->window, state->root);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 460, 170); gtk_window_set_resizable(GTK_WINDOW(state->window), FALSE); gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER);
    g_signal_connect(state->window, "delete-event", G_CALLBACK(progress_delete), state);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14); gtk_container_set_border_width(GTK_CONTAINER(box), 24); gtk_container_add(GTK_CONTAINER(state->window), box);
    state->phase = label("Preparing", "subtitle"); gtk_box_pack_start(GTK_BOX(box), state->phase, FALSE, FALSE, 0);
    state->status = label("Setting up Roblox…", NULL); gtk_box_pack_start(GTK_BOX(box), state->status, FALSE, FALSE, 0);
    state->progress = gtk_progress_bar_new(); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progress), TRUE); gtk_box_pack_start(GTK_BOX(box), state->progress, FALSE, FALSE, 0);
    state->cancel = gtk_button_new_with_mnemonic("_Cancel"); gtk_widget_set_halign(state->cancel, GTK_ALIGN_END); g_signal_connect(state->cancel, "clicked", G_CALLBACK(progress_cancel), state); gtk_box_pack_start(GTK_BOX(box), state->cancel, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(state->cancel, FALSE); gtk_widget_show_all(state->window);
    state->cancel_poll_id = g_timeout_add(CANCEL_POLL_MS, progress_poll_cancel, state);
    return G_SOURCE_REMOVE;
}
static void progress_event(ProgressState *state, const char *line) {
    JsonObject *object = parse_json(line); if (!object) return;
    const char *message = json_string(object, "message"); const char *phase = json_string(object, "phase"); gint64 percent = json_int(object, "percent");
    gint64 done = json_int(object, "bytes_done"); gint64 total = json_int(object, "bytes_total"); state->cancellable = json_bool(object, "cancellable");
    if (state->phase && *phase) { char *text = g_strdup_printf("Phase: %s", phase); label_text(state->phase, text); g_free(text); }
    if (state->status && *message) label_text(state->status, message);
    if (state->cancel && !state->cancelled) gtk_widget_set_sensitive(state->cancel, state->cancellable);
    if (state->progress) {
        if (percent >= 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), percent / 100.0);
        else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->progress));
        if (total > 0) { char *text = g_strdup_printf("%lld / %lld bytes", (long long)done, (long long)total); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), text); g_free(text); }
        else if (percent >= 0) { char *text = g_strdup_printf("%lld%%", (long long)percent); gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), text); g_free(text); }
        else gtk_progress_bar_set_text(GTK_PROGRESS_BAR(state->progress), "Working…");
    }
    if (!g_strcmp0(phase, "launching") || !g_strcmp0(phase, "update") ||
        !g_strcmp0(phase, "cancelled") || !g_strcmp0(phase, "failed")) {
        state->done = TRUE;
        /* The producer owns the outcome. A late or rejected cancellation
         * request must not turn a reported success into a local "cancelled". */
        state->authoritative = TRUE;
        state->result = !g_strcmp0(phase, "failed") ? 1 : (!g_strcmp0(phase, "cancelled") ? 4 : 0);
        if (state->window && !state->close_id) state->close_id = g_timeout_add(350, progress_hide, state);
        else if (!state->window) progress_close(state, state->result);
    }
    json_object_unref(object);
}
static gboolean progress_input(GIOChannel *channel, GIOCondition condition, gpointer data) {
    ProgressState *state = data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        if (state->done) return G_SOURCE_REMOVE;
        progress_close(state, state->cancelled ? 4 : state->result);
        return G_SOURCE_REMOVE;
    }
    gchar *line = NULL; gsize length = 0; GError *error = NULL; GIOStatus result = g_io_channel_read_line(channel, &line, &length, NULL, &error); g_clear_error(&error);
    if (result == G_IO_STATUS_NORMAL) { progress_event(state, line); g_free(line); return state->closed ? G_SOURCE_REMOVE : G_SOURCE_CONTINUE; }
    g_free(line);
    if (state->done) return G_SOURCE_REMOVE;
    progress_close(state, state->cancelled ? 4 : state->result);
    return G_SOURCE_REMOVE;
}
static int run_progress(const char *root) {
    if (!gtk_init_check(NULL, NULL)) return 3;
    apply_css();
    ProgressState *state = g_new0(ProgressState, 1); state->result = 1; state->root = g_strdup(root); state->cancel_file = g_build_filename(root, ".bootstrap-cancel", NULL); state->cancel_state = g_build_filename(root, ".bootstrap-cancel-state", NULL);
    state->show_id = g_timeout_add(UI_DELAY_MS, progress_show, state); GIOChannel *channel = g_io_channel_unix_new(STDIN_FILENO); g_io_channel_set_encoding(channel, NULL, NULL); g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL); g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, progress_input, state); gtk_main();
    int result = state->authoritative ? state->result : (state->cancelled ? 4 : state->result); g_io_channel_unref(channel); g_free(state->root); g_free(state->cancel_file); g_free(state->cancel_state); g_free(state); return result;
}

static void dialog_finish(DialogState *state, int result) { state->result = result; gtk_main_quit(); }
static gboolean dialog_timeout(gpointer data) { dialog_finish(data, 1); return G_SOURCE_REMOVE; }
static void dialog_button(GtkWidget *button, gpointer data) { dialog_finish(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "result"))); }
static int run_dialog(const char *latest, const char *installed) {
    if (!gtk_init_check(NULL, NULL)) return 3;
    apply_css(); DialogState state = {0}; state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(state.window), "Roblox update available"); setup_ui_identity(state.window, NULL); gtk_window_set_default_size(GTK_WINDOW(state.window), 500, 270); gtk_window_set_resizable(GTK_WINDOW(state.window), FALSE); gtk_window_set_modal(GTK_WINDOW(state.window), TRUE); gtk_window_set_position(GTK_WINDOW(state.window), GTK_WIN_POS_CENTER); g_signal_connect(state.window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14); gtk_container_set_border_width(GTK_CONTAINER(box), 26); gtk_container_add(GTK_CONTAINER(state.window), box); gtk_box_pack_start(GTK_BOX(box), label("A Roblox update is ready", "title"), FALSE, FALSE, 0); char *versions = g_strdup_printf("Installed: %s\nNew version: %s", *installed ? installed : "none", *latest ? latest : "available"); gtk_box_pack_start(GTK_BOX(box), label(versions, NULL), FALSE, FALSE, 0); g_free(versions); gtk_box_pack_start(GTK_BOX(box), label("The prompt closes after 30 seconds and launches without updating.", "muted"), FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL); gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END); gtk_box_set_spacing(GTK_BOX(buttons), 8); const char *names[] = {"Update now", "Launch without updating", "Skip this version"}; for (int i = 0; i < 3; ++i) { GtkWidget *button = gtk_button_new_with_mnemonic(names[i]); g_object_set_data(G_OBJECT(button), "result", GINT_TO_POINTER(i == 0 ? 0 : (i == 1 ? 1 : 2))); g_signal_connect(button, "clicked", G_CALLBACK(dialog_button), &state); gtk_container_add(GTK_CONTAINER(buttons), button); if (i == 1) gtk_widget_grab_focus(button); } gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0); gtk_widget_show_all(state.window); g_timeout_add_seconds(DIALOG_TIMEOUT_SECONDS, dialog_timeout, &state); gtk_main(); gtk_widget_destroy(state.window); return state.result;
}

static JsonObject *settings_status(LauncherState *state) { char *output = NULL; run_release(state->root, "status", NULL, NULL, &output); JsonObject *object = parse_json(output); g_free(output); return object; }
static void settings_message(LauncherState *state, const char *message) { label_text(state->message, message); }
static void set_prefixed(GtkWidget *widget, const char *prefix, const char *value) { char *text = g_strdup_printf("%s%s", prefix, value && *value ? value : "—"); label_text(widget, text); g_free(text); }
static void settings_refresh(LauncherState *state) {
    JsonObject *object = settings_status(state); if (!object) { state->update_available = FALSE; if (state->update) gtk_widget_set_sensitive(state->update, FALSE); settings_message(state, "Status unavailable; check again before updating."); return; }
    JsonObject *settings = json_object_has_member(object, "settings") ? json_object_get_object_member(object, "settings") : NULL; const char *mode = settings ? json_string(settings, "auto_update") : "ask"; const char *current = json_string(object, "installed_display_version"); const char *latest_raw = json_string(object, "latest_known"); const char *latest_display = json_string(object, "latest_display_version"); const char *installed_raw = json_string(object, "installed_version");
    state->update_available = json_object_has_member(object, "update_available") ? json_bool(object, "update_available") : (*latest_raw && g_strcmp0(latest_raw, installed_raw)); state->running = json_bool(object, "game_running"); gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->mode), mode); gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->interval), settings ? json_int(settings, "check_interval_hours") : 24); set_prefixed(state->settings_current, "Current version: ", current); set_prefixed(state->settings_latest, "Latest known: ", state->update_available ? (*latest_display ? latest_display : "Update available") : (*latest_display ? latest_display : current));
    char checked[64] = "—"; gint64 timestamp = settings ? json_int(settings, "last_check") : 0; if (timestamp > 0) { GDateTime *date = g_date_time_new_from_unix_local(timestamp); gchar *formatted = g_date_time_format(date, "%Y-%m-%d %H:%M"); g_strlcpy(checked, formatted, sizeof checked); g_free(formatted); g_date_time_unref(date); } set_prefixed(state->settings_checked, "Last checked: ", checked); set_prefixed(state->settings_skipped, "Skipped version: ", settings ? json_string(settings, "skipped_version") : ""); if (state->play_version) set_prefixed(state->play_version, "Client version: ", current); if (state->info_client) set_prefixed(state->info_client, "Client version: ", current); if (state->play_status) { char *status = state->update_available ? g_strdup_printf("Update available: %s", *latest_display ? latest_display : "new client") : g_strdup("Up to date"); label_text(state->play_status, status); g_free(status); }
    const char *dec = settings ? json_string(settings, "window_decoration") : "auto"; const char *cur = settings ? json_string(settings, "cursor_appearance") : "system";
    if (!*dec) dec = "auto";
    if (!*cur) cur = "system";
    if (state->decorations_combo) gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->decorations_combo), dec);
    if (state->cursor_combo) gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->cursor_combo), cur);
    gtk_widget_set_sensitive(state->update, !state->check_in_progress && !state->running && state->update_available); gtk_widget_set_tooltip_text(state->update, state->running ? "Close Roblox before updating." : (state->update_available ? "Update the installed client now." : "Already up to date.")); if (state->check) gtk_widget_set_sensitive(state->check, !state->check_in_progress); if (!state->check_in_progress && !state->handoff) { if (state->running) settings_message(state, "Roblox is running; Update now is disabled until it closes."); else if (state->update_available) settings_message(state, "An update is available."); else settings_message(state, "Your installed client is up to date."); }
    char *port_version = NULL;
    run_release(state->root, "port-version", NULL, NULL, &port_version);
    if (port_version) g_strstrip(port_version);
    const char *port_latest = settings ? json_string(settings, "port_latest") : "";
    gint64 port_checked = settings ? json_int(settings, "port_last_check") : 0;
    state->port_update_available = port_version && *port_version && *port_latest && g_strcmp0(port_version, port_latest);
    set_prefixed(state->port_current, "Installed port version: ", port_version);
    set_prefixed(state->port_latest, "Latest port version: ", port_latest);
    char port_checked_text[64] = "—";
    if (port_checked > 0) {
        GDateTime *date = g_date_time_new_from_unix_local(port_checked);
        gchar *formatted = g_date_time_format(date, "%Y-%m-%d %H:%M");
        g_strlcpy(port_checked_text, formatted, sizeof port_checked_text);
        g_free(formatted); g_date_time_unref(date);
    }
    set_prefixed(state->port_checked, "Last successful port check: ", port_checked_text);
    gtk_widget_set_sensitive(state->port_update, !state->port_check_in_progress && !state->port_handoff && !state->running && state->port_update_available);
    gtk_widget_set_tooltip_text(state->port_update, state->running ? "Close Roblox before updating the port." : (state->port_update_available ? "Install the latest port release." : "No port update is available."));
    if (state->port_check) gtk_widget_set_sensitive(state->port_check, !state->port_check_in_progress && !state->port_handoff);
    if (!state->port_check_in_progress && !state->port_handoff) {
        if (state->running) label_text(state->port_message, "Close Roblox before updating the port.");
        else if (!port_version || !*port_version) label_text(state->port_message, "Installed port version is unknown; check for a release before updating.");
        else if (state->port_update_available) label_text(state->port_message, "A newer port release is available.");
        else if (!*port_latest) label_text(state->port_message, "No successful port update check has been recorded yet.");
        else label_text(state->port_message, "The port is up to date.");
    }
    g_free(port_version);
    json_object_unref(object);
}
static void setting_set(LauncherState *state, const char *key, const char *value) { if (!run_release(state->root, "set", key, value, NULL)) settings_message(state, "Could not save this setting."); }
static void mode_changed(GtkComboBox *combo, gpointer data) { const char *id = gtk_combo_box_get_active_id(combo); if (id) setting_set(data, "auto_update", id); }
static void interval_changed(GtkSpinButton *spin, gpointer data) { char value[32]; g_snprintf(value, sizeof value, "%d", gtk_spin_button_get_value_as_int(spin)); setting_set(data, "check_interval_hours", value); }
static void decorations_changed(GtkComboBox *combo, gpointer data) { const char *id = gtk_combo_box_get_active_id(combo); if (id) { setting_set(data, "window_decoration", id); settings_message(data, "Window decorations changed. Applies on next launch."); } }
static void cursor_changed(GtkComboBox *combo, gpointer data) { const char *id = gtk_combo_box_get_active_id(combo); if (id) { setting_set(data, "cursor_appearance", id); settings_message(data, "Cursor appearance changed. Applies on next launch."); } }
static void appearance_reset(GtkButton *button, gpointer data) { LauncherState *state = data; setting_set(state, "window_decoration", "auto"); setting_set(state, "cursor_appearance", "system"); if (state->decorations_combo) gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->decorations_combo), "auto"); if (state->cursor_combo) gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->cursor_combo), "system"); settings_message(state, "Appearance settings restored to defaults. Applies on next launch."); }
static void settings_check_done(GObject *source, GAsyncResult *result, gpointer data) {
    LauncherState *state = data;
    GSubprocess *process = G_SUBPROCESS(source);
    gchar *output = NULL, *error_output = NULL;
    GError *error = NULL;
    gboolean communicated = g_subprocess_communicate_utf8_finish(process, result, &output, &error_output, &error);
    gboolean ok = communicated && g_subprocess_get_if_exited(process) && g_subprocess_get_exit_status(process) == 0;
    JsonObject *status = ok ? parse_json(output) : NULL;
    if (!status) ok = FALSE;
    if (error) g_clear_error(&error);
    g_free(output); g_free(error_output);
    state->check_in_progress = FALSE;
    state->check_process = NULL;
    g_object_unref(process);
    settings_refresh(state);
    if (ok) settings_message(state, state->update_available ? "An update is available." : "Your installed client is up to date.");
    else settings_message(state, "Could not check for updates. Check your connection and try again; the last known status is shown.");
    if (status) json_object_unref(status);
}
static void settings_check(GtkButton *button, gpointer data) {
    LauncherState *state = data;
    if (state->check_in_progress || state->handoff) return;
    char *run = g_build_filename(state->root, "run.sh", NULL);
    const gchar *argv[] = {"sh", run, "check", NULL};
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    gchar **environment = host_environment();
    g_subprocess_launcher_set_environ(launcher, environment);
    g_strfreev(environment);
    g_subprocess_launcher_setenv(launcher, "ROBLOX_MAC_NO_PROGRESS", "1", TRUE);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher); g_free(run);
    if (!process) {
        settings_message(state, error ? error->message : "Could not start the update check.");
        g_clear_error(&error);
        return;
    }
    state->check_process = process; state->check_in_progress = TRUE;
    if (state->check) gtk_widget_set_sensitive(state->check, FALSE);
    gtk_widget_set_sensitive(state->update, FALSE);
    settings_message(state, "Checking for updates…");
    g_subprocess_communicate_utf8_async(process, NULL, NULL, settings_check_done, state);
}
static void settings_update(GtkButton *button, gpointer data) {
    LauncherState *state = data;
    if (state->running || !state->update_available || state->check_in_progress || state->handoff) return;
    JsonObject *status = settings_status(state);
    if (!status || json_bool(status, "game_running") || !json_bool(status, "update_available")) {
        if (status) json_object_unref(status);
        settings_refresh(state);
        return;
    }
    json_object_unref(status);
    char *run = g_build_filename(state->root, "run.sh", NULL);
    gchar *argv[] = {(gchar *)"sh", run, (gchar *)"update-settings", NULL};
    gchar **environment = host_environment();
    environment = g_environ_unsetenv(environment, "ROBLOX_MAC_NO_PROGRESS");
    GError *error = NULL;
    gboolean spawned = g_spawn_async(state->root, argv, environment, G_SPAWN_SEARCH_PATH,
                                     NULL, NULL, NULL, &error);
    g_strfreev(environment);
    g_free(run);
    if (!spawned) {
        settings_message(state, error ? error->message : "Could not start the updater.");
        g_clear_error(&error);
        settings_refresh(state);
        return;
    }
    state->handoff = TRUE;
    gtk_widget_hide(state->window);
    gtk_widget_destroy(state->window);
}
static void settings_port_check_done(GObject *source, GAsyncResult *result, gpointer data) {
    LauncherState *state = data;
    GSubprocess *process = G_SUBPROCESS(source);
    gchar *output = NULL, *error_output = NULL;
    GError *error = NULL;
    gboolean communicated = g_subprocess_communicate_utf8_finish(process, result, &output, &error_output, &error);
    JsonObject *metadata = communicated && g_subprocess_get_if_exited(process) && g_subprocess_get_exit_status(process) == 0
        ? parse_json(output) : NULL;
    gboolean ok = metadata && *json_string(metadata, "latest_version");
    if (error) g_clear_error(&error);
    g_free(output); g_free(error_output);
    state->port_check_in_progress = FALSE;
    state->port_process = NULL;
    g_object_unref(process);
    if (ok) {
        char checked[32]; g_snprintf(checked, sizeof checked, "%lld", (long long)time(NULL));
        setting_set(state, "port_latest", json_string(metadata, "latest_version"));
        setting_set(state, "port_last_check", checked);
    }
    settings_refresh(state);
    if (ok) label_text(state->port_message, state->port_update_available ? "A newer port release is available." : "The port is up to date.");
    else label_text(state->port_message, "Could not check for port updates. Verify the network and try again.");
    if (metadata) json_object_unref(metadata);
}
static void settings_port_check(GtkButton *button, gpointer data) {
    LauncherState *state = data;
    if (state->port_check_in_progress || state->port_handoff) return;
    char *installer = g_build_filename(state->root, "installer.py", NULL);
    char *prefix = g_strdup(state->root);
    const gchar *argv[] = {"python3", installer, "release-status", "--prefix", prefix, NULL};
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE);
    gchar **environment = host_environment();
    g_subprocess_launcher_set_environ(launcher, environment); g_strfreev(environment);
    GError *error = NULL;
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, &error);
    g_object_unref(launcher); g_free(installer); g_free(prefix);
    if (!process) {
        label_text(state->port_message, error ? error->message : "Could not start the port update check.");
        g_clear_error(&error); return;
    }
    state->port_process = process; state->port_check_in_progress = TRUE;
    gtk_widget_set_sensitive(state->port_check, FALSE); gtk_widget_set_sensitive(state->port_update, FALSE);
    label_text(state->port_message, "Checking for port updates…");
    g_subprocess_communicate_utf8_async(process, NULL, NULL, settings_port_check_done, state);
}
static void settings_port_update(GtkButton *button, gpointer data) {
    LauncherState *state = data;
    if (state->running || !state->port_update_available || state->port_check_in_progress || state->port_handoff) return;
    char *run = g_build_filename(state->root, "run.sh", NULL);
    gchar *argv[] = {(gchar *)"sh", run, (gchar *)"port-update-settings", NULL};
    gchar **environment = host_environment();
    environment = g_environ_unsetenv(environment, "ROBLOX_MAC_NO_PROGRESS");
    GError *error = NULL;
    gboolean spawned = g_spawn_async(state->root, argv, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, &error);
    g_strfreev(environment); g_free(run);
    if (!spawned) {
        label_text(state->port_message, error ? error->message : "Could not start the port updater.");
        g_clear_error(&error); settings_refresh(state); return;
    }
    state->port_handoff = TRUE;
    gtk_widget_hide(state->window); gtk_widget_destroy(state->window);
}
static void settings_reset(GtkButton *button, gpointer data) { LauncherState *state = data; setting_set(state, "skipped_version", ""); settings_refresh(state); }
static gboolean launcher_delete(GtkWidget *window, GdkEvent *event, gpointer data) {
    LauncherState *state = data;
    if (state->check_in_progress || state->port_check_in_progress) {
        settings_message(state, "Please wait for the update check to finish.");
        return TRUE;
    }
    return FALSE;
}
static gboolean play_handoff(gpointer data) { LauncherState *state = data; state->launch_check_id = 0; if (state->launch_pid > 0 && (kill(state->launch_pid, 0) == 0 || errno == EPERM)) { gtk_widget_destroy(state->window); return G_SOURCE_REMOVE; } state->launch_pid = 0; label_text(state->play_status, "Roblox did not start. Open last log for details."); return G_SOURCE_REMOVE; }
static void play_launch(GtkButton *button, gpointer data) { LauncherState *state = data; char *run = g_build_filename(state->root, "run.sh", NULL); gchar *argv[] = {(gchar *)"setsid", (gchar *)"sh", run, NULL}; gchar **environment = host_environment(); GError *error = NULL; if (!g_spawn_async(state->root, argv, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, &state->launch_pid, &error)) { label_text(state->play_status, error->message); g_clear_error(&error); state->launch_pid = 0; } else { state->launch_check_id = g_timeout_add(1500, play_handoff, state); } g_strfreev(environment); g_free(run); }
static void find_newest_log(const char *dir, gchar **newest, time_t *latest) {
    GDir *entries = g_dir_open(dir, 0, NULL); if (!entries) return;
    const char *name;
    while ((name = g_dir_read_name(entries))) {
        gchar *path = g_build_filename(dir, name, NULL); struct stat info;
        if (stat(path, &info) != 0) { g_free(path); continue; }
        if (S_ISDIR(info.st_mode)) find_newest_log(path, newest, latest);
        else if (S_ISREG(info.st_mode) && info.st_mtime >= *latest) { g_free(*newest); *newest = path; *latest = info.st_mtime; continue; }
        g_free(path);
    }
    g_dir_close(entries);
}
static void open_log(GtkButton *button, gpointer data) {
    LauncherState *state = data; gchar *dir = g_build_filename(state->root, "DO_NOT_SHARE", "diagnostics", NULL); gchar *newest = NULL; time_t latest = 0; find_newest_log(dir, &newest, &latest);
    if (!newest) settings_message(state, "No diagnostics log is available yet.");
    else { gchar *uri = g_filename_to_uri(newest, NULL, NULL); gchar *argv[] = {(gchar *)"xdg-open", uri, NULL}; g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL); g_free(uri); g_free(newest); }
    g_free(dir);
}
static void show_page(GtkToggleButton *button, gpointer data) { if (!gtk_toggle_button_get_active(button)) return; LauncherState *state = data; gtk_stack_set_visible_child_name(GTK_STACK(state->stack), g_object_get_data(G_OBJECT(button), "page")); }
static GtkWidget *nav_button(const char *text, const char *page, LauncherState *state, GSList **group) { GtkWidget *button = gtk_radio_button_new_with_label(*group, text); *group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(button)); gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(button), FALSE); g_object_set_data(G_OBJECT(button), "page", (gpointer)page); gtk_style_context_add_class(gtk_widget_get_style_context(button), "nav"); g_signal_connect(button, "toggled", G_CALLBACK(show_page), state); return button; }

static int run_launcher(const char *root, gboolean settings_page) {
    if (!gtk_init_check(NULL, NULL)) return 1;
    apply_css(); LauncherState *state = g_new0(LauncherState, 1); state->root = g_strdup(root); state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(state->window), "Roblox Mac Linux Port"); setup_ui_identity(state->window, state->root); gtk_window_set_default_size(GTK_WINDOW(state->window), 820, 560); gtk_widget_set_size_request(state->window, 680, 460); gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER); g_signal_connect(state->window, "delete-event", G_CALLBACK(launcher_delete), state); g_signal_connect(state->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *outer = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0); gtk_container_add(GTK_CONTAINER(state->window), outer); GtkWidget *sidebar = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4); gtk_widget_set_size_request(sidebar, 190, -1); gtk_style_context_add_class(gtk_widget_get_style_context(sidebar), "sidebar"); gtk_box_pack_start(GTK_BOX(outer), sidebar, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(sidebar), label("Roblox Mac Linux Port", "brand"), FALSE, FALSE, 0); GtkWidget *nav = gtk_box_new(GTK_ORIENTATION_VERTICAL, 3); gtk_box_pack_start(GTK_BOX(sidebar), nav, FALSE, FALSE, 0); GSList *group = NULL; GtkWidget *play_nav = nav_button("▶  Play", "play", state, &group); GtkWidget *settings_nav = nav_button("⚙  Settings", "settings", state, &group); GtkWidget *info_nav = nav_button("●  Info", "info", state, &group); gtk_box_pack_start(GTK_BOX(nav), play_nav, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(nav), settings_nav, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(nav), info_nav, FALSE, FALSE, 0);
    state->stack = gtk_stack_new(); gtk_stack_set_transition_type(GTK_STACK(state->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE); gtk_box_pack_start(GTK_BOX(outer), state->stack, TRUE, TRUE, 0);
    GtkWidget *play = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18); gtk_container_set_border_width(GTK_CONTAINER(play), 42); gchar *icon_path = find_icon_path(root); GError *icon_error = NULL; GdkPixbuf *pixbuf = icon_path ? gdk_pixbuf_new_from_file_at_scale(icon_path, 160, 160, TRUE, &icon_error) : NULL; GtkWidget *image = pixbuf ? gtk_image_new_from_pixbuf(pixbuf) : gtk_image_new(); if (pixbuf) g_object_unref(pixbuf); g_clear_error(&icon_error); g_free(icon_path); gtk_widget_set_halign(image, GTK_ALIGN_CENTER); gtk_box_pack_start(GTK_BOX(play), image, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(play), label("Roblox Mac Linux Port", "title"), FALSE, FALSE, 0); state->play_version = label("Client version: —", "subtitle"); gtk_box_pack_start(GTK_BOX(play), state->play_version, FALSE, FALSE, 0); state->play_status = label("Checking…", "subtitle"); gtk_box_pack_start(GTK_BOX(play), state->play_status, FALSE, FALSE, 0); GtkWidget *play_button = gtk_button_new_with_mnemonic("_Play"); gtk_style_context_add_class(gtk_widget_get_style_context(play_button), "accent"); g_signal_connect(play_button, "clicked", G_CALLBACK(play_launch), state); gtk_box_pack_start(GTK_BOX(play), play_button, FALSE, FALSE, 0); GtkWidget *log_button = gtk_button_new_with_mnemonic("Open last _log"); g_signal_connect(log_button, "clicked", G_CALLBACK(open_log), state); gtk_box_pack_start(GTK_BOX(play), log_button, FALSE, FALSE, 0); gtk_stack_add_named(GTK_STACK(state->stack), play, "play");
    GtkWidget *settings = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(settings), 42);
    GtkWidget *update_header = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_style_context_add_class(gtk_widget_get_style_context(update_header), "update-header");
    gtk_box_pack_start(GTK_BOX(settings), update_header, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), label("Settings", "title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), label("Updates and launch behavior", "subtitle"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), label("Roblox updates", "section-title"), FALSE, FALSE, 4);
    state->settings_current = label(NULL, "update-value");
    state->settings_latest = label(NULL, "update-value");
    state->settings_checked = label(NULL, "update-value");
    state->settings_skipped = label(NULL, "update-value");
    gtk_box_pack_start(GTK_BOX(update_header), state->settings_current, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), state->settings_latest, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), state->settings_checked, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(update_header), state->settings_skipped, FALSE, FALSE, 0);
    GtkWidget *actions = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(actions), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(actions), 8);
    gtk_box_pack_start(GTK_BOX(update_header), actions, FALSE, FALSE, 4);
    state->check = gtk_button_new_with_mnemonic("_Check for updates");
    g_signal_connect(state->check, "clicked", G_CALLBACK(settings_check), state);
    gtk_container_add(GTK_CONTAINER(actions), state->check);
    state->update = gtk_button_new_with_mnemonic("_Update now");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->update), "accent");
    g_signal_connect(state->update, "clicked", G_CALLBACK(settings_update), state);
    gtk_container_add(GTK_CONTAINER(actions), state->update);
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset skipped version");
    g_signal_connect(reset, "clicked", G_CALLBACK(settings_reset), state);
    gtk_container_add(GTK_CONTAINER(actions), reset);
    state->message = label(NULL, "status");
    gtk_box_pack_start(GTK_BOX(update_header), state->message, FALSE, FALSE, 0);

    GtkWidget *port_separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(settings), port_separator, FALSE, FALSE, 8);
    GtkWidget *port_section = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_box_pack_start(GTK_BOX(settings), port_section, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(port_section), label("Port updates", "section-title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(port_section), label("Updates the Roblox Mac Linux Port files while preserving the client and your data.", "subtitle"), FALSE, FALSE, 0);
    state->port_current = label(NULL, "update-value");
    state->port_latest = label(NULL, "update-value");
    state->port_checked = label(NULL, "update-value");
    gtk_box_pack_start(GTK_BOX(port_section), state->port_current, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(port_section), state->port_latest, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(port_section), state->port_checked, FALSE, FALSE, 0);
    GtkWidget *port_actions = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(port_actions), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(port_actions), 8);
    gtk_box_pack_start(GTK_BOX(port_section), port_actions, FALSE, FALSE, 4);
    state->port_check = gtk_button_new_with_mnemonic("_Check for port updates");
    g_signal_connect(state->port_check, "clicked", G_CALLBACK(settings_port_check), state);
    gtk_container_add(GTK_CONTAINER(port_actions), state->port_check);
    state->port_update = gtk_button_new_with_mnemonic("_Update port");
    gtk_style_context_add_class(gtk_widget_get_style_context(state->port_update), "accent");
    g_signal_connect(state->port_update, "clicked", G_CALLBACK(settings_port_update), state);
    gtk_container_add(GTK_CONTAINER(port_actions), state->port_update);
    state->port_message = label(NULL, "status");
    gtk_box_pack_start(GTK_BOX(port_section), state->port_message, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings), label("Launch behavior", "section-title"), FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 20);
    gtk_box_pack_start(GTK_BOX(settings), grid, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), label("Auto-update mode", NULL), 0, 0, 1, 1);
    state->mode = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "auto", "Auto");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "ask", "Ask");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "off", "Off");
    gtk_grid_attach(GTK_GRID(grid), state->mode, 1, 0, 1, 1);
    g_signal_connect(state->mode, "changed", G_CALLBACK(mode_changed), state);
    gtk_grid_attach(GTK_GRID(grid), label("Check interval (hours)", NULL), 0, 1, 1, 1);
    state->interval = gtk_spin_button_new_with_range(0, 8760, 1);
    gtk_grid_attach(GTK_GRID(grid), state->interval, 1, 1, 1, 1);
    g_signal_connect(state->interval, "value-changed", G_CALLBACK(interval_changed), state);
    GtkWidget *sep = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_pack_start(GTK_BOX(settings), sep, FALSE, FALSE, 6);
    gtk_box_pack_start(GTK_BOX(settings), label("Appearance and desktop integration", "title"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings), label("Window decoration and cursor preferences (applies on next launch)", "subtitle"), FALSE, FALSE, 0);
    GtkWidget *app_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(app_grid), 12);
    gtk_grid_set_column_spacing(GTK_GRID(app_grid), 20);
    gtk_box_pack_start(GTK_BOX(settings), app_grid, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(app_grid), label("Window decorations", NULL), 0, 0, 1, 1);
    state->decorations_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->decorations_combo), "auto", "Automatic (compositor native with fallback)");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->decorations_combo), "system", "Prefer system decorations");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->decorations_combo), "gtk", "GTK decorations");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->decorations_combo), "legacy", "Legacy compatibility");
    gtk_grid_attach(GTK_GRID(app_grid), state->decorations_combo, 1, 0, 1, 1);
    g_signal_connect(state->decorations_combo, "changed", G_CALLBACK(decorations_changed), state);
    gtk_grid_attach(GTK_GRID(app_grid), label("Cursor appearance", NULL), 0, 1, 1, 1);
    state->cursor_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->cursor_combo), "system", "Desktop / system theme");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->cursor_combo), "legacy", "Roblox legacy");
    gtk_grid_attach(GTK_GRID(app_grid), state->cursor_combo, 1, 1, 1, 1);
    g_signal_connect(state->cursor_combo, "changed", G_CALLBACK(cursor_changed), state);
    gtk_box_pack_start(GTK_BOX(settings), label("• Automatic prefers native compositor decorations (KWin/Plasma) when supported, falling back to GTK.", "muted"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings), label("• Desktop cursor uses your system theme and size, while preserving custom game cursors.", "muted"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(settings), label("Appearance changes apply on the next launch.", "muted"), FALSE, FALSE, 0);
    GtkWidget *app_actions = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(app_actions), GTK_BUTTONBOX_START);
    gtk_box_set_spacing(GTK_BOX(app_actions), 8);
    gtk_box_pack_start(GTK_BOX(settings), app_actions, FALSE, FALSE, 0);
    GtkWidget *reset_app = gtk_button_new_with_mnemonic("_Restore appearance defaults");
    g_signal_connect(reset_app, "clicked", G_CALLBACK(appearance_reset), state);
    gtk_container_add(GTK_CONTAINER(app_actions), reset_app);
    GtkWidget *settings_scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(settings_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(settings_scroll), settings);
    gtk_stack_add_named(GTK_STACK(state->stack), settings_scroll, "settings");
    GtkWidget *info = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14); gtk_container_set_border_width(GTK_CONTAINER(info), 42); gtk_box_pack_start(GTK_BOX(info), label("About Roblox Mac Linux Port", "title"), FALSE, FALSE, 0); state->info_client = label("Client version: —", "subtitle"); gtk_box_pack_start(GTK_BOX(info), state->info_client, FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(info), label("Runtime build: bundled Darling + Wayland/Vulkan AppImage", "subtitle"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(info), label("An experimental Intel macOS Roblox client runner for Linux using Darling, Wayland and Vulkan.", NULL), FALSE, FALSE, 0); char *paths = g_strdup_printf("Release: %s\nData and logs: %s/DO_NOT_SHARE", root, root); gtk_box_pack_start(GTK_BOX(info), label(paths, "muted"), FALSE, FALSE, 0); g_free(paths); gtk_box_pack_start(GTK_BOX(info), label("Logs can contain account details; review them before sharing.", "muted"), FALSE, FALSE, 0); gtk_box_pack_start(GTK_BOX(info), label("Project: https://github.com/georgenoob1234/Roblox-Mac-Linux-Port", "muted"), FALSE, FALSE, 0); gtk_stack_add_named(GTK_STACK(state->stack), info, "info");
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_page ? settings_nav : play_nav), TRUE); gtk_widget_show_all(state->window); settings_refresh(state); gtk_main(); if (state->launch_check_id) g_source_remove(state->launch_check_id); g_free(state->root); g_free(state); return 0;
}

int main(int argc, char **argv) {
    g_set_prgname("roblox-mac-port");
    g_set_application_name("Roblox");
    if (argc >= 2 && !strcmp(argv[1], "--progress")) return run_progress(argc >= 3 ? argv[2] : ".");
    if (argc >= 4 && !strcmp(argv[1], "--dialog")) return run_dialog(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "--settings")) return run_launcher(argv[2], TRUE);
    if (argc >= 3 && !strcmp(argv[1], "--launcher")) return run_launcher(argv[2], FALSE);
    return 2;
}
