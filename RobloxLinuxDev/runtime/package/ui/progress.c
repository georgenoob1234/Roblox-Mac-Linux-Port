/* The launch window: progress of checking, downloading, installing and
 * starting, in George's placement (phase, detail, size above the bar, the bar,
 * Cancel bottom right). A failure stays on screen with Open log and Close. */

typedef struct {
    char *root, *cancel_file, *cancel_state;
    GtkWidget *window, *phase, *status, *size, *progress, *progress_area, *notice, *notice_text;
    GtkWidget *notice_reveal, *progress_reveal, *error_reveal;
    GtkWidget *error, *error_title, *error_hint, *error_log, *log_button, *cancel;
    /* The latest event, kept so a window that appears after it still shows it. */
    char *last_phase, *last_message, *last_hint, *last_log, *notice_phase;
    gint64 last_percent, last_done, last_total;
    gboolean seen_event;
    /* Set by an event with "hidden": true, which the launcher sends before it
     * opens the update prompt; the next event without it shows the window
     * again (B10 defect 4: the progress window covered the prompt). */
    gboolean hidden;
    /* A "failed" event: the window shows the error and waits for Close. */
    gboolean failed;
    guint show_id, close_id, cancel_poll_id;
    gboolean done, cancellable, cancelled, closed, authoritative;
    int result;
} ProgressState;

/* Feedback to the user's own action (a refused close, a refused or accepted
 * cancel). It has its own line because every progress event rewrites the
 * status line; it stays until the phase changes (B10 defect 3). */
static void progress_notice(ProgressState *state, const char *text) {
    g_free(state->notice_phase); state->notice_phase = g_strdup(state->last_phase ? state->last_phase : "");
    if (!state->notice) return;
    if (text && *text) label_text(state->notice_text, text);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->notice_reveal), text && *text);
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
    /* Open without following symlinks and without blocking on a FIFO, then
     * validate the *opened* descriptor: fstat() on the descriptor (rather than
     * only lstat() on the path) means a concurrent replacement cannot slip a
     * FIFO or a foreign file past a check/use window. */
    int fd = open(state_path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return CANCEL_STATE_UNKNOWN;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != getuid()) {
        close(fd);
        return CANCEL_STATE_UNKNOWN;
    }
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
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK, 0600);
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
        if (button) { button_set_text(button, "Cancelling…"); gtk_widget_set_sensitive(button, FALSE); }
        progress_notice(state, "Cancelling…");
        return;
    }
    /* Never claim the request was accepted unless it was recorded under a valid
     * "open" state. Disable further attempts and report the real outcome. */
    state->cancellable = FALSE;
    if (button) gtk_widget_set_sensitive(button, FALSE);
    if (result == CANCEL_REFUSED)
        progress_notice(state, "This update is already being applied and can no longer be cancelled.");
    else
        progress_notice(state, "Cancellation is unavailable; the update status could not be verified safely.");
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
                progress_notice(state, "Cancellation is unavailable; the update status could not be verified safely.");
        }
    }
    return G_SOURCE_CONTINUE;
}
static void progress_cancel(GtkButton *button, gpointer data) {
    ProgressState *state = data;
    if (state->failed) { progress_close(state, 1); return; }
    progress_request_cancel(state, GTK_WIDGET(button));
}
static gboolean progress_delete(GtkWidget *window, GdkEvent *event, gpointer data) {
    ProgressState *state = data;
    if (state->failed) { progress_close(state, 1); return TRUE; }
    if (state->cancelled)
        progress_notice(state, "Cancelling… The window closes as soon as the current step has stopped.");
    else if (state->cancellable)
        progress_request_cancel(state, state->cancel);
    else
        progress_notice(state, "This step can't be cancelled safely. The window closes by itself when it is done.");
    return TRUE;
}

/* What each phase is called on screen. */
static const char *phase_title(const char *phase) {
    static const struct { const char *phase, *title; } titles[] = {
        {"checking", "Checking for updates"}, {"first-run", "Setting up"}, {"downloading", "Downloading"},
        {"verifying", "Verifying"}, {"preparing", "Preparing shaders"}, {"staging", "Installing"},
        {"installing", "Installing"}, {"complete", "Finishing"}, {"update", "Update complete"},
        {"launching", "Starting Roblox"}, {"cancelled", "Cancelled"}, {"prompt", "Waiting for your answer"},
    };
    for (size_t i = 0; i < G_N_ELEMENTS(titles); ++i)
        if (!g_strcmp0(phase, titles[i].phase)) return titles[i].title;
    return NULL;
}
static char *megabytes(gint64 bytes) {
    double mb = bytes / 1048576.0;
    return mb < 10 ? g_strdup_printf("%.1f MB", mb) : g_strdup_printf("%.0f MB", mb);
}
static void open_log_path(GtkButton *button, gpointer data) {
    ProgressState *state = data;
    if (!state->last_log || !*state->last_log) return;
    gchar *uri = g_filename_to_uri(state->last_log, NULL, NULL);
    gchar *argv[] = {(gchar *)"xdg-open", uri, NULL};
    gchar **environment = host_environment();
    if (uri) g_spawn_async(NULL, argv, environment, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL);
    g_strfreev(environment); g_free(uri);
}

static void progress_apply(ProgressState *state);
static gboolean progress_show(gpointer data) {
    ProgressState *state = data; state->show_id = 0; if (state->done || state->hidden) return G_SOURCE_REMOVE;
    state->window = themed_window("Roblox", state->root, TRUE, FALSE);
    setup_ui_identity(state->window, state->root);
    gtk_window_set_default_size(GTK_WINDOW(state->window), 460, -1); gtk_window_set_resizable(GTK_WINDOW(state->window), FALSE); gtk_window_set_position(GTK_WINDOW(state->window), GTK_WIN_POS_CENTER);
    g_signal_connect(state->window, "delete-event", G_CALLBACK(progress_delete), state);
    GtkWidget *box = vbox(0); gtk_container_set_border_width(GTK_CONTAINER(box), 0);
    gtk_widget_set_margin_start(box, 20); gtk_widget_set_margin_end(box, 20); gtk_widget_set_margin_top(box, 18); gtk_widget_set_margin_bottom(box, 16);
    gtk_widget_set_size_request(box, 420, -1);
    gtk_container_add(GTK_CONTAINER(state->window), box);
    state->phase = label("Preparing", "phase"); gtk_box_pack_start(GTK_BOX(box), state->phase, FALSE, FALSE, 0);
    /* The compact layout of George's placement. The yellow note and the red
     * failure box slide in and out (GtkRevealer), and the window's height
     * follows them smoothly; its width never changes (George, 2026-11-15). */
    state->status = label("Setting up Roblox…", "detail");
    gtk_widget_set_margin_top(state->status, 2);
    gtk_box_pack_start(GTK_BOX(box), state->status, FALSE, FALSE, 0);

    state->notice_reveal = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->notice_reveal), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->notice_reveal), 220);
    state->notice = hbox(8); add_class(state->notice, "notice"); gtk_widget_set_margin_top(state->notice, 12);
    GtkWidget *notice_icon = icon("warning", 15); gtk_widget_set_valign(notice_icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(state->notice), notice_icon, FALSE, FALSE, 0);
    state->notice_text = label(NULL, NULL);
    gtk_box_pack_start(GTK_BOX(state->notice), state->notice_text, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(state->notice_reveal), state->notice);
    gtk_box_pack_start(GTK_BOX(box), state->notice_reveal, FALSE, FALSE, 0);

    state->progress_reveal = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->progress_reveal), GTK_REVEALER_TRANSITION_TYPE_SLIDE_UP);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->progress_reveal), 220);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->progress_reveal), TRUE);
    state->progress_area = vbox(6); gtk_widget_set_margin_top(state->progress_area, 14);
    state->size = gtk_label_new(""); add_class(state->size, "faint"); gtk_widget_set_halign(state->size, GTK_ALIGN_END);
    gtk_box_pack_start(GTK_BOX(state->progress_area), state->size, FALSE, FALSE, 0);
    state->progress = gtk_progress_bar_new(); gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(state->progress), FALSE);
    gtk_box_pack_start(GTK_BOX(state->progress_area), state->progress, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(state->progress_reveal), state->progress_area);
    gtk_box_pack_start(GTK_BOX(box), state->progress_reveal, FALSE, FALSE, 0);

    state->error_reveal = gtk_revealer_new();
    gtk_revealer_set_transition_type(GTK_REVEALER(state->error_reveal), GTK_REVEALER_TRANSITION_TYPE_SLIDE_DOWN);
    gtk_revealer_set_transition_duration(GTK_REVEALER(state->error_reveal), 260);
    state->error = hbox(10); add_class(state->error, "error-box"); gtk_widget_set_margin_top(state->error, 4);
    GtkWidget *error_icon = icon("warning", 18); gtk_widget_set_valign(error_icon, GTK_ALIGN_START);
    gtk_box_pack_start(GTK_BOX(state->error), error_icon, FALSE, FALSE, 0);
    GtkWidget *error_text = vbox(3);
    state->error_title = label(NULL, "error-title");
    state->error_hint = label(NULL, NULL);
    /* The log path on one line, shortened in the middle; all of it in the
     * tooltip and behind Open log. */
    state->error_log = gtk_label_new(NULL); add_class(state->error_log, "path");
    gtk_widget_set_halign(state->error_log, GTK_ALIGN_START); gtk_label_set_xalign(GTK_LABEL(state->error_log), 0);
    gtk_label_set_ellipsize(GTK_LABEL(state->error_log), PANGO_ELLIPSIZE_MIDDLE); gtk_label_set_selectable(GTK_LABEL(state->error_log), TRUE);
    gtk_box_pack_start(GTK_BOX(error_text), state->error_title, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(error_text), state->error_hint, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(error_text), state->error_log, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(state->error), error_text, TRUE, TRUE, 0);
    gtk_container_add(GTK_CONTAINER(state->error_reveal), state->error);
    gtk_box_pack_start(GTK_BOX(box), state->error_reveal, FALSE, FALSE, 0);

    GtkWidget *foot = hbox(10); gtk_widget_set_margin_top(foot, 16);
    state->log_button = button("Open _log", "log", "ghost"); g_signal_connect(state->log_button, "clicked", G_CALLBACK(open_log_path), state);
    gtk_box_pack_start(GTK_BOX(foot), state->log_button, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(foot), spacer(), TRUE, TRUE, 0);
    state->cancel = button("_Cancel", NULL, NULL); g_signal_connect(state->cancel, "clicked", G_CALLBACK(progress_cancel), state);
    gtk_box_pack_start(GTK_BOX(foot), state->cancel, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), foot, FALSE, FALSE, 0);
    gtk_widget_set_sensitive(state->cancel, FALSE); gtk_widget_show_all(state->window);
    gtk_widget_set_no_show_all(state->log_button, TRUE); gtk_widget_hide(state->log_button);
    if (state->seen_event) progress_apply(state);
    state->cancel_poll_id = g_timeout_add(CANCEL_POLL_MS, progress_poll_cancel, state);
    return G_SOURCE_REMOVE;
}
static void progress_apply(ProgressState *state) {
    const char *phase = state->last_phase ? state->last_phase : "", *message = state->last_message ? state->last_message : "";
    gint64 percent = state->last_percent, done = state->last_done, total = state->last_total;
    if (state->failed) {
        /* "WHAT: WHY" from the launcher: WHAT is the title, WHY the box's
         * first line (not repeated: the title already says what failed). */
        const char *colon = strstr(message, ": ");
        char *what = colon ? g_strndup(message, colon - message) : g_strdup(*message ? message : "Something went wrong");
        const char *why = colon ? colon + 2 : message;
        char *full = g_strdup_printf("%s%s", why, *why && !g_str_has_suffix(why, ".") ? "." : "");
        if (full[0]) full[0] = g_ascii_toupper(full[0]);
        label_text(state->phase, what); label_text(state->error_title, full);
        label_text(state->error_hint, state->last_hint);
        label_text(state->error_log, state->last_log);
        gtk_widget_set_tooltip_text(state->error_log, state->last_log);
        gtk_widget_set_visible(state->error_hint, state->last_hint && *state->last_hint);
        gtk_widget_set_visible(state->error_log, state->last_log && *state->last_log);
        gtk_widget_hide(state->status);
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->progress_reveal), FALSE);
        gtk_revealer_set_reveal_child(GTK_REVEALER(state->error_reveal), TRUE);
        gtk_widget_set_visible(state->log_button, state->last_log && *state->last_log);
        button_set_text(state->cancel, "_Close"); gtk_widget_set_sensitive(state->cancel, TRUE);
        progress_notice(state, NULL);
        g_free(what); g_free(full);
        return;
    }
    gtk_widget_show(state->status);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->error_reveal), FALSE);
    gtk_revealer_set_reveal_child(GTK_REVEALER(state->progress_reveal), TRUE);
    gtk_widget_hide(state->log_button);
    if (!state->cancelled) button_set_text(state->cancel, "_Cancel");
    const char *title = phase_title(phase);
    if (title) label_text(state->phase, title);
    else if (*phase) { char *text = g_strdup(phase); text[0] = g_ascii_toupper(text[0]); for (char *c = text; *c; ++c) if (*c == '-') *c = ' '; label_text(state->phase, text); g_free(text); }
    if (*message) label_text(state->status, message);
    /* A note answers the user's last action and goes when the phase moves on,
     * except a pending cancel: that stays until the launcher ends the run. */
    if (state->notice && gtk_revealer_get_reveal_child(GTK_REVEALER(state->notice_reveal)) && !state->cancelled && g_strcmp0(state->notice_phase, phase)) progress_notice(state, NULL);
    if (!state->cancelled) gtk_widget_set_sensitive(state->cancel, state->cancellable);
    if (percent >= 0) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(state->progress), percent / 100.0);
    else gtk_progress_bar_pulse(GTK_PROGRESS_BAR(state->progress));
    if (total > 0) {
        char *have = megabytes(done), *all = megabytes(total), *text = g_strdup_printf("%s / %s", have, all);
        label_text(state->size, text); g_free(have); g_free(all); g_free(text);
    } else if (percent >= 0) { char *text = g_strdup_printf("%lld%%", (long long)percent); label_text(state->size, text); g_free(text); }
    else label_text(state->size, "");
}
static void progress_event(ProgressState *state, const char *line) {
    JsonObject *object = parse_json(line); if (!object) return;
    const char *message = json_string(object, "message"); const char *phase = json_string(object, "phase");
    state->cancellable = json_bool(object, "cancellable");
    if (*phase) { g_free(state->last_phase); state->last_phase = g_strdup(phase); }
    if (*message) { g_free(state->last_message); state->last_message = g_strdup(message); }
    g_free(state->last_hint); state->last_hint = g_strdup(json_string(object, "hint"));
    g_free(state->last_log); state->last_log = g_strdup(json_string(object, "log"));
    state->last_percent = json_int(object, "percent"); state->last_done = json_int(object, "bytes_done"); state->last_total = json_int(object, "bytes_total");
    state->seen_event = TRUE;
    gboolean hidden = json_bool(object, "hidden");
    if (hidden && !state->hidden) {
        state->hidden = TRUE;
        if (state->window) gtk_widget_hide(state->window);
    } else if (!hidden && state->hidden) {
        state->hidden = FALSE;
        if (state->window) gtk_widget_show(state->window);
        /* The show delay may have passed while hidden: show at once, since
         * the user has just answered the prompt and expects the next step. */
        else if (!state->show_id && !state->done) state->show_id = g_idle_add(progress_show, state);
    }
    /* A failure stays on screen until the user closes it (George, B10
     * phase 3). A later event means the launch went on after all. */
    state->failed = !g_strcmp0(phase, "failed");
    if (state->failed) {
        state->result = 1;
        if (!state->window && !state->show_id) state->show_id = g_idle_add(progress_show, state);
        else if (!state->window && state->show_id) { g_source_remove(state->show_id); state->show_id = g_idle_add(progress_show, state); }
    }
    if (state->window) progress_apply(state);
    if (!g_strcmp0(phase, "launching") || !g_strcmp0(phase, "update") || !g_strcmp0(phase, "cancelled")) {
        state->done = TRUE;
        /* The producer owns the outcome. A late or rejected cancellation
         * request must not turn a reported success into a local "cancelled". */
        state->authoritative = TRUE;
        state->result = !g_strcmp0(phase, "cancelled") ? 4 : 0;
        if (state->window && gtk_widget_get_visible(state->window)) { if (!state->close_id) state->close_id = g_timeout_add(350, progress_hide, state); }
        else progress_close(state, state->result);
    }
    json_object_unref(object);
}
static gboolean progress_input(GIOChannel *channel, GIOCondition condition, gpointer data) {
    ProgressState *state = data;
    /* Read before honouring a hang-up. A producer that writes its final event
     * and closes the pipe at once makes poll() report the data and the hang-up
     * together, and the outcome is in that data. */
    if (!(condition & (G_IO_ERR | G_IO_NVAL))) {
        GIOStatus result;
        do {
            gchar *line = NULL; gsize length = 0; GError *error = NULL;
            result = g_io_channel_read_line(channel, &line, &length, NULL, &error); g_clear_error(&error);
            if (result == G_IO_STATUS_NORMAL) progress_event(state, line);
            g_free(line);
            if (state->closed) return G_SOURCE_REMOVE;
        } while (result == G_IO_STATUS_NORMAL);
        /* No complete line yet and the producer is still there: keep waiting. */
        if (result == G_IO_STATUS_AGAIN && !(condition & G_IO_HUP)) return G_SOURCE_CONTINUE;
    }
    if (state->done) return G_SOURCE_REMOVE;
    /* A shown failure waits for the user's Close, not for the producer. */
    if (state->failed) { state->authoritative = TRUE; return G_SOURCE_REMOVE; }
    progress_close(state, state->cancelled ? 4 : state->result);
    return G_SOURCE_REMOVE;
}
static int run_progress(const char *root) {
    if (!ui_init()) return 3;
    ProgressState *state = g_new0(ProgressState, 1); state->result = 1; state->root = g_strdup(root); state->cancel_file = g_build_filename(root, ".bootstrap-cancel", NULL); state->cancel_state = g_build_filename(root, ".bootstrap-cancel-state", NULL);
    state->show_id = g_timeout_add(UI_DELAY_MS, progress_show, state); GIOChannel *channel = g_io_channel_unix_new(STDIN_FILENO); g_io_channel_set_encoding(channel, NULL, NULL); g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL); g_io_add_watch(channel, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_NVAL, progress_input, state); gtk_main();
    int result = state->authoritative ? state->result : (state->cancelled ? 4 : state->result); g_io_channel_unref(channel); g_free(state->root); g_free(state->cancel_file); g_free(state->cancel_state); g_free(state->last_phase); g_free(state->last_message); g_free(state->last_hint); g_free(state->last_log); g_free(state->notice_phase); g_free(state); return result;
}
