#include <gtk/gtk.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include "dsss-transfer.h"
#include "gettext.h"

#define _(string) gettext(string)

/* UI Widgets */
GtkWidget *window;
GtkWidget *entry_driver;
GtkWidget *entry_freq;
GtkWidget *entry_rate;
GtkWidget *entry_sf;
GtkWidget *entry_gain;
GtkWidget *entry_offset;
GtkWidget *entry_bitrate;
GtkWidget *radio_rx;
GtkWidget *radio_tx;
GtkWidget *btn_start_stop;
GtkWidget *textview_log;
GtkWidget *entry_msg;
GtkWidget *btn_send;
GtkWidget *grid_settings;

/* State */
volatile int running = 0;
dsss_transfer_t transfer = NULL;
pthread_t transfer_thread;
GQueue *tx_queue = NULL;
GMutex tx_queue_mutex;

/* Constants */
#define DEFAULT_DRIVER "driver=hackrf"
#define DEFAULT_FREQ "434000000"
#define DEFAULT_RATE "4000000"
#define DEFAULT_SF "64"
#define DEFAULT_GAIN "30"
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
    
    return bytes_written;
}

/* RX Callback */
int rx_callback(void *context, unsigned char *payload, unsigned int payload_size) {
    if (!running) return 0;
    if (payload_size == 0) return 0;
    
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
        const char *driver = gtk_entry_get_text(GTK_ENTRY(entry_driver));
        unsigned long freq = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_freq)), NULL, 10);
        unsigned long rate = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_rate)), NULL, 10);
        unsigned int sf = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_sf)), NULL, 10);
        char *gain = (char*)gtk_entry_get_text(GTK_ENTRY(entry_gain));
        long int offset = strtol(gtk_entry_get_text(GTK_ENTRY(entry_offset)), NULL, 10);
        unsigned int bitrate = strtoul(gtk_entry_get_text(GTK_ENTRY(entry_bitrate)), NULL, 10);
        int is_tx = gtk_toggle_button_get_active(GTK_TOGGLE_BUTTON(radio_tx));
        
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
            "h128", // inner fec
            "none", // outer fec
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
    
    setlocale(LC_ALL, "");
    bindtextdomain(PACKAGE, LOCALEDIR);
    textdomain(PACKAGE);
    
    tx_queue = g_queue_new();
    g_mutex_init(&tx_queue_mutex);
    
    /* Main Window */
    window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "DSSS Transfer GUI");
    gtk_window_set_default_size(GTK_WINDOW(window), 600, 500);
    g_signal_connect(window, "destroy", G_CALLBACK(on_window_destroy), NULL);
    
    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 5);
    gtk_container_add(GTK_CONTAINER(window), vbox);
    gtk_container_set_border_width(GTK_CONTAINER(vbox), 10);
    
    /* Settings Grid */
    grid_settings = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid_settings), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid_settings), 10);
    gtk_box_pack_start(GTK_BOX(vbox), grid_settings, FALSE, FALSE, 0);
    
    // Driver
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Driver:")), 0, 0, 1, 1);
    entry_driver = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_driver), DEFAULT_DRIVER);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_driver, 1, 0, 1, 1);
    
    // Frequency
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Frequency (Hz):")), 0, 1, 1, 1);
    entry_freq = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_freq), DEFAULT_FREQ);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_freq, 1, 1, 1, 1);
    
    // Sample Rate
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Sample Rate (S/s):")), 2, 0, 1, 1);
    entry_rate = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_rate), DEFAULT_RATE);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_rate, 3, 0, 1, 1);
    
    // Spreading Factor
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Spreading Factor:")), 2, 1, 1, 1);
    entry_sf = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_sf), DEFAULT_SF);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_sf, 3, 1, 1, 1);
    
    // Gain
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Gain (dB):")), 4, 0, 1, 1);
    entry_gain = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_gain), DEFAULT_GAIN);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_gain, 5, 0, 1, 1);

    // Offset
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Offset (Hz):")), 4, 1, 1, 1);
    entry_offset = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_offset), DEFAULT_OFFSET);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_offset, 5, 1, 1, 1);

    // Bit Rate
    gtk_grid_attach(GTK_GRID(grid_settings), gtk_label_new(_("Bit Rate (b/s):")), 6, 0, 1, 1);
    entry_bitrate = gtk_entry_new();
    gtk_entry_set_text(GTK_ENTRY(entry_bitrate), DEFAULT_BITRATE);
    gtk_grid_attach(GTK_GRID(grid_settings), entry_bitrate, 7, 0, 1, 1);
    
    /* Control Area */
    GtkWidget *hbox_ctrl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_ctrl, FALSE, FALSE, 5);
    
    radio_rx = gtk_radio_button_new_with_label(NULL, _("Receive Mode"));
    radio_tx = gtk_radio_button_new_with_label_from_widget(GTK_RADIO_BUTTON(radio_rx), _("Transmit Mode"));
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), radio_rx, FALSE, FALSE, 0);
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), radio_tx, FALSE, FALSE, 0);
    
    btn_start_stop = gtk_button_new_with_label(_("Start"));
    g_signal_connect(btn_start_stop, "clicked", G_CALLBACK(on_start_stop_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_ctrl), btn_start_stop, FALSE, FALSE, 0);
    
    /* Log Area */
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                   GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox), scrolled_window, TRUE, TRUE, 0);
    
    textview_log = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(textview_log), FALSE);
    gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(textview_log), FALSE);
    gtk_container_add(GTK_CONTAINER(scrolled_window), textview_log);
    
    /* Message Entry Area */
    GtkWidget *hbox_msg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    gtk_box_pack_start(GTK_BOX(vbox), hbox_msg, FALSE, FALSE, 0);
    
    entry_msg = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry_msg), _("Type message here..."));
    g_signal_connect(entry_msg, "activate", G_CALLBACK(on_send_clicked), NULL); // Enter key sends
    gtk_box_pack_start(GTK_BOX(hbox_msg), entry_msg, TRUE, TRUE, 0);
    
    btn_send = gtk_button_new_with_label(_("Send"));
    g_signal_connect(btn_send, "clicked", G_CALLBACK(on_send_clicked), NULL);
    gtk_box_pack_start(GTK_BOX(hbox_msg), btn_send, FALSE, FALSE, 0);
    
    // Binding TX only controls
    // We could disable send button/entry when in RX mode, but it's okay to leave them
    // enabled but doing nothing or showing warning.
    
    gtk_widget_show_all(window);
    
    gtk_main();
    
    return 0;
}

