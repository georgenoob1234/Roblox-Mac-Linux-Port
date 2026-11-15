/* Test-only control channel, off unless ROBLOX_MAC_UI_TEST_IN (a FIFO) and
 * ROBLOX_MAC_UI_TEST_OUT (a file) are both set. Checks use it to read what the
 * windows show and to act on them the way a user would, without guessing pixel
 * positions. One command per line; one JSON line answers each:
 *   dump           every mapped window: title, and its mapped widgets in order
 *   click TEXT     press the mapped button labelled TEXT; refused if insensitive
 *   close [TITLE]  ask the (first, or named) window to close, as its X does */
static FILE *ui_test_out;

static char *ui_test_plain(const char *text) {
    GString *plain = g_string_new(NULL);
    for (const char *c = text ? text : ""; *c; ++c) {
        if (*c == '_' && c[1] == '_') { g_string_append_c(plain, '_'); ++c; }
        else if (*c != '_') g_string_append_c(plain, *c);
    }
    return g_string_free(plain, FALSE);
}
/* A button's name: its own label, else the text of the label inside it
 * (buttons with an icon carry their text in a label next to the icon). */
static void ui_test_find_label(GtkWidget *widget, gpointer data) {
    const char **text = data;
    if (*text) return;
    if (GTK_IS_LABEL(widget)) *text = gtk_label_get_label(GTK_LABEL(widget));
    else if (GTK_IS_CONTAINER(widget)) gtk_container_forall(GTK_CONTAINER(widget), ui_test_find_label, data);
}
static char *ui_test_button_name(GtkWidget *widget) {
    const char *text = gtk_button_get_label(GTK_BUTTON(widget));
    if (!text) ui_test_find_label(widget, &text);
    return ui_test_plain(text);
}
static void ui_test_reply(JsonBuilder *builder) {
    JsonGenerator *generator = json_generator_new();
    JsonNode *root = json_builder_get_root(builder);
    json_generator_set_root(generator, root);
    char *text = json_generator_to_data(generator, NULL);
    fprintf(ui_test_out, "%s\n", text); fflush(ui_test_out);
    g_free(text); json_node_unref(root); g_object_unref(generator); g_object_unref(builder);
}
static void ui_test_simple(gboolean ok, const char *error) {
    JsonBuilder *builder = json_builder_new(); json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ok"); json_builder_add_boolean_value(builder, ok);
    if (error) { json_builder_set_member_name(builder, "error"); json_builder_add_string_value(builder, error); }
    json_builder_end_object(builder); ui_test_reply(builder);
}
static void ui_test_widget(GtkWidget *widget, gpointer data) {
    JsonBuilder *builder = data;
    if (!gtk_widget_get_mapped(widget)) return;
    const char *type = NULL;
    if (GTK_IS_LABEL(widget) && !GTK_IS_ACCEL_LABEL(widget)) type = "label";
    else if (GTK_IS_TOGGLE_BUTTON(widget)) type = "toggle";
    else if (GTK_IS_BUTTON(widget)) type = "button";
    else if (GTK_IS_PROGRESS_BAR(widget)) type = "progress";
    else if (GTK_IS_STACK(widget)) type = "stack";
    else if (GTK_IS_COMBO_BOX(widget)) type = "combo";
    else if (GTK_IS_SPIN_BUTTON(widget)) type = "spin";
    else if (GTK_IS_SWITCH(widget)) type = "switch";
    if (type) {
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "type"); json_builder_add_string_value(builder, type);
        json_builder_set_member_name(builder, "sensitive"); json_builder_add_boolean_value(builder, gtk_widget_is_sensitive(widget));
        int minimum = 0, natural = 0; gtk_widget_get_preferred_width(widget, &minimum, &natural);
        json_builder_set_member_name(builder, "wanted_width"); json_builder_add_int_value(builder, natural);
        if (GTK_IS_LABEL(widget)) {
            json_builder_set_member_name(builder, "text"); json_builder_add_string_value(builder, gtk_label_get_text(GTK_LABEL(widget)));
        } else if (GTK_IS_BUTTON(widget)) {
            char *text = ui_test_button_name(widget);
            json_builder_set_member_name(builder, "text"); json_builder_add_string_value(builder, text); g_free(text);
            if (GTK_IS_TOGGLE_BUTTON(widget)) { json_builder_set_member_name(builder, "active"); json_builder_add_boolean_value(builder, gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(widget))); }
        } else if (GTK_IS_PROGRESS_BAR(widget)) {
            json_builder_set_member_name(builder, "fraction"); json_builder_add_double_value(builder, gtk_progress_bar_get_fraction(GTK_PROGRESS_BAR(widget)));
            json_builder_set_member_name(builder, "text"); json_builder_add_string_value(builder, gtk_progress_bar_get_text(GTK_PROGRESS_BAR(widget)) ? gtk_progress_bar_get_text(GTK_PROGRESS_BAR(widget)) : "");
        } else if (GTK_IS_STACK(widget)) {
            const char *name = gtk_stack_get_visible_child_name(GTK_STACK(widget));
            json_builder_set_member_name(builder, "visible"); json_builder_add_string_value(builder, name ? name : "");
        } else if (GTK_IS_SPIN_BUTTON(widget)) {
            json_builder_set_member_name(builder, "value"); json_builder_add_double_value(builder, gtk_spin_button_get_value(GTK_SPIN_BUTTON(widget)));
        } else if (GTK_IS_COMBO_BOX(widget)) {
            const char *id = gtk_combo_box_get_active_id(GTK_COMBO_BOX(widget));
            json_builder_set_member_name(builder, "active"); json_builder_add_string_value(builder, id ? id : "");
        } else if (GTK_IS_SWITCH(widget)) {
            json_builder_set_member_name(builder, "active"); json_builder_add_boolean_value(builder, gtk_switch_get_active(GTK_SWITCH(widget)));
        }
        json_builder_end_object(builder);
    }
    /* A button's own label and a combo's or spin's internals are part of it. */
    if (GTK_IS_CONTAINER(widget) && !GTK_IS_BUTTON(widget) && !GTK_IS_COMBO_BOX(widget))
        gtk_container_forall(GTK_CONTAINER(widget), ui_test_widget, builder);
}
static GList *ui_test_windows(void) {
    GList *windows = NULL, *all = gtk_window_list_toplevels();
    for (GList *item = all; item; item = item->next)
        if (gtk_window_get_window_type(item->data) == GTK_WINDOW_TOPLEVEL && gtk_widget_get_mapped(item->data))
            windows = g_list_append(windows, item->data);
    g_list_free(all);
    return windows;
}
static void ui_test_dump(void) {
    JsonBuilder *builder = json_builder_new(); json_builder_begin_object(builder);
    json_builder_set_member_name(builder, "ok"); json_builder_add_boolean_value(builder, TRUE);
    json_builder_set_member_name(builder, "windows"); json_builder_begin_array(builder);
    GList *windows = ui_test_windows();
    for (GList *item = windows; item; item = item->next) {
        const char *title = gtk_window_get_title(item->data);
        json_builder_begin_object(builder);
        json_builder_set_member_name(builder, "title"); json_builder_add_string_value(builder, title ? title : "");
        int width = 0, height = 0; gtk_window_get_size(item->data, &width, &height);
        json_builder_set_member_name(builder, "width"); json_builder_add_int_value(builder, width);
        json_builder_set_member_name(builder, "height"); json_builder_add_int_value(builder, height);
        /* The width the window's content asks for (the window's own figure
         * adds its shadow). Some displays do not resize a window to it
         * (broadway), others do (KDE Wayland), so checks look at this. */
        int minimum = 0, natural = 0;
        GtkWidget *content = gtk_bin_get_child(GTK_BIN(item->data));
        if (content) gtk_widget_get_preferred_width(content, &minimum, &natural);
        json_builder_set_member_name(builder, "wanted_width"); json_builder_add_int_value(builder, natural);
        json_builder_set_member_name(builder, "widgets"); json_builder_begin_array(builder);
        gtk_container_forall(GTK_CONTAINER(item->data), ui_test_widget, builder);
        json_builder_end_array(builder); json_builder_end_object(builder);
    }
    g_list_free(windows);
    json_builder_end_array(builder); json_builder_end_object(builder); ui_test_reply(builder);
}
typedef struct { const char *text; GtkWidget *found; } UiTestFind;
static void ui_test_find_button(GtkWidget *widget, gpointer data) {
    UiTestFind *find = data;
    if (find->found || !gtk_widget_get_mapped(widget)) return;
    if (GTK_IS_MENU_ITEM(widget)) {
        char *text = ui_test_plain(gtk_menu_item_get_label(GTK_MENU_ITEM(widget)));
        gboolean match = !g_strcmp0(text, find->text); g_free(text);
        if (match) find->found = widget;
        return;
    }
    if (GTK_IS_BUTTON(widget)) {
        char *text = ui_test_button_name(widget);
        gboolean match = !g_strcmp0(text, find->text); g_free(text);
        if (match) { find->found = widget; return; }
    }
    if (GTK_IS_CONTAINER(widget)) gtk_container_forall(GTK_CONTAINER(widget), ui_test_find_button, find);
}
static void ui_test_command(const char *line) {
    if (!g_strcmp0(line, "dump")) { ui_test_dump(); return; }
    GList *windows = ui_test_windows();
    if (g_str_has_prefix(line, "click ")) {
        UiTestFind find = {line + 6, NULL};
        /* Open menus are popup windows: search them first. */
        GList *all = gtk_window_list_toplevels();
        for (GList *item = all; item && !find.found; item = item->next)
            if (gtk_window_get_window_type(item->data) == GTK_WINDOW_POPUP && gtk_widget_get_mapped(item->data))
                gtk_container_forall(GTK_CONTAINER(item->data), ui_test_find_button, &find);
        g_list_free(all);
        for (GList *item = windows; item && !find.found; item = item->next)
            gtk_container_forall(GTK_CONTAINER(item->data), ui_test_find_button, &find);
        if (!find.found) ui_test_simple(FALSE, "no such button");
        else if (!gtk_widget_is_sensitive(find.found)) ui_test_simple(FALSE, "insensitive");
        else if (GTK_IS_MENU_ITEM(find.found)) {
            ui_test_simple(TRUE, NULL);
            GtkWidget *shell = gtk_widget_get_parent(find.found);
            gtk_menu_item_activate(GTK_MENU_ITEM(find.found));
            if (GTK_IS_MENU_SHELL(shell)) gtk_menu_shell_deactivate(GTK_MENU_SHELL(shell));
        } else { ui_test_simple(TRUE, NULL); gtk_button_clicked(GTK_BUTTON(find.found)); }
    } else if (!g_strcmp0(line, "close") || g_str_has_prefix(line, "close ")) {
        const char *title = line[5] ? line + 6 : NULL;
        GtkWidget *target = NULL;
        for (GList *item = windows; item && !target; item = item->next)
            if (!title || !g_strcmp0(gtk_window_get_title(item->data), title)) target = item->data;
        if (!target) ui_test_simple(FALSE, "no such window");
        else { ui_test_simple(TRUE, NULL); gtk_window_close(GTK_WINDOW(target)); }
    } else ui_test_simple(FALSE, "unknown command");
    g_list_free(windows);
}
static gboolean ui_test_input(GIOChannel *channel, GIOCondition condition, gpointer data) {
    GIOStatus result;
    do {
        gchar *line = NULL; gsize length = 0;
        result = g_io_channel_read_line(channel, &line, &length, NULL, NULL);
        if (result == G_IO_STATUS_NORMAL && line) { g_strchomp(line); ui_test_command(line); }
        g_free(line);
    } while (result == G_IO_STATUS_NORMAL);
    return G_SOURCE_CONTINUE;
}
static void ui_test_init(void) {
    const char *in = g_getenv("ROBLOX_MAC_UI_TEST_IN"), *out = g_getenv("ROBLOX_MAC_UI_TEST_OUT");
    if (!in || !*in || !out || !*out || ui_test_out) return;
    /* Read-write so the FIFO never reports end of file between test commands. */
    int fd = open(in, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    ui_test_out = fopen(out, "ae");
    if (!ui_test_out) { close(fd); return; }
    GIOChannel *channel = g_io_channel_unix_new(fd);
    g_io_channel_set_encoding(channel, NULL, NULL);
    g_io_channel_set_flags(channel, G_IO_FLAG_NONBLOCK, NULL);
    g_io_add_watch(channel, G_IO_IN, ui_test_input, NULL);
    g_io_channel_unref(channel);
}
