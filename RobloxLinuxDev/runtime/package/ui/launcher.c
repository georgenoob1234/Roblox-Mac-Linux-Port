/* The launcher and settings window: sidebar Play / Updates / Appearance /
 * About, cards, and a status bar (B10 phase 3, George's mockup). */

typedef struct {
    char *root;
    GtkWidget *window, *stack, *play_version, *play_pill, *play_status, *last_log;
    GtkWidget *client_sub, *client_pill, *check, *update, *message_box, *message;
    GtkWidget *port_sub, *port_pill, *port_check, *port_update, *port_message_box, *port_message_label;
    GtkWidget *mode, *interval, *skipped_sub, *forget_skipped;
    GtkWidget *decorations_combo, *cursor_switch, *appearance_message;
    GtkWidget *about_versions, *status_versions, *status_port;
    GSubprocess *check_process, *port_process;
    GPid launch_pid;
    guint launch_check_id;
    /* Set while settings_refresh fills the controls, so their "changed"
     * handlers do not write the same values back. */
    gboolean refreshing;
    gboolean running, update_available, check_in_progress, handoff;
    gboolean port_update_available, port_check_in_progress, port_handoff;
} LauncherState;

/* A message line under a card. Failures ("Could not ...") get the error box. */
static void message_set(GtkWidget *box, GtkWidget *text, const char *message) {
    label_text(text, message);
    gtk_widget_set_visible(box, message && *message);
    GtkStyleContext *context = gtk_widget_get_style_context(box);
    gboolean error = message && (g_str_has_prefix(message, "Could not") || g_str_has_prefix(message, "Roblox did not start"));
    gtk_style_context_remove_class(context, error ? "status-line" : "error-box");
    gtk_style_context_add_class(context, error ? "error-box" : "status-line");
}
static GtkWidget *message_area(GtkWidget **text) {
    GtkWidget *box = hbox(10); gtk_widget_set_margin_top(box, 2); gtk_widget_set_margin_bottom(box, 6);
    *text = label(NULL, NULL); gtk_label_set_xalign(GTK_LABEL(*text), 0); gtk_label_set_selectable(GTK_LABEL(*text), TRUE);
    gtk_box_pack_start(GTK_BOX(box), *text, TRUE, TRUE, 0);
    gtk_widget_set_no_show_all(box, TRUE);
    gtk_widget_show(*text);
    return box;
}
static void settings_message(LauncherState *state, const char *message) { message_set(state->message_box, state->message, message); }
static void port_message(LauncherState *state, const char *message) { message_set(state->port_message_box, state->port_message_label, message); }
static JsonObject *settings_status(LauncherState *state) { char *output = NULL; run_release(state->root, "status", NULL, NULL, &output); JsonObject *object = parse_json(output); g_free(output); return object; }
static char *when_text(gint64 timestamp) {
    if (timestamp <= 0) return g_strdup("never");
    GDateTime *date = g_date_time_new_from_unix_local(timestamp), *now = g_date_time_new_now_local();
    gboolean today = g_date_time_get_year(date) == g_date_time_get_year(now) && g_date_time_get_day_of_year(date) == g_date_time_get_day_of_year(now);
    gchar *text = g_date_time_format(date, today ? "today %H:%M" : "%Y-%m-%d %H:%M");
    g_date_time_unref(date); g_date_time_unref(now);
    return text;
}
/* The interval choices; any other stored value is shown as its own entry. */
static void interval_select(LauncherState *state, gint64 hours) {
    char id[32]; g_snprintf(id, sizeof id, "%lld", (long long)hours);
    if (!dropdown_set_active(state->interval, id)) {
        char *text = g_strdup_printf("Every %lld hours", (long long)hours);
        dropdown_add(state->interval, id, text); g_free(text);
        dropdown_set_active(state->interval, id);
    }
}
/* A release version is v1.2.3 (the "v" optional); anything else (a commit id,
 * "dev") is a custom build. Same rule as port_is_newer in bootstrap.sh. */
static gboolean release_parts(const char *value, guint64 parts[8], int *count) {
    if (!value) return FALSE;
    if (*value == 'v') value++;
    *count = 0;
    for (;;) {
        if (!g_ascii_isdigit(*value) || *count == 8) return FALSE;
        char *end = NULL; parts[(*count)++] = g_ascii_strtoull(value, &end, 10); value = end;
        if (!*value) return TRUE;
        if (*value++ != '.') return FALSE;
    }
}
/* <0, 0, >0 like strcmp; missing trailing parts count as 0 (v1.2 == v1.2.0). */
static int release_compare(const guint64 *a, int a_count, const guint64 *b, int b_count) {
    for (int i = 0; i < MAX(a_count, b_count); i++) {
        guint64 x = i < a_count ? a[i] : 0, y = i < b_count ? b[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
static void settings_refresh(LauncherState *state) {
    state->refreshing = TRUE;
    JsonObject *object = settings_status(state);
    if (!object) {
        state->update_available = FALSE; gtk_widget_set_sensitive(state->update, FALSE);
        settings_message(state, "Could not read the launcher's status; check again before updating.");
        state->refreshing = FALSE; return;
    }
    JsonObject *settings = json_object_has_member(object, "settings") ? json_object_get_object_member(object, "settings") : NULL;
    const char *mode = settings ? json_string(settings, "auto_update") : "ask";
    const char *current = json_string(object, "installed_display_version");
    const char *latest_raw = json_string(object, "latest_known"), *latest_display = json_string(object, "latest_display_version"), *installed_raw = json_string(object, "installed_version");
    state->update_available = json_object_has_member(object, "update_available") ? json_bool(object, "update_available") : (*latest_raw && g_strcmp0(latest_raw, installed_raw));
    state->running = json_bool(object, "game_running");
    dropdown_set_active(state->mode, *mode ? mode : "ask");
    interval_select(state, settings ? json_int(settings, "check_interval_hours") : 24);
    const char *newest = *latest_display ? latest_display : (*latest_raw ? latest_raw : "");
    char *checked = when_text(settings ? json_int(settings, "last_check") : 0);
    char *client_line = g_strdup_printf("Installed %s · newest %s · checked %s", *current ? current : "—",
                                        state->update_available ? newest : (*newest ? newest : (*current ? current : "—")), checked);
    label_text(state->client_sub, client_line); g_free(client_line); g_free(checked);
    pill_set(state->client_pill, state->update_available ? "Update available" : "Up to date", state->update_available);
    gtk_widget_set_visible(state->client_pill, *current != '\0');
    char *version = g_strdup_printf("Client %s", *current ? current : "not installed");
    label_text(state->play_version, version); g_free(version);
    pill_set(state->play_pill, state->update_available ? "Update available" : "Up to date", state->update_available);
    /* Skipped versions. */
    const char *skipped = settings ? json_string(settings, "skipped_version") : "", *skipped_port = settings ? json_string(settings, "skipped_port_version") : "";
    char *skipped_text = *skipped || *skipped_port
        ? g_strdup_printf("%s%s%s", *skipped ? skipped : "", *skipped && *skipped_port ? " · port " : (*skipped_port ? "port " : ""), skipped_port)
        : g_strdup("None");
    label_text(state->skipped_sub, skipped_text); g_free(skipped_text);
    gtk_widget_set_sensitive(state->forget_skipped, *skipped || *skipped_port);
    /* Appearance. */
    const char *dec = settings ? json_string(settings, "window_decoration") : "auto"; const char *cur = settings ? json_string(settings, "cursor_appearance") : "system";
    dropdown_set_active(state->decorations_combo, *dec ? dec : "auto");
    gtk_switch_set_active(GTK_SWITCH(state->cursor_switch), g_strcmp0(cur, "legacy") != 0);
    /* Client buttons and message. */
    gtk_widget_set_sensitive(state->update, !state->check_in_progress && !state->running && state->update_available);
    gtk_widget_set_tooltip_text(state->update, state->running ? "Close Roblox before updating." : (state->update_available ? "Update the installed client now." : "Already up to date."));
    gtk_widget_set_sensitive(state->check, !state->check_in_progress);
    if (!state->check_in_progress && !state->handoff) {
        if (state->running) settings_message(state, "Roblox is running; Update now is disabled until it closes.");
        else settings_message(state, NULL);
    }
    /* The port. */
    char *port_version = NULL;
    run_release(state->root, "port-version", NULL, NULL, &port_version);
    if (port_version) g_strstrip(port_version);
    const char *port_latest = settings ? json_string(settings, "port_latest") : "";
    guint64 installed_parts[8], latest_parts[8]; int installed_count = 0, latest_count = 0;
    gboolean installed_release = release_parts(port_version, installed_parts, &installed_count);
    gboolean latest_release = release_parts(port_latest, latest_parts, &latest_count);
    gboolean custom_build = port_version && *port_version && !installed_release;
    state->port_update_available = installed_release && latest_release
        && release_compare(installed_parts, installed_count, latest_parts, latest_count) < 0;
    char *port_checked = when_text(settings ? json_int(settings, "port_last_check") : 0);
    char *port_line = g_strdup_printf("Installed %s · newest %s · checked %s", port_version && *port_version ? port_version : "unknown",
                                      *port_latest ? port_latest : "not known yet", port_checked);
    label_text(state->port_sub, port_line); g_free(port_line); g_free(port_checked);
    pill_set(state->port_pill, custom_build ? "Custom build" : (state->port_update_available ? "Update available" : "Up to date"), state->port_update_available);
    gtk_widget_set_visible(state->port_pill, custom_build || *port_latest != '\0');
    gtk_widget_set_sensitive(state->port_update, !state->port_check_in_progress && !state->port_handoff && !state->running && state->port_update_available);
    gtk_widget_set_tooltip_text(state->port_update, state->running ? "Close Roblox before updating the port."
        : state->port_update_available ? "Install the latest port release."
        : custom_build ? "This is a custom build; releases are not offered as updates to it."
        : "No port update is available.");
    gtk_widget_set_sensitive(state->port_check, !state->port_check_in_progress && !state->port_handoff);
    if (!state->port_check_in_progress && !state->port_handoff) {
        if (state->running) port_message(state, "Close Roblox before updating the port.");
        else if (!port_version || !*port_version) port_message(state, "The installed port version is unknown; check for a release before updating.");
        else port_message(state, NULL);
    }
    /* About and the status bar. */
    char *versions = g_strdup_printf("Port %s · client %s", port_version && *port_version ? port_version : "unknown", *current ? current : "not installed");
    label_text(state->about_versions, versions); label_text(state->status_versions, versions); g_free(versions);
    label_text(state->status_port, custom_build ? "● Custom build" : !*port_latest ? "" : (state->port_update_available ? "● Port update available" : "● Port up to date"));
    GtkStyleContext *status_context = gtk_widget_get_style_context(state->status_port);
    gtk_style_context_remove_class(status_context, state->port_update_available ? "ok" : "new");
    gtk_style_context_add_class(status_context, state->port_update_available ? "new" : "ok");
    g_free(port_version);
    json_object_unref(object);
    state->refreshing = FALSE;
}
static void setting_set(LauncherState *state, const char *key, const char *value) { if (!run_release(state->root, "set", key, value, NULL)) settings_message(state, "Could not save this setting."); }
static void mode_changed(GtkWidget *dropdown, gpointer data) { LauncherState *state = data; const char *id = dropdown_get_active(dropdown); if (id && !state->refreshing) setting_set(state, "auto_update", id); }
static void interval_changed(GtkWidget *dropdown, gpointer data) { LauncherState *state = data; const char *id = dropdown_get_active(dropdown); if (id && !state->refreshing) setting_set(state, "check_interval_hours", id); }
static void appearance_message(LauncherState *state, const char *text) { message_set(gtk_widget_get_parent(state->appearance_message), state->appearance_message, text); }
static void decorations_changed(GtkWidget *dropdown, gpointer data) { LauncherState *state = data; const char *id = dropdown_get_active(dropdown); if (id && !state->refreshing) { setting_set(state, "window_decoration", id); appearance_message(state, "Window decorations changed. Applies on the next launch."); } }
static void cursor_changed(GObject *widget, GParamSpec *spec, gpointer data) { LauncherState *state = data; if (state->refreshing) return; setting_set(state, "cursor_appearance", gtk_switch_get_active(GTK_SWITCH(widget)) ? "system" : "legacy"); appearance_message(state, "Cursor changed. Applies on the next launch."); }
static void appearance_reset(GtkButton *button, gpointer data) {
    LauncherState *state = data;
    setting_set(state, "window_decoration", "auto"); setting_set(state, "cursor_appearance", "system");
    settings_refresh(state);
    appearance_message(state, "Appearance restored to the defaults. Applies on the next launch.");
}
static void forget_skipped(GtkButton *button, gpointer data) { LauncherState *state = data; setting_set(state, "skipped_version", ""); setting_set(state, "skipped_port_version", ""); settings_refresh(state); }

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
    else {
        /* B10 defect 2: what failed, why (kept by the launcher as
         * last_check_error), what to do, and where the log is. */
        JsonObject *latest = settings_status(state);
        JsonObject *values = latest && json_object_has_member(latest, "settings") ? json_object_get_object_member(latest, "settings") : NULL;
        const char *why = values ? json_string(values, "last_check_error") : "";
        char *log = g_build_filename(state->root, "DO_NOT_SHARE", "launcher.log", NULL);
        char *text = g_strdup_printf("Could not check for updates: %s.\nCheck your internet connection and try again; the last known status is shown. Details: %s",
                                     *why ? why : "the check did not finish", log);
        settings_message(state, text);
        g_free(text); g_free(log);
        if (latest) json_object_unref(latest);
    }
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
/* Why a port-update check failed, from the installer's own message: its last
 * "! ..." line on stderr, without the "Could not query ..." lead-in. */
static char *port_check_cause(gboolean communicated, GSubprocess *process, const char *error_output) {
    if (!communicated || !g_subprocess_get_if_exited(process))
        return g_strdup("the check stopped unexpectedly");
    char *cause = NULL;
    gchar **lines = g_strsplit(error_output ? error_output : "", "\n", -1);
    for (gchar **line = lines; *line; ++line) {
        char *text = g_strstrip(*line);
        if (g_str_has_prefix(text, "! ")) { g_free(cause); cause = g_strdup(text + 2); }
    }
    g_strfreev(lines);
    const char *lead = "Could not query the latest GitHub release: ";
    if (cause && g_str_has_prefix(cause, lead)) { char *rest = g_strdup(cause + strlen(lead)); g_free(cause); cause = rest; }
    if (cause && g_str_has_suffix(cause, ".")) cause[strlen(cause) - 1] = '\0';
    if (!cause || !*cause) {
        g_free(cause);
        cause = g_subprocess_get_exit_status(process) == 0
            ? g_strdup("the installer's answer could not be read")
            : g_strdup_printf("the installer exited with status %d", g_subprocess_get_exit_status(process));
    }
    return cause;
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
    char *cause = ok ? NULL : port_check_cause(communicated, process, error_output);
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
    if (ok) port_message(state, state->port_update_available ? "A newer port release is available." : "The port is up to date.");
    else {
        /* B10 defect 5: what failed, why, what to do, where the details are. */
        char *log = g_build_filename(g_get_user_state_dir(), "roblox-mac-linux-port", "installer.log", NULL);
        char *text = g_strdup_printf("Could not check for port updates: %s.\nCheck your internet connection and try again. Details: %s", cause, log);
        port_message(state, text);
        g_free(text); g_free(log);
    }
    g_free(cause);
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
        port_message(state, error ? error->message : "Could not start the port update check.");
        g_clear_error(&error); return;
    }
    state->port_process = process; state->port_check_in_progress = TRUE;
    gtk_widget_set_sensitive(state->port_check, FALSE); gtk_widget_set_sensitive(state->port_update, FALSE);
    port_message(state, "Checking for port updates…");
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
        port_message(state, error ? error->message : "Could not start the port updater.");
        g_clear_error(&error); settings_refresh(state); return;
    }
    state->port_handoff = TRUE;
    gtk_widget_hide(state->window); gtk_widget_destroy(state->window);
}
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

static void open_path(LauncherState *state, const char *path) {
    gchar *uri = g_str_has_prefix(path, "https://") ? g_strdup(path) : g_filename_to_uri(path, NULL, NULL);
    gchar *argv[] = {(gchar *)"xdg-open", uri, NULL};
    gchar **environment = host_environment();
    if (!uri || !g_spawn_async(NULL, argv, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL))
        settings_message(state, "Could not open it: xdg-open is not available.");
    g_strfreev(environment); g_free(uri);
}
static void open_data_folder(GtkButton *button, gpointer data) { LauncherState *state = data; char *path = g_build_filename(state->root, "DO_NOT_SHARE", NULL); open_path(state, path); g_free(path); }
static void open_install_folder(GtkButton *button, gpointer data) { LauncherState *state = data; open_path(state, state->root); }
static void open_project(GtkButton *button, gpointer data) { open_path(data, "https://github.com/georgenoob1234/Roblox-Mac-Linux-Port"); }
static void show_page(GtkToggleButton *button, gpointer data) { if (!gtk_toggle_button_get_active(button)) return; LauncherState *state = data; gtk_stack_set_visible_child_name(GTK_STACK(state->stack), g_object_get_data(G_OBJECT(button), "page")); }
static GtkWidget *nav_button(const char *text, const char *icon_name, const char *page, LauncherState *state, GSList **group) {
    GtkWidget *widget = gtk_radio_button_new(*group); *group = gtk_radio_button_get_group(GTK_RADIO_BUTTON(widget));
    gtk_toggle_button_set_mode(GTK_TOGGLE_BUTTON(widget), FALSE);
    button_content(widget, text, icon_name, 18, TRUE);
    gtk_widget_set_can_focus(widget, TRUE);
    g_object_set_data(G_OBJECT(widget), "page", (gpointer)page); add_class(widget, "nav");
    g_signal_connect(widget, "toggled", G_CALLBACK(show_page), state);
    return widget;
}
static GtkWidget *page_box(const char *title, const char *subtitle) {
    GtkWidget *box = vbox(8); add_class(box, "content");
    if (title) gtk_box_pack_start(GTK_BOX(box), label(title, "page-title"), FALSE, FALSE, 0);
    if (subtitle) { GtkWidget *sub = label(subtitle, "muted"); gtk_widget_set_margin_bottom(sub, 10); gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0); }
    return box;
}
static void add_page(LauncherState *state, GtkWidget *page, const char *name) {
    GtkWidget *scroll = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_container_add(GTK_CONTAINER(scroll), page);
    gtk_stack_add_named(GTK_STACK(state->stack), scroll, name);
}
static gchar *newest_log_text(const char *root) {
    gchar *dir = g_build_filename(root, "DO_NOT_SHARE", "diagnostics", NULL); gchar *newest = NULL; time_t latest = 0;
    find_newest_log(dir, &newest, &latest); g_free(dir);
    if (!newest) return g_strdup("No log yet: logs appear after the first game.");
    char *when = when_text(latest), *text = g_strdup_printf("Written %s", when);
    g_free(when); g_free(newest); return text;
}

static int run_launcher(const char *root, gboolean settings_page) {
    if (!ui_init()) return 1;
    LauncherState *state = g_new0(LauncherState, 1); state->root = g_strdup(root);
    state->window = themed_window("Roblox Mac Linux Port", state->root, TRUE, TRUE);
    setup_ui_identity(state->window, state->root);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 900, 600); gtk_widget_set_size_request(state->window, 760, 500); gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER);
    g_signal_connect(state->window, "delete-event", G_CALLBACK(launcher_delete), state); g_signal_connect(state->window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    int scale = gtk_widget_get_scale_factor(state->window);
    GtkWidget *outer = vbox(0); gtk_container_add(GTK_CONTAINER(state->window), outer);
    GtkWidget *main = hbox(0); gtk_box_pack_start(GTK_BOX(outer), main, TRUE, TRUE, 0);

    /* Sidebar */
    GtkWidget *sidebar = vbox(3); gtk_widget_set_size_request(sidebar, 214, -1); add_class(sidebar, "sidebar");
    gtk_box_pack_start(GTK_BOX(main), sidebar, FALSE, FALSE, 0);
    GtkWidget *brand = hbox(10); gtk_widget_set_margin_start(brand, 8); gtk_widget_set_margin_bottom(brand, 14); gtk_widget_set_margin_top(brand, 4);
    gtk_box_pack_start(GTK_BOX(brand), app_image(root, 30, scale), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(brand), label("Roblox Mac\nLinux Port", "brand-name"), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), brand, FALSE, FALSE, 0);
    GSList *group = NULL;
    GtkWidget *play_nav = nav_button("Play", "play", "play", state, &group);
    GtkWidget *updates_nav = nav_button("Updates", "update", "updates", state, &group);
    GtkWidget *appearance_nav = nav_button("Appearance", "appearance", "appearance", state, &group);
    GtkWidget *about_nav = nav_button("About", "about", "about", state, &group);
    gtk_box_pack_start(GTK_BOX(sidebar), play_nav, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), updates_nav, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(sidebar), appearance_nav, FALSE, FALSE, 0);
    gtk_box_pack_end(GTK_BOX(sidebar), about_nav, FALSE, FALSE, 0);
    state->stack = gtk_stack_new(); gtk_stack_set_transition_type(GTK_STACK(state->stack), GTK_STACK_TRANSITION_TYPE_CROSSFADE);
    gtk_stack_set_transition_duration(GTK_STACK(state->stack), 150);
    gtk_box_pack_start(GTK_BOX(main), state->stack, TRUE, TRUE, 0);

    /* The controls at the right of a page's cards share one width, so they
     * line up (George, 2026-11-15). The cursor switch keeps its own size. */
    GtkSizeGroup *play_controls = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL), *update_controls = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);
    GtkSizeGroup *appearance_controls = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL), *about_controls = gtk_size_group_new(GTK_SIZE_GROUP_HORIZONTAL);

    /* Play */
    GtkWidget *play = page_box(NULL, NULL);
    GtkWidget *hero = hbox(22); add_class(hero, "hero");
    gtk_box_pack_start(GTK_BOX(hero), app_image(root, 92, scale), FALSE, FALSE, 0);
    GtkWidget *hero_text = vbox(4); gtk_widget_set_valign(hero_text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(hero_text), label("Roblox", "hero-title"), FALSE, FALSE, 0);
    GtkWidget *version_row = hbox(8);
    state->play_version = label("Client —", "muted"); gtk_box_pack_start(GTK_BOX(version_row), state->play_version, FALSE, FALSE, 0);
    state->play_pill = pill("…"); gtk_box_pack_start(GTK_BOX(version_row), state->play_pill, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero_text), version_row, FALSE, FALSE, 0);
    GtkWidget *play_button = button("_Play", "play", "primary"); add_class(play_button, "big");
    gtk_widget_set_halign(play_button, GTK_ALIGN_START); gtk_widget_set_margin_top(play_button, 10);
    g_signal_connect(play_button, "clicked", G_CALLBACK(play_launch), state);
    gtk_box_pack_start(GTK_BOX(hero_text), play_button, FALSE, FALSE, 0);
    state->play_status = label(NULL, "muted"); gtk_box_pack_start(GTK_BOX(hero_text), state->play_status, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hero), hero_text, TRUE, TRUE, 0);
    gtk_box_pack_start(GTK_BOX(play), hero, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(play), label("Recent", "section"), FALSE, FALSE, 0);
    gchar *log_text = newest_log_text(root);
    GtkWidget *log_card = card("log", "Last log", log_text, &state->last_log); g_free(log_text);
    GtkWidget *log_button = button("Open _log", "log", NULL); g_signal_connect(log_button, "clicked", G_CALLBACK(open_log), state);
    card_add(log_card, log_button); gtk_size_group_add_widget(play_controls, log_button); gtk_box_pack_start(GTK_BOX(play), log_card, FALSE, FALSE, 0);
    GtkWidget *data_card = card("folder", "Data and logs", "Logs can contain account details; review them before sharing.", NULL);
    GtkWidget *data_button = button("Open _folder", NULL, NULL); g_signal_connect(data_button, "clicked", G_CALLBACK(open_data_folder), state);
    card_add(data_card, data_button); gtk_size_group_add_widget(play_controls, data_button); gtk_box_pack_start(GTK_BOX(play), data_card, FALSE, FALSE, 0);
    add_page(state, play, "play");

    /* Updates */
    GtkWidget *updates = page_box("Updates", "The Roblox client and the port itself.");
    GtkWidget *client_top = NULL, *client_actions = NULL;
    GtkWidget *client_card = action_card("download", "Roblox client", "", &state->client_sub, &client_top, &client_actions);
    state->client_pill = pill("…"); card_add(client_top, state->client_pill);
    state->update = button("_Update now", "download", "primary"); g_signal_connect(state->update, "clicked", G_CALLBACK(settings_update), state); gtk_box_pack_start(GTK_BOX(client_actions), state->update, FALSE, FALSE, 0);
    state->check = button("_Check for updates", NULL, NULL); g_signal_connect(state->check, "clicked", G_CALLBACK(settings_check), state); gtk_box_pack_start(GTK_BOX(client_actions), state->check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(updates), client_card, FALSE, FALSE, 0);
    state->message_box = message_area(&state->message); gtk_box_pack_start(GTK_BOX(updates), state->message_box, FALSE, FALSE, 0);
    GtkWidget *port_top = NULL, *port_actions = NULL;
    GtkWidget *port_card = action_card("shield", "Roblox Mac Linux Port", "", &state->port_sub, &port_top, &port_actions);
    state->port_pill = pill("…"); card_add(port_top, state->port_pill);
    state->port_update = button("Update p_ort", "download", "primary"); g_signal_connect(state->port_update, "clicked", G_CALLBACK(settings_port_update), state); gtk_box_pack_start(GTK_BOX(port_actions), state->port_update, FALSE, FALSE, 0);
    state->port_check = button("Check for _port updates", NULL, NULL); g_signal_connect(state->port_check, "clicked", G_CALLBACK(settings_port_check), state); gtk_box_pack_start(GTK_BOX(port_actions), state->port_check, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(updates), port_card, FALSE, FALSE, 0);
    state->port_message_box = message_area(&state->port_message_label); gtk_box_pack_start(GTK_BOX(updates), state->port_message_box, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(updates), label("When updates are found", "section"), FALSE, FALSE, 0);
    static const char *const modes[] = {"ask", "Ask first", "auto", "Automatically", "off", "Never", NULL};
    GtkWidget *mode_card = card("update", "Updates", "Ask before updating, update by itself, or never offer updates.", NULL);
    state->mode = dropdown_new(modes, mode_changed, state); card_add(mode_card, state->mode); gtk_size_group_add_widget(update_controls, state->mode);
    gtk_box_pack_start(GTK_BOX(updates), mode_card, FALSE, FALSE, 0);
    static const char *const intervals[] = {"0", "Every launch", "6", "Every 6 hours", "24", "Once a day", "168", "Once a week", NULL};
    GtkWidget *interval_card = card("clock", "Look for updates", "A check never delays the game: a slow answer is offered at the next start.", NULL);
    state->interval = dropdown_new(intervals, interval_changed, state); card_add(interval_card, state->interval); gtk_size_group_add_widget(update_controls, state->interval);
    gtk_box_pack_start(GTK_BOX(updates), interval_card, FALSE, FALSE, 0);
    GtkWidget *skipped_card = card("close", "Skipped versions", "None", &state->skipped_sub);
    state->forget_skipped = button("_Forget skipped", NULL, NULL); g_signal_connect(state->forget_skipped, "clicked", G_CALLBACK(forget_skipped), state); card_add(skipped_card, state->forget_skipped); gtk_size_group_add_widget(update_controls, state->forget_skipped);
    gtk_box_pack_start(GTK_BOX(updates), skipped_card, FALSE, FALSE, 0);
    add_page(state, updates, "updates");

    /* Appearance */
    GtkWidget *appearance = page_box("Appearance", "How the game window looks on your desktop. Applies on the next launch.");
    static const char *const decorations[] = {"auto", "Automatic", "system", "Desktop title bar", "gtk", "GTK title bar", "legacy", "Legacy compatibility", NULL};
    GtkWidget *decorations_card = card("window", "Window decorations", "Automatic uses your desktop's title bar when it offers one, else GTK's.", NULL);
    state->decorations_combo = dropdown_new(decorations, decorations_changed, state); card_add(decorations_card, state->decorations_combo); gtk_size_group_add_widget(appearance_controls, state->decorations_combo);
    gtk_box_pack_start(GTK_BOX(appearance), decorations_card, FALSE, FALSE, 0);
    GtkWidget *cursor_card = card("cursor", "Desktop cursor", "Use your system cursor theme and size; the game's own cursors still show. Off: Roblox's classic cursor.", NULL);
    state->cursor_switch = gtk_switch_new(); g_signal_connect(state->cursor_switch, "notify::active", G_CALLBACK(cursor_changed), state); card_add(cursor_card, state->cursor_switch);
    gtk_box_pack_start(GTK_BOX(appearance), cursor_card, FALSE, FALSE, 0);
    GtkWidget *reset_card = card("update", "Restore defaults", "Automatic decorations and the desktop cursor.", NULL);
    GtkWidget *reset = button("_Restore defaults", NULL, NULL); g_signal_connect(reset, "clicked", G_CALLBACK(appearance_reset), state); card_add(reset_card, reset); gtk_size_group_add_widget(appearance_controls, reset);
    gtk_box_pack_start(GTK_BOX(appearance), reset_card, FALSE, FALSE, 0);
    GtkWidget *appearance_box = message_area(&state->appearance_message); gtk_box_pack_start(GTK_BOX(appearance), appearance_box, FALSE, FALSE, 0);
    add_page(state, appearance, "appearance");

    /* About */
    GtkWidget *about = page_box("About", "An Intel macOS Roblox client running on Linux through Darling, Wayland and Vulkan.");
    GtkWidget *versions_card = card("about", "Versions", "", &state->about_versions);
    gtk_box_pack_start(GTK_BOX(about), versions_card, FALSE, FALSE, 0);
    GtkWidget *folder_card = card("folder", "Install folder", root, NULL);
    GtkWidget *folder_button = button("Open _install folder", NULL, NULL); g_signal_connect(folder_button, "clicked", G_CALLBACK(open_install_folder), state); card_add(folder_card, folder_button); gtk_size_group_add_widget(about_controls, folder_button);
    gtk_box_pack_start(GTK_BOX(about), folder_card, FALSE, FALSE, 0);
    GtkWidget *project_card = card("shield", "Project page", "github.com/georgenoob1234/Roblox-Mac-Linux-Port", NULL);
    GtkWidget *project_button = button("Open project _page", NULL, NULL); g_signal_connect(project_button, "clicked", G_CALLBACK(open_project), state); card_add(project_card, project_button); gtk_size_group_add_widget(about_controls, project_button);
    gtk_box_pack_start(GTK_BOX(about), project_card, FALSE, FALSE, 0);
    add_page(state, about, "about");

    /* Status bar */
    GtkWidget *statusbar = hbox(10); add_class(statusbar, "statusbar");
    state->status_versions = label(NULL, NULL); gtk_box_pack_start(GTK_BOX(statusbar), state->status_versions, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(statusbar), spacer(), TRUE, TRUE, 0);
    state->status_port = gtk_label_new(""); gtk_box_pack_start(GTK_BOX(statusbar), state->status_port, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(outer), statusbar, FALSE, FALSE, 0);

    g_object_unref(play_controls); g_object_unref(update_controls); g_object_unref(appearance_controls); g_object_unref(about_controls);

    /* A GtkStack ignores a switch to a page that is not visible yet, so the
     * window is shown before the first page is chosen (B10 defect 1). */
    gtk_widget_show_all(state->window);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(settings_page ? updates_nav : play_nav), TRUE);
    gtk_stack_set_visible_child_full(GTK_STACK(state->stack), settings_page ? "updates" : "play", GTK_STACK_TRANSITION_TYPE_NONE);
    settings_refresh(state);
    gtk_main(); if (state->launch_check_id) g_source_remove(state->launch_check_id); g_free(state->root); g_free(state); return 0;
}
