#include <gtk/gtk.h>
#include <json-glib/json-glib.h>
#include <gio/gio.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UI_DELAY_MS 450
#define DIALOG_TIMEOUT_SECONDS 30

typedef struct {
    char *root;
    char *run;
    GtkWidget *window;
    GtkWidget *status;
    GtkWidget *progress;
    guint show_id;
    guint close_id;
    gboolean shown;
    gboolean done;
} ProgressState;

typedef struct {
    char *root;
    GtkWidget *window;
    GtkWidget *mode;
    GtkWidget *interval;
    GtkWidget *current;
    GtkWidget *latest;
    GtkWidget *checked;
    GtkWidget *skipped;
    GtkWidget *update;
    GtkWidget *message;
    gboolean running;
} SettingsState;

typedef struct {
    GtkWidget *window;
    int result;
} DialogState;

static gboolean spawn_sync(char **argv, char **out, int *status) {
    GError *error = NULL;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, out, NULL, status, &error);
    if (!ok) {
        g_clear_error(&error);
        if (out) *out = NULL;
        if (status) *status = -1;
    }
    return ok;
}

static gboolean run_release(const char *root, const char *command, const char *key,
                            const char *value, char **out) {
    char *argv[7] = {(char *)"sh", (char *)root, NULL, NULL, NULL, NULL, NULL};
    int status = -1;
    argv[1] = (char *)g_build_filename(root, "run.sh", NULL);
    argv[2] = (char *)command;
    int n = 3;
    if (key) { argv[n++] = (char *)key; if (value) argv[n++] = (char *)value; }
    argv[n] = NULL;
    gboolean ok = spawn_sync(argv, out, &status) && status == 0;
    g_free(argv[1]);
    return ok;
}

static JsonObject *parse_json(const char *text) {
    if (!text) return NULL;
    JsonParser *parser = json_parser_new();
    GError *error = NULL;
    if (!json_parser_load_from_data(parser, text, -1, &error)) {
        g_clear_error(&error); g_object_unref(parser); return NULL;
    }
    JsonNode *root = json_parser_get_root(parser);
    JsonObject *object = JSON_NODE_HOLDS_OBJECT(root) ? json_node_get_object(root) : NULL;
    if (object) json_object_ref(object);
    g_object_unref(parser);
    return object;
}

static const char *json_string(JsonObject *object, const char *name) {
    return object && json_object_has_member(object, name) &&
           JSON_NODE_HOLDS_VALUE(json_object_get_member(object, name))
        ? json_object_get_string_member(object, name) : "";
}

static gboolean json_bool(JsonObject *object, const char *name) {
    return object && json_object_has_member(object, name) &&
           json_object_get_boolean_member(object, name);
}

static gint64 json_int(JsonObject *object, const char *name) {
    return object && json_object_has_member(object, name)
        ? json_object_get_int_member(object, name) : 0;
}

static void set_label(GtkWidget *widget, const char *prefix, const char *value) {
    char *text = g_strdup_printf("%s%s", prefix, value && *value ? value : "—");
    gtk_label_set_text(GTK_LABEL(widget), text); g_free(text);
}

static void progress_close(ProgressState *state) {
    if (state->window) gtk_widget_destroy(state->window);
    if (state->show_id) g_source_remove(state->show_id);
    if (state->close_id) g_source_remove(state->close_id);
    g_free(state->root); g_free(state->run); g_free(state);
    gtk_main_quit();
}

static gboolean progress_hide(gpointer data) {
    ProgressState *state = data; state->close_id = 0; progress_close(state); return G_SOURCE_REMOVE;
}

static gboolean progress_show(gpointer data) {
    ProgressState *state = data; state->show_id = 0;
    if (state->done) return G_SOURCE_REMOVE;
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state->window), "Roblox");
    gtk_window_set_default_size(GTK_WINDOW(state->window), 430, 150);
    gtk_window_set_resizable(GTK_WINDOW(state->window), FALSE);
    gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER);
    g_signal_connect(state->window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 14);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24);
    gtk_container_add(GTK_CONTAINER(state->window), box);
    state->status = gtk_label_new("Setting up Roblox…");
    gtk_widget_set_halign(state->status, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(box), state->status, FALSE, FALSE, 0);
    state->progress = gtk_progress_bar_new();
    gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progress), TRUE);
    gtk_box_pack_start(GTK_BOX(box), state->progress, FALSE, FALSE, 0);
    gtk_widget_show_all(state->window); state->shown = TRUE; return G_SOURCE_REMOVE;
}

static void progress_event(ProgressState *state, const char *line) {
    JsonObject *object = parse_json(line);
    if (!object) return;
    const char *message = json_string(object, "message");
    gint64 percent = json_int(object, "percent");
    const char *phase = json_string(object, "phase");
    if (state->status && *message) gtk_label_set_text(GTK_LABEL(state->status), message);
    if (state->progress && percent >= 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), percent / 100.0);
    if (!g_strcmp0(phase, "launching") || percent >= 100) {
        state->done = TRUE;
        if (state->shown && !state->close_id) state->close_id = g_timeout_add(350, progress_hide, state);
    }
    json_object_unref(object);
}

static gboolean progress_input(GIOChannel *channel, GIOCondition condition, gpointer data) {
    ProgressState *state = data;
    if (condition & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) { progress_close(state); return G_SOURCE_REMOVE; }
    gchar *line = NULL; gsize length = 0; GError *error = NULL;
    GIOStatus result = g_io_channel_read_line(channel, &line, &length, NULL, &error);
    g_clear_error(&error);
    if (result == G_IO_STATUS_NORMAL) { progress_event(state, line); g_free(line); return G_SOURCE_CONTINUE; }
    g_free(line); progress_close(state); return G_SOURCE_REMOVE;
}

static int run_progress(void) {
    if (!gtk_init_check(NULL, NULL)) return 3;
    ProgressState *state = g_new0(ProgressState, 1);
    state->show_id = g_timeout_add(UI_DELAY_MS, progress_show, state);
    GIOChannel *channel = g_io_channel_unix_new(STDIN_FILENO);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, progress_input, state);
    gtk_main(); g_io_channel_unref(channel); return 0;
}

static void dialog_finish(DialogState *state, int result) {
    state->result = result; gtk_main_quit();
}

static gboolean dialog_timeout(gpointer data) { dialog_finish(data, 1); return G_SOURCE_REMOVE; }
static void dialog_button(GtkWidget *button, gpointer data) {
    int result = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "result"));
    dialog_finish(data, result);
}

static int run_dialog(const char *latest, const char *installed) {
    if (!gtk_init_check(NULL, NULL)) return 3;
    DialogState state = {0};
    state.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(state.window), "Roblox update available");
    gtk_window_set_default_size(GTK_WINDOW(state.window), 470, 250);
    gtk_window_set_resizable(GTK_WINDOW(state.window), FALSE);
    gtk_window_set_modal(GTK_WINDOW(state.window), TRUE);
    gtk_window_set_position(GTK_WINDOW(state.window), GTK_WIN_POS_CENTER);
    g_signal_connect(state.window, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_container_set_border_width(GTK_CONTAINER(box), 24); gtk_container_add(GTK_CONTAINER(state.window), box);
    GtkWidget *title = gtk_label_new(NULL);
    char *title_text = g_strdup_printf("Roblox %s is ready", latest && *latest ? latest : "has an update");
    gtk_label_set_markup(GTK_LABEL(title), title_text); g_free(title_text);
    gtk_widget_set_halign(title, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
    char *versions = g_strdup_printf("Installed: %s\nNew version: %s", installed && *installed ? installed : "none", latest && *latest ? latest : "unknown");
    GtkWidget *version_label = gtk_label_new(versions); g_free(versions);
    gtk_widget_set_halign(version_label, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), version_label, FALSE, FALSE, 0);
    GtkWidget *note = gtk_label_new("Only one client version has been tested; see the README for details.");
    gtk_label_set_line_wrap(GTK_LABEL(note), TRUE); gtk_widget_set_halign(note, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), note, FALSE, FALSE, 0);
    GtkWidget *buttons = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_button_box_set_layout(GTK_BUTTON_BOX(buttons), GTK_BUTTONBOX_END); gtk_box_set_spacing(GTK_BOX(buttons), 8);
    const char *labels[] = {"Update now", "Launch without updating", "Skip this version"};
    for (int i = 0; i < 3; ++i) {
        GtkWidget *button = gtk_button_new_with_mnemonic(labels[i]);
        g_object_set_data(G_OBJECT(button), "result", GINT_TO_POINTER(i == 0 ? 0 : (i == 1 ? 1 : 2)));
        g_signal_connect(button, "clicked", G_CALLBACK(dialog_button), &state);
        gtk_container_add(GTK_CONTAINER(buttons), button);
        if (i == 1) gtk_widget_grab_focus(button);
    }
    gtk_box_pack_end(GTK_BOX(box), buttons, FALSE, FALSE, 0); gtk_widget_show_all(state.window);
    g_timeout_add_seconds(DIALOG_TIMEOUT_SECONDS, dialog_timeout, &state); gtk_main(); gtk_widget_destroy(state.window); return state.result;
}

static void settings_message(SettingsState *state, const char *message) { gtk_label_set_text(GTK_LABEL(state->message), message ? message : ""); }

static JsonObject *settings_status(SettingsState *state) {
    char *output = NULL; run_release(state->root, "status", NULL, NULL, &output);
    JsonObject *object = parse_json(output); g_free(output); return object;
}

static void settings_refresh(SettingsState *state) {
    JsonObject *object = settings_status(state); if (!object) { settings_message(state, "Status unavailable; Roblox will launch without updating."); return; }
    JsonObject *settings = json_object_has_member(object, "settings") ? json_object_get_object_member(object, "settings") : NULL;
    const char *mode = settings ? json_string(settings, "auto_update") : "ask";
    const char *latest = json_string(object, "latest_known"); const char *current = json_string(object, "installed_version");
    const char *skipped_value = settings ? json_string(settings, "skipped_version") : "";
    gtk_combo_box_set_active_id(GTK_COMBO_BOX(state->mode), mode);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(state->interval), settings ? json_int(settings, "check_interval_hours") : 24);
    set_label(state->current, "Current version: ", current); set_label(state->latest, "Latest known: ", latest);
    char checked[64] = "—"; gint64 timestamp = settings ? json_int(settings, "last_check") : 0;
    if (timestamp > 0) { GDateTime *date = g_date_time_new_from_unix_local(timestamp); gchar *formatted = g_date_time_format(date, "%Y-%m-%d %H:%M"); g_strlcpy(checked, formatted, sizeof checked); g_free(formatted); g_date_time_unref(date); }
    set_label(state->checked, "Last checked: ", checked); set_label(state->skipped, "Skipped version: ", skipped_value);
    state->running = json_bool(object, "game_running");
    gtk_widget_set_sensitive(state->update, !state->running);
    gtk_widget_set_tooltip_text(state->update, state->running ? "Close Roblox before updating." : "Update the installed client now.");
    if (state->running) settings_message(state, "Roblox is running; Update now is disabled until it closes.");
    json_object_unref(object);
}

static void setting_set(SettingsState *state, const char *key, const char *value) {
    if (!run_release(state->root, "set", key, value, NULL)) settings_message(state, "Could not save this setting.");
}
static void mode_changed(GtkComboBox *combo, gpointer data) { SettingsState *state = data; const char *id = gtk_combo_box_get_active_id(combo); if (id) setting_set(state, "auto_update", id); }
static void interval_changed(GtkSpinButton *spin, gpointer data) { SettingsState *state = data; char value[32]; g_snprintf(value, sizeof value, "%d", gtk_spin_button_get_value_as_int(spin)); setting_set(state, "check_interval_hours", value); }
static void settings_check(GtkButton *button, gpointer data) { SettingsState *state = data; char *output = NULL; if (run_release(state->root, "check", NULL, NULL, &output)) settings_message(state, "Update check complete."); else settings_message(state, "Update check failed; Roblox will launch without updating."); g_free(output); settings_refresh(state); }
static void settings_update(GtkButton *button, gpointer data) { SettingsState *state = data; if (state->running) return; settings_message(state, "Updating Roblox…"); if (run_release(state->root, "update", NULL, NULL, NULL)) settings_message(state, "Update complete."); else settings_message(state, "Update failed; the installed client was kept."); settings_refresh(state); }
static void settings_reset(GtkButton *button, gpointer data) { SettingsState *state = data; setting_set(state, "skipped_version", ""); settings_refresh(state); }

static int run_settings(const char *root) {
    if (!gtk_init_check(NULL, NULL)) return 1;
    SettingsState *state = g_new0(SettingsState, 1); state->root = g_strdup(root);
    state->window = gtk_window_new(GTK_WINDOW_TOPLEVEL); gtk_window_set_title(GTK_WINDOW(state->window), "Roblox Settings"); gtk_window_set_default_size(GTK_WINDOW(state->window), 520, 390); gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER);
    g_signal_connect(state->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12); gtk_container_set_border_width(GTK_CONTAINER(box), 24); gtk_container_add(GTK_CONTAINER(state->window), box);
    GtkWidget *heading = gtk_label_new("Roblox update settings"); gtk_widget_set_halign(heading, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), heading, FALSE, FALSE, 0);
    GtkWidget *grid = gtk_grid_new(); gtk_grid_set_row_spacing(GTK_GRID(grid), 9); gtk_grid_set_column_spacing(GTK_GRID(grid), 14); gtk_box_pack_start(GTK_BOX(box), grid, FALSE, FALSE, 0);
    GtkWidget *mode_label = gtk_label_new("Auto-update mode"); gtk_widget_set_halign(mode_label, GTK_ALIGN_START); gtk_grid_attach(GTK_GRID(grid), mode_label, 0, 0, 1, 1);
    state->mode = gtk_combo_box_text_new(); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "auto", "Auto"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "ask", "Ask"); gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(state->mode), "off", "Off"); gtk_grid_attach(GTK_GRID(grid), state->mode, 1, 0, 1, 1); g_signal_connect(state->mode, "changed", G_CALLBACK(mode_changed), state);
    GtkWidget *interval_label = gtk_label_new("Check interval (hours)"); gtk_widget_set_halign(interval_label, GTK_ALIGN_START); gtk_grid_attach(GTK_GRID(grid), interval_label, 0, 1, 1, 1);
    state->interval = gtk_spin_button_new_with_range(0, 8760, 1); gtk_grid_attach(GTK_GRID(grid), state->interval, 1, 1, 1, 1); g_signal_connect(state->interval, "value-changed", G_CALLBACK(interval_changed), state);
    state->current = gtk_label_new(NULL); state->latest = gtk_label_new(NULL); state->checked = gtk_label_new(NULL); state->skipped = gtk_label_new(NULL);
    for (GtkWidget *label = state->current; label != NULL; label = label == state->current ? state->latest : (label == state->latest ? state->checked : (label == state->checked ? state->skipped : NULL))) { if (!label) break; gtk_widget_set_halign(label, GTK_ALIGN_START); gtk_box_pack_start(GTK_BOX(box), label, FALSE, FALSE, 0); if (label == state->skipped) break; }
    GtkWidget *actions = gtk_button_box_new(GTK_ORIENTATION_HORIZONTAL); gtk_button_box_set_layout(GTK_BUTTON_BOX(actions), GTK_BUTTONBOX_START); gtk_box_set_spacing(GTK_BOX(actions), 8); gtk_box_pack_start(GTK_BOX(box), actions, FALSE, FALSE, 0);
    GtkWidget *check = gtk_button_new_with_mnemonic("_Check for updates"); g_signal_connect(check, "clicked", G_CALLBACK(settings_check), state); gtk_container_add(GTK_CONTAINER(actions), check);
    state->update = gtk_button_new_with_mnemonic("_Update now"); g_signal_connect(state->update, "clicked", G_CALLBACK(settings_update), state); gtk_container_add(GTK_CONTAINER(actions), state->update);
    GtkWidget *reset = gtk_button_new_with_mnemonic("_Reset skipped version"); g_signal_connect(reset, "clicked", G_CALLBACK(settings_reset), state); gtk_container_add(GTK_CONTAINER(actions), reset);
    state->message = gtk_label_new(""); gtk_label_set_line_wrap(GTK_LABEL(state->message), TRUE); gtk_widget_set_halign(state->message, GTK_ALIGN_START); gtk_box_pack_end(GTK_BOX(box), state->message, FALSE, FALSE, 0);
    gtk_widget_show_all(state->window); settings_refresh(state); gtk_main(); g_free(state->root); g_free(state); return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "--progress")) return run_progress();
    if (argc >= 4 && !strcmp(argv[1], "--dialog")) return run_dialog(argv[2], argv[3]);
    if (argc >= 3 && !strcmp(argv[1], "--settings")) return run_settings(argv[2]);
    return 2;
}
