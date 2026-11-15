/* Shared helpers, icons, window chrome and the style for every window. */

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
    GtkWidget *widget = gtk_label_new(text); gtk_widget_set_halign(widget, GTK_ALIGN_START); gtk_label_set_xalign(GTK_LABEL(widget), 0);
    if (class_name) gtk_style_context_add_class(gtk_widget_get_style_context(widget), class_name);
    /* Wrap instead of asking for the whole text's width (see fit_text). */
    gtk_label_set_line_wrap(GTK_LABEL(widget), TRUE); gtk_label_set_max_width_chars(GTK_LABEL(widget), 56); return widget;
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

/* ---- Icons: alpha masks from ui/icons.h, painted in the widget's CSS colour ---- */
static cairo_status_t icon_read(void *closure, unsigned char *data, unsigned int length) {
    const unsigned char **cursor = closure;
    memcpy(data, *cursor, length); *cursor += length;
    return CAIRO_STATUS_SUCCESS;
}
static cairo_surface_t *icon_surface(const char *name) {
    static GHashTable *cache;
    if (!cache) cache = g_hash_table_new(g_str_hash, g_str_equal);
    cairo_surface_t *surface = g_hash_table_lookup(cache, name);
    if (surface) return surface;
    for (size_t i = 0; i < G_N_ELEMENTS(UI_ICONS); ++i) {
        if (strcmp(UI_ICONS[i].name, name)) continue;
        const unsigned char *cursor = UI_ICONS[i].png;
        surface = cairo_image_surface_create_from_png_stream(icon_read, &cursor);
        g_hash_table_insert(cache, (gpointer)UI_ICONS[i].name, surface);
        return surface;
    }
    return NULL;
}
static gboolean icon_draw(GtkWidget *widget, cairo_t *cr, gpointer data) {
    cairo_surface_t *surface = icon_surface(data);
    if (!surface || cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) return FALSE;
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    GdkRGBA color; gtk_style_context_get_color(context, gtk_style_context_get_state(context), &color);
    int width = gtk_widget_get_allocated_width(widget), height = gtk_widget_get_allocated_height(widget);
    int size = MIN(width, height);
    cairo_save(cr);
    cairo_translate(cr, (width - size) / 2.0, (height - size) / 2.0);
    cairo_scale(cr, (double)size / UI_ICON_SIZE, (double)size / UI_ICON_SIZE);
    gdk_cairo_set_source_rgba(cr, &color);
    cairo_pattern_t *mask = cairo_pattern_create_for_surface(surface);
    cairo_pattern_set_filter(mask, CAIRO_FILTER_GOOD);
    cairo_mask(cr, mask);
    cairo_pattern_destroy(mask);
    cairo_restore(cr);
    return FALSE;
}
static GtkWidget *icon(const char *name, int size) {
    GtkWidget *area = gtk_drawing_area_new();
    gtk_widget_set_size_request(area, size, size);
    gtk_widget_set_halign(area, GTK_ALIGN_CENTER); gtk_widget_set_valign(area, GTK_ALIGN_CENTER);
    g_signal_connect(area, "draw", G_CALLBACK(icon_draw), (gpointer)name);
    return area;
}
/* The port's own picture (icon.png), sharp on HiDPI screens. */
static GtkWidget *app_image(const char *root, int size, int scale) {
    char *path = find_icon_path(root);
    GdkPixbuf *pixbuf = path ? gdk_pixbuf_new_from_file_at_scale(path, size * scale, size * scale, TRUE, NULL) : NULL;
    g_free(path);
    if (!pixbuf) { GtkWidget *empty = gtk_image_new(); gtk_widget_set_size_request(empty, size, size); return empty; }
    cairo_surface_t *surface = gdk_cairo_surface_create_from_pixbuf(pixbuf, scale, NULL);
    GtkWidget *image = gtk_image_new_from_surface(surface);
    cairo_surface_destroy(surface); g_object_unref(pixbuf);
    return image;
}

/* A text that wraps (or is shortened) inside whatever width it is given,
 * without asking for more: in GTK 3 a wrapping label asks for the width of
 * its whole text, and on KDE Wayland a fixed-size window grew to that
 * (460 -> 1095 px when an error appeared, George's desktop, 2026-11-15). */
static void fit_text(GtkWidget *widget, int characters) { gtk_label_set_max_width_chars(GTK_LABEL(widget), characters); }

/* ---- Small builders ---- */
static void add_class(GtkWidget *widget, const char *name) { gtk_style_context_add_class(gtk_widget_get_style_context(widget), name); }
static GtkWidget *hbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_HORIZONTAL, spacing); }
static GtkWidget *vbox(int spacing) { return gtk_box_new(GTK_ORIENTATION_VERTICAL, spacing); }
static GtkWidget *spacer(void) { GtkWidget *box = hbox(0); gtk_widget_set_hexpand(box, TRUE); return box; }
/* Fill a button with an icon and its text, 8 px apart (GTK 3 has no CSS for
 * that gap). The text is a mnemonic label inside, which the test channel
 * reads as the button's name. */
static void button_content(GtkWidget *widget, const char *text, const char *icon_name, int icon_size, gboolean start) {
    GtkWidget *content = hbox(icon_size > 15 ? 11 : 8);
    if (icon_name) gtk_box_pack_start(GTK_BOX(content), icon(icon_name, icon_size), FALSE, FALSE, 0);
    GtkWidget *text_label = gtk_label_new_with_mnemonic(text);
    gtk_label_set_mnemonic_widget(GTK_LABEL(text_label), widget);
    gtk_box_pack_start(GTK_BOX(content), text_label, FALSE, FALSE, 0);
    gtk_widget_set_halign(content, start ? GTK_ALIGN_START : GTK_ALIGN_CENTER);
    gtk_container_add(GTK_CONTAINER(widget), content);
}
static GtkWidget *button(const char *text, const char *icon_name, const char *class_name) {
    GtkWidget *widget = gtk_button_new();
    button_content(widget, text, icon_name, 15, FALSE);
    if (class_name) add_class(widget, class_name);
    gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
    return widget;
}
/* Change a button's text (the label inside it). */
static GtkWidget *button_text_label(GtkWidget *widget) {
    GtkWidget *child = gtk_bin_get_child(GTK_BIN(widget));
    if (GTK_IS_LABEL(child)) return child;
    if (!GTK_IS_CONTAINER(child)) return NULL;
    GList *children = gtk_container_get_children(GTK_CONTAINER(child)), *found = NULL;
    for (GList *item = children; item; item = item->next) if (GTK_IS_LABEL(item->data)) found = item;
    GtkWidget *result = found ? found->data : NULL;
    g_list_free(children);
    return result;
}
static void button_set_text(GtkWidget *widget, const char *text) {
    GtkWidget *text_label = button_text_label(widget);
    if (text_label) gtk_label_set_text_with_mnemonic(GTK_LABEL(text_label), text);
    else gtk_button_set_label(GTK_BUTTON(widget), text);
}
/* A rounded icon tile, as in the mockup's cards. */
static GtkWidget *icon_tile(const char *icon_name) {
    GtkWidget *tile = hbox(0); add_class(tile, "ic");
    gtk_widget_set_valign(tile, GTK_ALIGN_CENTER); gtk_widget_set_halign(tile, GTK_ALIGN_START);
    gtk_widget_set_size_request(tile, 34, 34);
    gtk_box_set_center_widget(GTK_BOX(tile), icon(icon_name, 18));
    return tile;
}
/* A card: icon tile, title, grey description, then the given controls. The
 * description label is returned through *sub so it can change later. */
static GtkWidget *card(const char *icon_name, const char *title, const char *description, GtkWidget **sub) {
    GtkWidget *row = hbox(14); add_class(row, "card");
    if (icon_name) gtk_box_pack_start(GTK_BOX(row), icon_tile(icon_name), FALSE, FALSE, 0);
    GtkWidget *text = vbox(1); gtk_widget_set_hexpand(text, TRUE); gtk_widget_set_valign(text, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(text), label(title, "card-title"), FALSE, FALSE, 0);
    GtkWidget *description_label = label(description, "card-sub");
    gtk_box_pack_start(GTK_BOX(text), description_label, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(row), text, TRUE, TRUE, 0);
    if (sub) *sub = description_label;
    return row;
}
static void card_add(GtkWidget *row, GtkWidget *control) {
    gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
    gtk_box_pack_start(GTK_BOX(row), control, FALSE, FALSE, 0);
}
/* A card whose buttons sit on their own row under the text, so a long
 * description is not squeezed. Returns the card; *actions is the row. */
static GtkWidget *action_card(const char *icon_name, const char *title, const char *description, GtkWidget **sub, GtkWidget **top, GtkWidget **actions) {
    GtkWidget *outer = vbox(0); add_class(outer, "card");
    GtkWidget *row = card(icon_name, title, description, sub);
    gtk_style_context_remove_class(gtk_widget_get_style_context(row), "card");
    gtk_box_pack_start(GTK_BOX(outer), row, FALSE, FALSE, 0);
    GtkWidget *buttons = hbox(8); add_class(buttons, "card-actions");
    gtk_widget_set_margin_start(buttons, icon_name ? 48 : 0);
    gtk_box_pack_start(GTK_BOX(outer), buttons, FALSE, FALSE, 0);
    if (top) *top = row;
    *actions = buttons;
    return outer;
}
static GtkWidget *pill(const char *text) { GtkWidget *widget = gtk_label_new(text); add_class(widget, "pill"); gtk_widget_set_valign(widget, GTK_ALIGN_CENTER); return widget; }
static void pill_set(GtkWidget *widget, const char *text, gboolean is_new) {
    gtk_label_set_text(GTK_LABEL(widget), text);
    GtkStyleContext *context = gtk_widget_get_style_context(widget);
    gtk_style_context_remove_class(context, is_new ? "ok" : "new");
    gtk_style_context_add_class(context, is_new ? "new" : "ok");
}

/* ---- Window chrome: our own title bar, so every window looks the same and
 * is dragged by it (George: "make sure the bootstrapper is draggable") ---- */
static void chrome_minimize(GtkButton *button, gpointer window) { gtk_window_iconify(window); }
static void chrome_maximize(GtkButton *button, gpointer window) {
    if (gtk_window_is_maximized(window)) gtk_window_unmaximize(window); else gtk_window_maximize(window);
}
/* Closing from our button is the same as closing from the desktop: the
 * window's delete-event decides (Cancel rules, prompt = cancel, ...). */
static void chrome_close(GtkButton *button, gpointer window) { gtk_window_close(window); }
static gboolean chrome_state(GtkWidget *window, GdkEventWindowState *event, gpointer data) {
    GtkStyleContext *context = gtk_widget_get_style_context(window);
    if (event->new_window_state & GDK_WINDOW_STATE_MAXIMIZED) gtk_style_context_add_class(context, "maximized");
    else gtk_style_context_remove_class(context, "maximized");
    return FALSE;
}
static GtkWidget *window_button(const char *icon_name, const char *tooltip, GCallback callback, GtkWidget *window, gboolean close) {
    GtkWidget *widget = gtk_button_new();
    gtk_container_add(GTK_CONTAINER(widget), icon(icon_name, 14));
    add_class(widget, "wbtn"); if (close) add_class(widget, "close");
    gtk_widget_set_tooltip_text(widget, tooltip); gtk_widget_set_valign(widget, GTK_ALIGN_CENTER);
    gtk_widget_set_can_focus(widget, FALSE);
    g_signal_connect(widget, "clicked", callback, window);
    return widget;
}
static GtkWidget *themed_window(const char *title, const char *root, gboolean can_minimize, gboolean can_maximize) {
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), title);
    add_class(window, "rbx");
    GtkWidget *bar = gtk_header_bar_new(); add_class(bar, "rbx-titlebar");
    gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(bar), FALSE);
    gtk_header_bar_set_custom_title(GTK_HEADER_BAR(bar), hbox(0));
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), app_image(root, 18, gtk_widget_get_scale_factor(window)));
    GtkWidget *name = gtk_label_new(title); add_class(name, "title-text");
    gtk_header_bar_pack_start(GTK_HEADER_BAR(bar), name);
    gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), window_button("close", "Close", G_CALLBACK(chrome_close), window, TRUE));
    if (can_maximize) gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), window_button("maximize", "Maximize", G_CALLBACK(chrome_maximize), window, FALSE));
    if (can_minimize) gtk_header_bar_pack_end(GTK_HEADER_BAR(bar), window_button("minimize", "Minimize", G_CALLBACK(chrome_minimize), window, FALSE));
    gtk_window_set_titlebar(GTK_WINDOW(window), bar);
    /* Set again: the desktop (task bar, window list) reads this title. */
    gtk_window_set_title(GTK_WINDOW(window), title);
    g_signal_connect(window, "window-state-event", G_CALLBACK(chrome_state), NULL);
    return window;
}

/* The look is ours on every desktop: Adwaita (built into GTK) as the base
 * in its dark variant, our CSS above it. */
static void apply_css(void) {
    g_object_set(gtk_settings_get_default(), "gtk-theme-name", "Adwaita",
                 "gtk-application-prefer-dark-theme", TRUE, NULL);
    GtkCssProvider *provider = gtk_css_provider_new();
    GError *error = NULL;
    if (!gtk_css_provider_load_from_data(provider, UI_CSS, -1, &error)) {
        g_printerr("bootstrapper-ui: style not loaded: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
    }
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
        GTK_STYLE_PROVIDER(provider), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}
