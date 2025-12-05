#include <gtk/gtk.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <stdint.h>
#include <inttypes.h>
#include "dsss-transfer.h"
#include "gettext.h"

#define _(string) gettext(string)

/* UI Widgets */
GtkWidget *window;
GtkWidget *entry_freq;
GtkWidget *entry_rate;
GtkWidget *entry_sf;
GtkWidget *entry_gain;
GtkWidget *entry_offset;
GtkWidget *entry_bitrate;
GtkWidget *combo_fec_inner;
GtkWidget *combo_fec_outer;
GtkWidget *radio_rx;
GtkWidget *radio_tx;
GtkWidget *btn_start_stop;
GtkWidget *entry_pwd;
GtkWidget *textview_log;
GtkWidget *entry_msg;
GtkWidget *btn_send;
GtkWidget *grid_settings;
GtkWidget *drawing_area_spectrum;
/* Метрики */
GtkWidget *label_metric_status;
GtkWidget *label_metric_mode;
GtkWidget *label_metric_rx;
GtkWidget *label_metric_tx;

/* State */
volatile int running = 0;
dsss_transfer_t transfer = NULL;
pthread_t transfer_thread;
GQueue *tx_queue = NULL;
GMutex tx_queue_mutex;
// Spectrum data
float *spectrum_data = NULL;
int spectrum_len = 0;
GMutex spectrum_mutex;
/* Счётчики */
uint64_t rx_bytes = 0;
uint64_t tx_bytes = 0;
uint32_t password_key = 0;
int use_cipher = 0;

/* Обновление метрик */
gboolean update_metrics_idle(gpointer data) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Статус: %s", running ? "Работает" : "Остановлено");
    gtk_label_set_text(GTK_LABEL(label_metric_status), buf);

    snprintf(buf, sizeof(buf), "Режим: %s", gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_tx)) ? "Передача (TX)" : "Приём (RX)");
    gtk_label_set_text(GTK_LABEL(label_metric_mode), buf);

    snprintf(buf, sizeof(buf), "Принято байт: %" PRIu64, rx_bytes);
    gtk_label_set_text(GTK_LABEL(label_metric_rx), buf);

    snprintf(buf, sizeof(buf), "Отправлено байт: %" PRIu64, tx_bytes);
    gtk_label_set_text(GTK_LABEL(label_metric_tx), buf);
    return G_SOURCE_REMOVE;
}

/* Простое хеширование пароля (FNV-1a) */
uint32_t hash_password(const char *pwd) {
    uint32_t h = 0x811c9dc5;
    if (!pwd) return 0;
    while (*pwd) {
        h ^= (unsigned char)(*pwd++);
        h *= 0x01000193;
    }
    return h;
}

/* Генерация простого xorshift32 */
static inline uint32_t xorshift32(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

/* Простейшее "шифрование" (xor) для демонстрации: если пароли не совпадают, будет "каша" */
void xor_buffer_with_key(unsigned char *buf, unsigned int len, uint32_t key_seed) {
    if (!use_cipher || key_seed == 0) return;
    uint32_t st = key_seed;
    for (unsigned int i = 0; i < len; i++) {
        st = xorshift32(&st);
        buf[i] ^= (unsigned char)(st & 0xFF);
    }
}

/* Constants */
#define DEFAULT_FREQ "434000000"
#define DEFAULT_RATE "2000000"
#define DEFAULT_SF "64"
#define DEFAULT_GAIN "20"
#define DEFAULT_OFFSET "100000"
#define DEFAULT_BITRATE "100"

/* Helper to append text to log */
struct log_data {
    char *text;
    int len;
};

gboolean append_log_idle(gpointer user_data) {
    struct log_data *data = (struct log_data *)user_data;
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(textview_log));
    GtkTextIter end;
    gtk_text_buffer_get_end_iter(buffer, &end);
    
    // Check if we need to insert a newline if the previous text didn't end with one
    // or just append as is. The protocol doesn't strictly define lines, so we just append.
    // However, for chat, we might want to separate messages. 
    // Since this is raw data transfer, we'll just append what we get.
    gtk_text_buffer_insert(buffer, &end, data->text, data->len);
    
    // Auto-scroll
    GtkTextMark *mark = gtk_text_buffer_get_insert(buffer);
    gtk_text_view_scroll_to_mark(GTK_TEXT_VIEW(textview_log), mark, 0.0, TRUE, 0.0, 1.0);
    
    free(data->text);
    free(data);
    return G_SOURCE_REMOVE;
}

// Callback to draw spectrum
gboolean on_draw_event(GtkWidget *widget, cairo_t *cr, gpointer user_data) {
    GtkAllocation allocation;
    gtk_widget_get_allocation(widget, &allocation);
    int width = allocation.width;
    int height = allocation.height;

    // Draw background
    // cairo_set_source_rgb(cr, 0.1, 0.1, 0.1); // Dark background
    // cairo_paint(cr);

    // Draw grid
    cairo_set_source_rgb(cr, 0.3, 0.3, 0.3);
    cairo_set_line_width(cr, 0.5);
    for (int i = 1; i < 10; i++) {
        cairo_move_to(cr, i * width / 10.0, 0);
        cairo_line_to(cr, i * width / 10.0, height);
        cairo_move_to(cr, 0, i * height / 10.0);
        cairo_line_to(cr, width, i * height / 10.0);
    }
    cairo_stroke(cr);

    // Draw spectrum data
    g_mutex_lock(&spectrum_mutex);
    if (spectrum_data && spectrum_len > 0) {
        float x_step = (float)width / spectrum_len;
        
        // Draw Noise Floor (Simulated as Blue)
        cairo_set_source_rgb(cr, 0.0, 0.5, 1.0); // Blue for noise
        cairo_set_line_width(cr, 1.0);
        cairo_move_to(cr, 0, height);
        
        for (int i = 0; i < spectrum_len; i++) {
            // Base noise level
            float noise_val = 0.1 + ((float)rand() / RAND_MAX) * 0.05; 
            float y = height - (noise_val * height);
            cairo_line_to(cr, i * x_step, y);
        }
        cairo_stroke(cr);

        // Draw Signal (Green) - overlay if active
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0); // Green for signal
        cairo_set_line_width(cr, 2.0); // Thicker line
        cairo_move_to(cr, 0, height);
        
        for (int i = 0; i < spectrum_len; i++) {
            float val = spectrum_data[i]; // This contains signal + noise from rx_callback simulation
            float y = height - (val * height * 5.0); 
            if (y < 0) y = 0;
            if (y > height) y = height;
            
            cairo_line_to(cr, i * x_step, y);
        }
        cairo_stroke(cr);
        
        // Legend
        cairo_set_source_rgb(cr, 0.0, 0.5, 1.0);
        cairo_move_to(cr, 10, 20);
        cairo_show_text(cr, "Шум");
        
        cairo_set_source_rgb(cr, 0.0, 1.0, 0.0);
        cairo_move_to(cr, 60, 20);
        cairo_show_text(cr, "Сигнал");
    }
    g_mutex_unlock(&spectrum_mutex);

    return FALSE;
}

/* Callbacks for dsss-transfer */

int tx_callback(void *context, unsigned char *payload, unsigned int payload_size) {
    int bytes_written = 0;
    
    if (!running) return -1;
    
    g_mutex_lock(&tx_queue_mutex);
    if (!g_queue_is_empty(tx_queue)) {
        GBytes *bytes = g_queue_pop_head(tx_queue);
        gsize size;
        const void *data = g_bytes_get_data(bytes, &size);
        
        // Copy data to payload.
        // Note: payload_size is the max we can write.
        // If our message is larger, we should probably handle fragmentation or just truncate/wait.
        // For simplicity, we assume messages fit or we send what fits.
        // Actually, dsss-transfer expects us to fill payload.
        // If we have data, we fill it. If we have less data than payload_size, 
        // dsss-transfer might pad or we can return just what we wrote?
        // Checking dsss-transfer.c: send_frames calls callback. n = r.
        // So we can return less than payload_size.
        
        int to_copy = (size > payload_size) ? payload_size : size;
        memcpy(payload, data, to_copy);
        bytes_written = to_copy;
        
        // If we didn't send everything, push the rest back to front?
        // This is a simple chat, let's assume short messages for now or implement split.
        if (size > payload_size) {
             GBytes *remaining = g_bytes_new_from_bytes(bytes, to_copy, size - to_copy);
             g_queue_push_head(tx_queue, remaining);
        }
        
        g_bytes_unref(bytes);
    }
    g_mutex_unlock(&tx_queue_mutex);
    if (bytes_written > 0) {
        xor_buffer_with_key(payload, bytes_written, password_key); // "Шифруем" перед отправкой
        tx_bytes += bytes_written;
        g_idle_add(update_metrics_idle, NULL);
    }
    
    return bytes_written;
}

/* RX Callback */
int rx_callback(void *context, unsigned char *payload, unsigned int payload_size) {
    if (!running) return 0;
    
    // Update Spectrum Data (Fake visualization for now based on payload activity)
    // Real implementation would require access to IQ samples which is not exposed by callback
    g_mutex_lock(&spectrum_mutex);
    if (!spectrum_data) {
        spectrum_len = 512;
        spectrum_data = malloc(spectrum_len * sizeof(float));
    }
    // Generate some random noise + signal for visualization
    for (int i = 0; i < spectrum_len; i++) {
        spectrum_data[i] = ((float)rand() / RAND_MAX) * 0.1; // Noise floor
        if (payload_size > 0 && i > 200 && i < 300) {
             spectrum_data[i] += 0.5; // Signal "bump"
        }
    }
    g_mutex_unlock(&spectrum_mutex);
    gtk_widget_queue_draw(drawing_area_spectrum); // Trigger redraw

    if (payload_size == 0) return 0;
    xor_buffer_with_key(payload, payload_size, password_key); // "Расшифровываем" (или каша при неверном пароле)
    rx_bytes += payload_size;
    g_idle_add(update_metrics_idle, NULL);
    
    struct log_data *data = malloc(sizeof(struct log_data));
    data->text = malloc(payload_size + 1);
    memcpy(data->text, payload, payload_size);
    data->text[payload_size] = '\0'; // Null terminate for display, though it might contain binary
    data->len = payload_size;
    
    // Replace non-printable characters? For now, assume text.
    
    g_idle_add(append_log_idle, data);
    return payload_size;
}

void *transfer_worker(void *arg) {
    dsss_transfer_start(transfer);
    
    // When start returns, it means it stopped (or error)
    
    // Update UI state back to stopped (needs to be on main thread)
    gdk_threads_add_idle((GSourceFunc)gtk_widget_set_sensitive, grid_settings);
    // We can't pass arguments to set_sensitive easily with g_idle_add if we want to pass TRUE
    // We'll handle UI reset in a specific function
    
    return NULL;
}

gboolean reset_ui_state(gpointer data) {
    gtk_widget_set_sensitive(grid_settings, TRUE);
    gtk_button_set_label(GTK_BUTTON(btn_start_stop), _("Start"));
    running = 0;
    if (transfer) {
        dsss_transfer_free(transfer);
        transfer = NULL;
    }
    return G_SOURCE_REMOVE;
}

void on_start_stop_clicked(GtkWidget *widget, gpointer data) {
    if (running) {
        // Stop
        dsss_transfer_stop(transfer);
        // Worker thread will exit and clean up
        running = 0;
        gtk_button_set_label(GTK_BUTTON(btn_start_stop), _("Start"));
        // Don't enable settings immediately, wait for thread to finish?
        // dsss_transfer_stop sets a flag. dsss_transfer_start loop should break.
        // We'll let the worker thread completion trigger the UI reset via idle callback if needed,
        // or just do it here if join is not blocked.
        // Ideally we should wait for thread to join.
        // For responsiveness, we set a flag and let it die.
        // We can disable the button temporarily to prevent double clicks.
        
        // Hack: Run hackrf_info to unblock device if stuck
        if (system("hackrf_info > /dev/null 2>&1 &")) {}
    } else {
        // Start
        const char *driver = "driver=hackrf";
        unsigned long freq = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_freq)), NULL, 10);
        unsigned long rate = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_rate)), NULL, 10);
        unsigned int sf = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_sf)), NULL, 10);
        char *gain = (char*)gtk_entry_get_text(GTK_ENTRY(entry_gain));
        long int offset = strtol(gtk_entry_get_text(GTK_ENTRY(entry_offset)), NULL, 10);
        unsigned int bitrate = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_bitrate)), NULL, 10);
        int is_tx = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_tx));
        const char *fec_inner = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_fec_inner));
        const char *fec_outer = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo_fec_outer));
        char inner_code[32] = "h128";
        char outer_code[32] = "none";
        if (fec_inner) sscanf(fec_inner, "%31s", inner_code);
        if (fec_outer) sscanf(fec_outer, "%31s", outer_code);

        const char *pwd = gtk_entry_get_text(GTK_ENTRY(entry_pwd));
        if (pwd && strlen(pwd) > 0) {
            use_cipher = 1;
            password_key = hash_password(pwd);
        } else {
            use_cipher = 0;
            password_key = 0;
        }
        
        transfer = dsss_transfer_create_callback(
            (char*)driver,
            is_tx,
            is_tx ? tx_callback : rx_callback, // In TX mode we read data to send. In RX mode we write received data.
            NULL,
            rate,
            bitrate,
            freq,
            offset,
            gain,
            0.0, // ppm
            sf,
            inner_code, // inner fec
            outer_code, // outer fec
            "", // id
            NULL, // dump
            0, // timeout
            0 // audio
        );
        
        if (!transfer) {
            // Show error dialog
            GtkWidget *dialog = gtk_message_dialog_new(GTK_WINDOW(window),
                                     GTK_DIALOG_DESTROY_WITH_PARENT,
                                     GTK_MESSAGE_ERROR,
                                     GTK_BUTTONS_CLOSE,
                                     "Error initializing transfer (check params/device)");
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
            return;
        }
        
        running = 1;
        gtk_widget_set_sensitive(grid_settings, FALSE);
        gtk_button_set_label(GTK_BUTTON(btn_start_stop), _("Stop"));
        
        pthread_create(&transfer_thread, NULL, transfer_worker, NULL);
    }
}

void on_send_clicked(GtkWidget *widget, gpointer data) {
    if (!running) return;
    if (!gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_tx))) return;
    
    const char *text = gtk_entry_get_text(GTK_ENTRY(entry_msg));
    if (strlen(text) == 0) return;
    
    // Add newline for chat-like behavior?
    char *msg_with_nl = g_strdup_printf("%s\n", text);
    
    g_mutex_lock(&tx_queue_mutex);
    g_queue_push_tail(tx_queue, g_bytes_new(msg_with_nl, strlen(msg_with_nl)));
    g_mutex_unlock(&tx_queue_mutex);
    
    // Echo to local log
    struct log_data *log = malloc(sizeof(struct log_data));
    log->text = g_strdup_printf("Me: %s", msg_with_nl);
    log->len = strlen(log->text);
    g_idle_add(append_log_idle, log);
    
    g_free(msg_with_nl);
    gtk_entry_set_text(GTK_ENTRY(entry_msg), "");
}

/* Cleanup on window destroy */
void on_window_destroy(GtkWidget *widget, gpointer data) {
    if (running) {
        dsss_transfer_stop(transfer);
        running = 0;
        // Wait a bit?
    }
    gtk_main_quit();
}

int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    
    // Add CSS Styling
    GtkCssProvider *provider = gtk_css_provider_new();
    gtk_css_provider_load_from_data(provider,
        "window { background-color: #f4f4f4; color: #1e1e1e; }"
        "entry { padding: 6px; border-radius: 4px; border: 1px solid #c8c8c8; background-color: #ffffff; color: #1f1f1f; }"
        "button { padding: 8px 16px; border-radius: 4px; border: 1px solid #c0c0c0; background-color: #f0f0f0; color: #1f1f1f; }"
        "button:hover { background-color: #e2e2e2; }"
        "label { color: #1e1e1e; }"
        "frame { border: 1px solid #dcdcdc; border-radius: 6px; padding: 10px; background: #fafafa; }"
        "textview { font-family: 'Monospace'; font-size: 22px; color: #1a1a1a; background-color: #ffffff; }"
        "textview text { color: #1a1a1a; background-color: #ffffff; }"
        "combobox { color: #1f1f1f; background-color: #ffffff; }"
        "tooltip { background-color: #ffffe1; color: #111111; border: 1px solid #d0d0d0; }"
        "tooltip * { color: #111111; }"
        , -1, NULL);
    gtk_style_context_add_provider_for_screen(gdk_screen_get_default(),
                                              GTK_STYLE_PROVIDER(provider),
                                              GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    dsss_transfer_set_verbose(1); // Enable verbose logging to terminal

    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
    
    tx_queue = g_queue_new();
    g_mutex_init(&tx_queue_mutex);
    
    /* Main Window */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "Широкополосная модуляция с прямым расширением спектра на HackRF");
    gtk_window_set_default_size(GTK_WINDOW(window), 800, 600);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    
    /* Settings Grid */
    grid_settings = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_settings), 10);
    gtk_grid_set_column_spacing(GTK_GRID(grid_settings), 15);
    gtk_box_pack_start(GTK_BOX(vbox), grid_settings, FALSE, FALSE, 0);
    
    // Frequency
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Частота (Hz):")), 0, 0, 1, 1);
    entry_freq = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_freq), DEFAULT_FREQ);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_freq, 1, 0, 1, 1);
    gtk_widget_set_tooltip_text(entry_freq, "Частота несущей, Гц (например, 434000000)");
    
    // Sample Rate
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Частота дискретизации (S/s):")), 0, 1, 1, 1);
    entry_rate = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_rate), DEFAULT_RATE);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_rate, 1, 1, 1, 1);
    gtk_widget_set_tooltip_text(entry_rate, "Частота дискретизации, S/s. 2e6–4e6 обычно для HackRF");
    
    // Spreading Factor
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Коэфф. расширения:")), 2, 0, 1, 1);
    entry_sf = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_sf), DEFAULT_SF);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_sf, 3, 0, 1, 1);
    gtk_widget_set_tooltip_text(entry_sf, "Коэффициент DSSS. 64 — надежно, но медленно");
    
    // Gain
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Усиление (dB):")), 2, 1, 1, 1);
    entry_gain = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_gain), DEFAULT_GAIN);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_gain, 3, 1, 1, 1);
    gtk_widget_set_tooltip_text(entry_gain, "Усиление TX/RX. В одной комнате 10–20 dB");

    // Offset
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Смещение (Hz):")), 4, 0, 1, 1);
    entry_offset = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_offset), DEFAULT_OFFSET);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_offset, 5, 0, 1, 1);
    gtk_widget_set_tooltip_text(entry_offset, "Смещение для ухода от DC. Для HackRF обычно ~100000");

    // Bit Rate
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Скорость (b/s):")), 4, 1, 1, 1);
    entry_bitrate = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_bitrate), DEFAULT_BITRATE);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_bitrate, 5, 1, 1, 1);
    gtk_widget_set_tooltip_text(entry_bitrate, "Битовая скорость полезных данных. 100 — надежно");

    // FEC Inner
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Внутренний код (Inner FEC):")), 0, 2, 1, 1);
    combo_fec_inner = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "none", "none");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "h74", "h74");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "h84", "h84");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "h128", "h128 [Default]");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "v27", "v27");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_inner), "v29", "v29");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_fec_inner), 3); // h128 default
    gtk_grid_attach(GTK_GRID(grid_settings), combo_fec_inner, 1, 2, 1, 1);
    gtk_widget_set_tooltip_text(combo_fec_inner, "Внутренний (быстрый) FEC. Hamming(12,8) по умолчанию");

    // FEC Outer
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Внешний код (Outer FEC):")), 2, 2, 1, 1);
    combo_fec_outer = gtk_combo_box_text_new();
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_outer), "none", "none [Default]");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_outer), "rs8", "rs8");
    gtk_combo_box_text_append(GTK_COMBO_BOX_TEXT(combo_fec_outer), "h128", "h128");
    gtk_combo_box_set_active(GTK_COMBO_BOX(combo_fec_outer), 0); // none default
    gtk_grid_attach(GTK_GRID(grid_settings), combo_fec_outer, 3, 2, 1, 1);
    gtk_widget_set_tooltip_text(combo_fec_outer, "Внешний (доп.) FEC. none — без, rs8 — сильнее защита");

    // Пароль для ПСП (демонстрация: XOR-шифр)
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Пароль (ПСП):")), 0, 3, 1, 1);
    entry_pwd = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_pwd), "");
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_pwd), _("Оставьте пустым для открытого режима"));
    gtk_grid_attach(GTK_GRID(grid_settings), entry_pwd, 1, 3, 1, 1);
    gtk_widget_set_tooltip_text(entry_pwd, "Пароль задаёт ПСП (простая XOR-демо). При неверном пароле будет «каша».");
    
    /* Spectrum Display - Removed as requested */
    /*
    GtkWidget *frame_spectrum = gtk_frame_new(_("Спектр амплитуд (Шум vs Сигнал)"));
    gtk_box_pack_start(GTK_BOX(vbox), frame_spectrum, TRUE, TRUE, 5); // Allow expand
    drawing_area_spectrum = gtk_drawing_area_new();
    gtk_widget_set_size_request(drawing_area_spectrum, -1, 300); // Increased height to 300
    g_signal_connect(G_OBJECT(drawing_area_spectrum), "draw", G_CALLBACK(on_draw_event), NULL);
    gtk_container_add(GTK_CONTAINER(frame_spectrum), drawing_area_spectrum);
    */
    
    /* Control Area */
    GtkWidget *hbox_ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_ctrl, FALSE, FALSE, 5);
    
    radio_rx = gtk_radio_button_new_with_label(NULL, _("Режим приема (RX)"));
    radio_tx = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_rx), _("Режим передачи (TX)"));
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), radio_rx, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), radio_tx, FALSE, FALSE, 0);
    
    btn_start_stop = gtk_button_new_with_label(_("Старт"));
    g_signal_connect(btn_start_stop, "clicked", G_CALLBACK(on_start_stop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), btn_start_stop, FALSE, FALSE, 0);
    
    /* Метрики */
    GtkWidget *frame_metrics = gtk_frame_new(_("Метрики"));
    gtk_box_pack_start(GTK_BOX(vbox), frame_metrics, FALSE, FALSE, 5);
    GtkWidget *grid_metrics = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_metrics), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid_metrics), 10);
    gtk_container_add(GTK_CONTAINER(frame_metrics), grid_metrics);

    label_metric_status = gtk_label_new(_("Статус: Остановлено"));
    label_metric_mode   = gtk_label_new(_("Режим: Приём (RX)"));
    label_metric_rx     = gtk_label_new(_("Принято байт: 0"));
    label_metric_tx     = gtk_label_new(_("Отправлено байт: 0"));

    gtk_grid_attach(GTK_GRID(grid_metrics), label_metric_status, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_metrics), label_metric_mode,   1, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_metrics), label_metric_rx,     0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid_metrics), label_metric_tx,     1, 1, 1, 1);

    /* Log Area */
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    // Restore larger log area since spectrum is gone
    // gtk_widget_set_size_request(scrolled_window, -1, 100); 
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);
    
    textview_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview_log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview_log), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled_window), textview_log);
    
    /* Message Entry Area */
    GtkWidget *hbox_msg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_msg, FALSE, FALSE, 0);
    
    entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_msg), _("Введите сообщение..."));
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_send_clicked), NULL); // Enter key sends
    gtk_box_pack_start(GTK_BOX(hbox_msg), entry_msg, TRUE, TRUE, 0);
    
    btn_send = gtk_button_new_with_label(_("Отправить"));
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_msg), btn_send, FALSE, FALSE, 0);
    
    // Binding TX only controls
    // We could disable send button/entry when in RX mode, but it's okay to leave them
    // enabled but doing nothing or showing warning.
    
    gtk_widget_show_all(window);
    
    gtk_main();
    
    return 0;
}

