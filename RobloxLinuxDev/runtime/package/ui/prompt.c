/* The update prompt: one window for every update that is ready (B10 flow 7-9). */

typedef struct { GtkWidget *window; int result; } DialogState;

/* Prompt answers: 0 update now, 1 launch without updating, 2 skip this
 * version, 4 cancel the launch (also closing the window). There is no
 * timer: the prompt waits for the user (B10 flow 7 and 8). */
enum { PROMPT_UPDATE = 0, PROMPT_LAUNCH = 1, PROMPT_SKIP = 2, PROMPT_CANCEL = 4 };
static void dialog_finish(DialogState *state, int result) { state->result = result; gtk_main_quit(); }
static gboolean dialog_delete(GtkWidget *window, GdkEvent *event, gpointer data) { dialog_finish(data, PROMPT_CANCEL); return TRUE; }
static void dialog_button(GtkWidget *button, gpointer data) { dialog_finish(data, GPOINTER_TO_INT(g_object_get_data(G_OBJECT(button), "result"))); }

static GtkWidget *update_row(const char *icon_name, const char *name, const char *installed, const char *latest) {
    GtkWidget *sub = NULL;
    char *versions = g_strdup_printf("%s → %s", installed && *installed ? installed : "not installed", latest);
    GtkWidget *row = card(icon_name, name, versions, &sub);
    g_free(versions);
    return row;
}

/* The update prompt lists every update that is ready: the Roblox client
 * (LATEST/INSTALLED) and the port (PORT_LATEST/PORT_INSTALLED); an empty
 * LATEST means no update of that kind (B10 flow 9: one prompt for both).
 * With no client installed (an empty INSTALLED) the prompt offers only
 * Install and Cancel: skipping it or launching without it cannot work. */
static int run_dialog(const char *latest, const char *installed, const char *port_latest, const char *port_installed) {
    if (!ui_init()) return 3;
    gboolean client = latest && *latest, port = port_latest && *port_latest;
    gboolean install = client && !(installed && *installed);
    DialogState state = {0};
    state.window = themed_window(install ? "Install Roblox" : client && port ? "Updates available" : (port ? "Port update available" : "Roblox update available"), NULL, FALSE, FALSE);
    setup_ui_identity(state.window, NULL);
    gtk_window_set_resizable(GTK_WINDOW(state.window), FALSE); gtk_window_set_modal(GTK_WINDOW(state.window), TRUE); gtk_window_set_position(GTK_WINDOW(state.window), GTK_WIN_POS_CENTER);
    g_signal_connect(state.window, "delete-event", G_CALLBACK(dialog_delete), &state);
    GtkWidget *box = vbox(0);
    gtk_widget_set_margin_start(box, 22); gtk_widget_set_margin_end(box, 22); gtk_widget_set_margin_top(box, 20); gtk_widget_set_margin_bottom(box, 18);
    gtk_widget_set_size_request(box, 596, -1);
    gtk_container_add(GTK_CONTAINER(state.window), box);
    gtk_box_pack_start(GTK_BOX(box), label(install ? "Roblox is not installed yet" : client && port ? "Updates are ready" : (port ? "A port update is ready" : "A Roblox update is ready"), "page-title"), FALSE, FALSE, 0);
    GtkWidget *sub = label(install ? (port ? "Install it to play; the port update is installed with it." : "Install it to play.")
                                   : "Install now, or play on the versions you have.", "muted");
    gtk_widget_set_margin_top(sub, 2); gtk_widget_set_margin_bottom(sub, 16);
    gtk_box_pack_start(GTK_BOX(box), sub, FALSE, FALSE, 0);
    GtkWidget *rows = vbox(8);
    if (client) gtk_box_pack_start(GTK_BOX(rows), update_row("download", "Roblox", installed, latest), FALSE, FALSE, 0);
    if (port) gtk_box_pack_start(GTK_BOX(rows), update_row("shield", "Roblox Mac Linux Port", port_installed && *port_installed ? port_installed : "unknown", port_latest), FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(box), rows, FALSE, FALSE, 0);
    GtkWidget *foot = hbox(8); gtk_widget_set_margin_top(foot, 16);
    struct { const char *text, *icon, *style; int result; } buttons[] = {
        {"_Skip this version", NULL, "ghost", PROMPT_SKIP}, {NULL, NULL, NULL, -1},
        {"_Cancel", NULL, NULL, PROMPT_CANCEL}, {"_Launch without updating", NULL, NULL, PROMPT_LAUNCH},
        {"_Update now", "download", "primary", PROMPT_UPDATE},
    };
    if (install) buttons[0].text = NULL, buttons[3].text = NULL, buttons[4].text = "_Install";
    GtkWidget *focus = NULL;
    for (size_t i = 0; i < G_N_ELEMENTS(buttons); ++i) {
        if (!buttons[i].text) {
            if (buttons[i].result == -1) gtk_box_pack_start(GTK_BOX(foot), spacer(), TRUE, TRUE, 0);
            continue;
        }
        GtkWidget *widget = button(buttons[i].text, buttons[i].icon, buttons[i].style);
        g_object_set_data(G_OBJECT(widget), "result", GINT_TO_POINTER(buttons[i].result));
        g_signal_connect(widget, "clicked", G_CALLBACK(dialog_button), &state);
        gtk_box_pack_start(GTK_BOX(foot), widget, FALSE, FALSE, 0);
        if (buttons[i].result == (install ? PROMPT_UPDATE : PROMPT_LAUNCH)) focus = widget;
    }
    gtk_box_pack_start(GTK_BOX(box), foot, FALSE, FALSE, 0);
    GtkWidget *hint = label(install ? "Cancel: nothing is installed and Roblox does not start." : client && port ? "Skip this version: these two versions are not offered again; newer ones are. Cancel: nothing is updated and Roblox does not start."
                                           : "Skip this version: this version is not offered again; newer ones are. Cancel: nothing is updated and Roblox does not start.", "faint");
    gtk_widget_set_margin_top(hint, 10);
    gtk_box_pack_start(GTK_BOX(box), hint, FALSE, FALSE, 0);
    gtk_widget_show_all(state.window);
    if (focus) gtk_widget_grab_focus(focus);
    gtk_main(); gtk_widget_destroy(state.window); return state.result;
}
