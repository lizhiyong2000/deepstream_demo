#include <glib.h>
#include <gst/gst.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include <iostream>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "gst-nvmessage.h"
#include "gstnvdsmeta.h"
#include "myheader.h"
#include "nvds_analytics_meta.h"
#include "nvdsmeta_schema.h"

#define MAX_DISPLAY_LEN 64
#define MAX_TIME_STAMP_LEN 32

#define PGIE_CLASS_ID_VEHICLE 0
#define PGIE_CLASS_ID_PERSON 2

#define PGIE_CONFIG_FILE "nvdsanalytics_pgie_config.txt"
#define MSCONV_CONFIG_FILE "dstest4_msgconv_config.txt"
#define ANALYTICS_CONFIG_FILE "config_nvdsanalytics.txt"

/* The muxer output resolution must be set if the input streams will be of
 * different resolution. The muxer will scale all the input frames to this
 * resolution. */
#define MUXER_OUTPUT_WIDTH 1920
#define MUXER_OUTPUT_HEIGHT 1080

/* Muxer batch formation timeout, for e.g. 40 millisec. Should ideally be set
 * based on the fastest source's framerate. */
#define MUXER_BATCH_TIMEOUT_USEC 40000

#define TILED_OUTPUT_WIDTH 1920
#define TILED_OUTPUT_HEIGHT 1080

/* NVIDIA Decoder source pad memory feature. This feature signifies that source
 * pads having this capability will push GstBuffers containing cuda buffers. */
#define GST_CAPS_FEATURES_NVMM "memory:NVMM"
#define MAX_NUM_SOURCES 4

/* Dynamic add and remove video source */
//#define VIDEO_SOURCE_FILE "sources.txt"
gchar *VIDEO_SOURCE_FILE = (char *)"sources.txt";
static gchar *cfg_file = NULL;
static gchar *input_file = NULL;
const gchar *topic = "line-crossing";
const gchar *conn_str = "localhost;9092";
const gchar *proto_lib = "/opt/nvidia/deepstream/deepstream-5.0/lib/libnvds_kafka_proto.so";
static gint schema_type = 1;
static gboolean display_off = FALSE;

gint g_num_sources = 0;
gint g_source_id_list[MAX_NUM_SOURCES];
gboolean g_eos_list[MAX_NUM_SOURCES];
gboolean g_source_enabled[MAX_NUM_SOURCES];
GstElement **g_source_bin_list = NULL;
GMutex eos_lock;

gchar *g_video_source[MAX_NUM_SOURCES];
gchar *g_lc_type[MAX_NUM_SOURCES];

gchar pgie_classes_str[4][32] = {"Vehicle", "TwoWheeler", "Person", "RoadSign"};

MyUserMeta user_data[MAX_NUM_SOURCES];

GMainLoop *loop = NULL;
GstElement *pipeline = NULL, *streammux = NULL, *sink = NULL, *pgie = NULL, *nvtracker = NULL, *nvdsanalytics = NULL,
           *nvvidconv = NULL, *nvosd = NULL, *tiler = NULL;

static char *ReplaceSubStr(char *str, const char *srcSubStr, const char *dstSubStr, char *out) {
    if (!str && !out) {
        return NULL;
    }
    if (!srcSubStr && !dstSubStr) {
        return out;
    }
    char *out_temp = out;
    int src_len = strlen(srcSubStr);
    int dst_len = strlen(dstSubStr);
    while (*str != '\0') {
        if (*str == *srcSubStr) {
            const char *str_temp = str;
            int flag = 0;
            for (int i = 0; i < src_len; i++) {
                if (str_temp[i] != srcSubStr[i]) {
                    flag = 1;
                    break;
                }
            }
            if (flag) {
                *out_temp++ = *str++;
            } else {
                for (int i = 0; i < dst_len; i++) {
                    *out_temp++ = dstSubStr[i];
                }
                str = str + src_len;
            }
        } else {
            *out_temp++ = *str++;
        }
    }
    *out_temp = 0;
    return out;
}

static void get_lc_type(const gchar *config_file) {
    FILE *pFile = NULL;
    gchar txt_line[200];
    pFile = fopen(config_file, "r");
    if (pFile == NULL)
        perror("Error reading config file");
    else {
        int lc_stream_id = 0;
        int lc_class_id = 0;
        while (fgets(txt_line, 200, pFile) != NULL) {
            int len = strlen(txt_line);
            if (strstr(txt_line, "line-crossing-stream")) {
                lc_stream_id = txt_line[len - 3] - '0';
            }
            if (strstr(txt_line, "class-id")) {
                // g_print("find class-id");
                lc_class_id = txt_line[len - 2] - '0';
                g_lc_type[lc_stream_id] = strdup(pgie_classes_str[lc_class_id]);
            }
        }
    }
    fclose(pFile);
}

static void generate_ts_rfc3339(char *buf, int buf_size) {
    time_t tloc;
    struct tm tm_log;
    struct timespec ts;
    char strmsec[6];

    clock_gettime(CLOCK_REALTIME, &ts);
    memcpy(&tloc, (void *)(&ts.tv_sec), sizeof(time_t));
    gmtime_r(&tloc, &tm_log);
    strftime(buf, buf_size, "%Y-%m-%dT%H:%M:%S", &tm_log);
    int ms = ts.tv_nsec / 1000000;
    g_snprintf(strmsec, sizeof(strmsec), ".%.3dZ", ms);
    strncat(buf, strmsec, buf_size);
}

static gpointer meta_copy_func(gpointer data, gpointer user_data) {
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *)user_meta->user_meta_data;
    NvDsEventMsgMeta *dstMeta = NULL;

    dstMeta = (NvDsEventMsgMeta *)g_memdup(srcMeta, sizeof(NvDsEventMsgMeta));

    if (srcMeta->ts) dstMeta->ts = g_strdup(srcMeta->ts);

    if (srcMeta->sensorStr) dstMeta->sensorStr = g_strdup(srcMeta->sensorStr);

    if (srcMeta->objSignature.size > 0) {
        dstMeta->objSignature.signature =
            (gdouble *)g_memdup(srcMeta->objSignature.signature, srcMeta->objSignature.size);
        dstMeta->objSignature.size = srcMeta->objSignature.size;
    }

    if (srcMeta->objectId) {
        dstMeta->objectId = g_strdup(srcMeta->objectId);
    }

    if (srcMeta->sensorId) {
        dstMeta->sensorId = srcMeta->sensorId;
    }

    if (srcMeta->extMsgSize > 0) {
        if (srcMeta->objType == NVDS_OBJECT_TYPE_VEHICLE) {
            NvDsVehicleObject *srcObj = (NvDsVehicleObject *)srcMeta->extMsg;
            NvDsVehicleObject *obj = (NvDsVehicleObject *)g_malloc0(sizeof(NvDsVehicleObject));
            if (srcObj->type) obj->type = g_strdup(srcObj->type);
            if (srcObj->make) obj->make = g_strdup(srcObj->make);
            if (srcObj->model) obj->model = g_strdup(srcObj->model);
            if (srcObj->color) obj->color = g_strdup(srcObj->color);
            if (srcObj->license) obj->license = g_strdup(srcObj->license);
            if (srcObj->region) obj->region = g_strdup(srcObj->region);

            dstMeta->extMsg = obj;
            dstMeta->extMsgSize = sizeof(NvDsVehicleObject);
        } else if (srcMeta->objType == NVDS_OBJECT_TYPE_PERSON) {
            NvDsPersonObject *srcObj = (NvDsPersonObject *)srcMeta->extMsg;
            NvDsPersonObject *obj = (NvDsPersonObject *)g_malloc0(sizeof(NvDsPersonObject));

            obj->age = srcObj->age;

            if (srcObj->gender) obj->gender = g_strdup(srcObj->gender);
            if (srcObj->cap) obj->cap = g_strdup(srcObj->cap);
            if (srcObj->hair) obj->hair = g_strdup(srcObj->hair);
            if (srcObj->apparel) obj->apparel = g_strdup(srcObj->apparel);
            dstMeta->extMsg = obj;
            dstMeta->extMsgSize = sizeof(NvDsPersonObject);
        }
    }

    return dstMeta;
}

static void meta_free_func(gpointer data, gpointer user_data) {
    NvDsUserMeta *user_meta = (NvDsUserMeta *)data;
    NvDsEventMsgMeta *srcMeta = (NvDsEventMsgMeta *)user_meta->user_meta_data;

    g_free(srcMeta->ts);
    g_free(srcMeta->sensorStr);

    if (srcMeta->objSignature.size > 0) {
        g_free(srcMeta->objSignature.signature);
        srcMeta->objSignature.size = 0;
    }

    if (srcMeta->objectId) {
        g_free(srcMeta->objectId);
    }

    if (srcMeta->extMsgSize > 0) {
        if (srcMeta->objType == NVDS_OBJECT_TYPE_VEHICLE) {
            NvDsVehicleObject *obj = (NvDsVehicleObject *)srcMeta->extMsg;
            if (obj->type) g_free(obj->type);
            if (obj->color) g_free(obj->color);
            if (obj->make) g_free(obj->make);
            if (obj->model) g_free(obj->model);
            if (obj->license) g_free(obj->license);
            if (obj->region) g_free(obj->region);
        } else if (srcMeta->objType == NVDS_OBJECT_TYPE_PERSON) {
            NvDsPersonObject *obj = (NvDsPersonObject *)srcMeta->extMsg;

            if (obj->gender) g_free(obj->gender);
            if (obj->cap) g_free(obj->cap);
            if (obj->hair) g_free(obj->hair);
            if (obj->apparel) g_free(obj->apparel);
        }
        g_free(srcMeta->extMsg);
        srcMeta->extMsgSize = 0;
    }
    g_free(user_meta->user_meta_data);
    user_meta->user_meta_data = NULL;
}

static void generate_event_msg_meta(gpointer data, MyUserMeta *user_param) {
    // g_print("generate event msg-------");
    NvDsEventMsgMeta *meta = (NvDsEventMsgMeta *)data;

    meta->ts = (gchar *)g_malloc0(MAX_TIME_STAMP_LEN + 1);
    generate_ts_rfc3339(meta->ts, MAX_TIME_STAMP_LEN);

    meta->lccum_cnt_entry = user_param->lcc_cnt_entry;
    meta->lccum_cnt_exit = user_param->lcc_cnt_exit;
    meta->lccum_cnt_type = strdup(user_param->lcc_cnt_type);
}

/* nvdsanalytics_src_pad_buffer_probe  will extract metadata received on tiler sink pad
 * and extract nvanalytics metadata etc. */
static GstPadProbeReturn nvdsanalytics_src_pad_buffer_probe(GstPad *pad, GstPadProbeInfo *info, gpointer u_data) {
    GstBuffer *buf = (GstBuffer *)info->data;
    NvDsObjectMeta *obj_meta = NULL;
    NvDsMetaList *l_frame = NULL;
    NvDsMetaList *l_obj = NULL;
    NvDsDisplayMeta *display_meta = NULL;
    guint stream_id = 0;

    NvDsBatchMeta *batch_meta = gst_buffer_get_nvds_batch_meta(buf);

    for (l_frame = batch_meta->frame_meta_list; l_frame != NULL; l_frame = l_frame->next) {
        NvDsFrameMeta *frame_meta = (NvDsFrameMeta *)(l_frame->data);
        gboolean isCrossed = FALSE;
        std::stringstream out_string;
        std::stringstream total_string;
        stream_id = frame_meta->pad_index;

        /* Iterate user metadata in frames to search analytics metadata */
        for (NvDsMetaList *l_user = frame_meta->frame_user_meta_list; l_user != NULL; l_user = l_user->next) {
            // g_print("in meta\n");
            NvDsUserMeta *user_meta = (NvDsUserMeta *)l_user->data;
            if (user_meta->base_meta.meta_type != NVDS_USER_FRAME_META_NVDSANALYTICS) continue;

            /* convert to  metadata */
            NvDsAnalyticsFrameMeta *meta = (NvDsAnalyticsFrameMeta *)user_meta->user_meta_data;

            user_data[stream_id].lcc_cnt_type = strdup(g_lc_type[frame_meta->pad_index]);

            /* Get the labels from nvdsanalytics config file */
            for (std::pair<std::string, uint32_t> status : meta->objLCCumCnt) {
                out_string << " LineCrossing Cumulative ";
                out_string << status.first;
                out_string << " = ";
                out_string << status.second;
            }
            for (std::pair<std::string, uint32_t> status : meta->objLCCurrCnt) {
                out_string << " LineCrossing Current Frame ";
                out_string << status.first;
                out_string << " = ";
                out_string << status.second;
            }

            if (meta->objLCCurrCnt["Entry"] == 0 && meta->objLCCurrCnt["Exit"] == 0) {
                continue;
            } else {
                isCrossed = TRUE;
                if (meta->objLCCumCnt["Entry"] < user_data[stream_id].lcc_cnt_entry) {
                    user_data[stream_id].lcc_cnt_entry += meta->objLCCurrCnt["Entry"];
                } else {
                    user_data[stream_id].lcc_cnt_entry = meta->objLCCumCnt["Entry"];
                }
                if (meta->objLCCumCnt["Exit"] < user_data[stream_id].lcc_cnt_exit) {
                    user_data[stream_id].lcc_cnt_exit += meta->objLCCurrCnt["Exit"];
                } else {
                    user_data[stream_id].lcc_cnt_exit = meta->objLCCumCnt["Exit"];
                }
                user_data[stream_id].lcc_cnt_type = strdup(g_lc_type[frame_meta->pad_index]);
            }
        }
        if (isCrossed) {
            NvDsEventMsgMeta *msg_meta = (NvDsEventMsgMeta *)g_malloc0(sizeof(NvDsEventMsgMeta));
            msg_meta->frameId = frame_meta->frame_num;
            msg_meta->sensorId = stream_id;
            msg_meta->videoPath = g_strdup(g_video_source[stream_id]);

            MyUserMeta *user_data_pointer = &user_data[stream_id];

            generate_event_msg_meta(msg_meta, user_data_pointer);

            NvDsUserMeta *user_event_meta = nvds_acquire_user_meta_from_pool(batch_meta);

            if (user_event_meta) {
                user_event_meta->user_meta_data = (void *)msg_meta;
                user_event_meta->base_meta.meta_type = NVDS_EVENT_MSG_META;
                user_event_meta->base_meta.copy_func = (NvDsMetaCopyFunc)meta_copy_func;
                user_event_meta->base_meta.release_func = (NvDsMetaReleaseFunc)meta_free_func;
                nvds_add_user_meta_to_frame(frame_meta, user_event_meta);
            } else {
                g_print("Error in attaching event meta to buffer\n");
            }
        }
        total_string << "Entry=" << user_data[stream_id].lcc_cnt_entry << ", ";
        total_string << "Exit=" << user_data[stream_id].lcc_cnt_exit << ", ";
        total_string << "Type=" << g_lc_type[stream_id] << "\t";
        // g_print ("Stream = %d, %s\n", stream_id, total_string.str().c_str());

        display_meta = nvds_acquire_display_meta_from_pool(batch_meta);
        NvOSD_TextParams *txt_params = &display_meta->text_params[0];
        display_meta->num_labels = 1;
        txt_params->display_text = (gchar *)g_malloc0(256);

        txt_params->display_text = g_strdup(total_string.str().c_str());

        /* Now set the offsets where the string should appear */
        txt_params->x_offset = 20;
        txt_params->y_offset = 20;

        /* Font , font-color and font-size */
        txt_params->font_params.font_name = (gchar *)"Serif";
        txt_params->font_params.font_size = 20;
        txt_params->font_params.font_color.red = 1.0;
        txt_params->font_params.font_color.green = 1.0;
        txt_params->font_params.font_color.blue = 1.0;
        txt_params->font_params.font_color.alpha = 1.0;

        /* Text background color */
        txt_params->set_bg_clr = 1;
        txt_params->text_bg_clr.red = 0.0;
        txt_params->text_bg_clr.green = 0.0;
        txt_params->text_bg_clr.blue = 0.0;
        txt_params->text_bg_clr.alpha = 1.0;

        nvds_add_display_meta_to_frame(frame_meta, display_meta);

        g_print("Frame Number = %d of Stream = %d, %s\n", frame_meta->frame_num, frame_meta->pad_index,
                out_string.str().c_str());
    }
    return GST_PAD_PROBE_OK;
}

static gboolean bus_call(GstBus *bus, GstMessage *msg, gpointer data) {
    GMainLoop *loop = (GMainLoop *)data;
    switch (GST_MESSAGE_TYPE(msg)) {
        case GST_MESSAGE_EOS:
            g_print("End of stream\n");
            g_main_loop_quit(loop);
            break;
        case GST_MESSAGE_WARNING: {
            gchar *debug;
            GError *error;
            gst_message_parse_warning(msg, &error, &debug);
            g_printerr("WARNING from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
            g_free(debug);
            g_printerr("Warning: %s\n", error->message);
            g_error_free(error);
            break;
        }
        case GST_MESSAGE_ERROR: {
            gchar *debug;
            GError *error;
            gst_message_parse_error(msg, &error, &debug);
            g_printerr("ERROR from element %s: %s\n", GST_OBJECT_NAME(msg->src), error->message);
            if (debug) g_printerr("Error details: %s\n", debug);
            g_free(debug);
            g_error_free(error);
            g_main_loop_quit(loop);
            break;
        }
        case GST_MESSAGE_ELEMENT: {
            if (gst_nvmessage_is_stream_eos(msg)) {
                guint stream_id;
                if (gst_nvmessage_parse_stream_eos(msg, &stream_id)) {
                    g_print("Got EOS from stream %d\n", stream_id);
                    g_mutex_lock(&eos_lock);
                    g_eos_list[stream_id] = TRUE;
                    g_mutex_unlock(&eos_lock);
                }
            }
            break;
        }
        default:
            break;
    }
    return TRUE;
}

static void decodebin_child_added(GstChildProxy *child_proxy, GObject *object, gchar *name, gpointer user_data) {
    g_print("decodebin child added %s\n", name);
    /* gchar * g_strrstr (const gchar *haystack, const gchar *needle)
       search the string haystack for the last occurence of the string needle*/
    if (g_strrstr(name, "decodebin") == name) {
        g_signal_connect(G_OBJECT(object), "child-added", G_CALLBACK(decodebin_child_added), user_data);
    }
    if (g_strrstr(name, "nvv4l2decoder") == name) {
#ifdef PLATFORM_TEGRA
        g_object_set(object, "enable-max-performance", TRUE, NULL);
        g_object_set(object, "bufapi-version", TRUE, NULL);
        g_object_set(object, "drop-frame-interval", 0, NULL);
        g_object_set(object, "num-extra-surfaces", 0, NULL);
#else
        g_object_set(object, "gpu-id", GPU_ID, NULL);
#endif
    }
}

static void cb_newpad(GstElement *decodebin, GstPad *pad, gpointer data) {
    GstCaps *caps = gst_pad_query_caps(pad, NULL);
    const GstStructure *str = gst_caps_get_structure(caps, 0);
    const gchar *name = gst_structure_get_name(str);

    g_print("decodebin new pad %s\n", name);
    if (!strncmp(name, "video", 5)) {
        gint source_id = (*(gint *)data);
        gchar pad_name[16] = {0};
        GstPad *sinkpad = NULL;
        g_snprintf(pad_name, 15, "sink_%u", source_id);
        sinkpad = gst_element_get_request_pad(streammux, pad_name);
        if (gst_pad_link(pad, sinkpad) != GST_PAD_LINK_OK) {
            g_print("Failed to link decodebin to pipeline\n");
        } else {
            g_print("Decodebin linked to pipeline\n");
        }
        gst_object_unref(sinkpad);
    }
}

static GstElement *create_uridecode_bin(guint index, gchar *filename) {
    GstElement *bin = NULL;
    gchar bin_name[16] = {};

    g_print("creating uridecodebin for [%s]\n", filename);
    g_source_id_list[index] = index;
    /* gint g_snprintf (gchar *string, gulong n, gchar const *format, ...)
       string - the buffer to hold the output
       n - the maximum number of bytes to produce
       format - format string
       ... the arguments to insert in the output */
    g_snprintf(bin_name, 15, "source-bin-%02d", index);
    bin = gst_element_factory_make("uridecodebin", bin_name);
    g_object_set(G_OBJECT(bin), "uri", filename, NULL);
    /* g_signal_connect (instance, detailed_signal, c_handler, data)
       instance - the instance to connect to
       detailed_signal - a string of the form "signal-name::detail
       c_handler - the GCallback to connect
       data - data to pass to c_handler calls"*/
    g_signal_connect(G_OBJECT(bin), "pad-added", G_CALLBACK(cb_newpad), &g_source_id_list[index]);
    g_signal_connect(G_OBJECT(bin), "child-added", G_CALLBACK(decodebin_child_added), &g_source_id_list[index]);
    g_source_enabled[index] = TRUE;

    return bin;
}

static void stop_release_source(gint source_id) {
    GstStateChangeReturn state_return;
    gchar pad_name[16];
    GstPad *sinkpad = NULL;
    user_data[source_id].lcc_cnt_exit = 0;
    user_data[source_id].lcc_cnt_entry = 0;
    g_object_set(G_OBJECT(nvdsanalytics), "config-file", ANALYTICS_CONFIG_FILE, NULL);
    state_return = gst_element_set_state(g_source_bin_list[source_id], GST_STATE_NULL);
    switch (state_return) {
        case GST_STATE_CHANGE_SUCCESS:
            g_print("STATE CHANGE SUCCESS\n\n");
            g_snprintf(pad_name, 15, "sink_%u", source_id);
            sinkpad = gst_element_get_static_pad(streammux, pad_name);
            gst_pad_send_event(sinkpad, gst_event_new_flush_stop(FALSE));
            gst_element_release_request_pad(streammux, sinkpad);
            g_print("STATE CHANGE SUCCESS %p\n\n", sinkpad);
            gst_object_unref(sinkpad);
            gst_bin_remove(GST_BIN(pipeline), g_source_bin_list[source_id]);
            source_id--;
            g_num_sources--;
            break;
        case GST_STATE_CHANGE_FAILURE:
            g_print("STATE CHANGE FAILURE\n\n");
            break;
        case GST_STATE_CHANGE_ASYNC:
            g_print("STATE CHANGE ASYNC\n\n");
            state_return = gst_element_get_state(g_source_bin_list[source_id], NULL, NULL, GST_CLOCK_TIME_NONE);
            g_snprintf(pad_name, 15, "sink_%u", source_id);
            sinkpad = gst_element_get_static_pad(streammux, pad_name);
            gst_pad_send_event(sinkpad, gst_event_new_flush_stop(FALSE));
            gst_element_release_request_pad(streammux, sinkpad);
            g_print("STATE CHANGE ASYNC %p\n\n", sinkpad);
            gst_object_unref(sinkpad);
            gst_bin_remove(GST_BIN(pipeline), g_source_bin_list[source_id]);
            source_id--;
            g_num_sources--;
            break;
        case GST_STATE_CHANGE_NO_PREROLL:
            g_print("STATE CHANGE NO PREROLL\n\n");
            break;
        default:
            break;
    }
}

static gboolean add_sources(gpointer data, gchar *source_uri, gint source_id) {
    // gint source_id = 0;
    GstElement *source_bin;
    GstStateChangeReturn state_return;
    /*while (g_source_enabled[source_id] && source_id < MAX_NUM_SOURCES) {
      source_id ++;
    }*/
    // do {
    /* Generating random source id between 0 - MAX_NUM_SOURCES - 1,
     * which has not been enabled
     */
    // source_id = rand () % MAX_NUM_SOURCES;
    //} while (g_source_enabled[source_id] && source_id < MAX_NUM_SOURCES);
    g_source_enabled[source_id] = TRUE;

    g_print("Calling Start %d \n", source_id);
    source_bin = create_uridecode_bin(source_id, source_uri);
    if (!source_bin) {
        g_printerr("Failed to create source bin. Exiting.\n");
        return -1;
    }
    g_source_bin_list[source_id] = source_bin;
    gst_bin_add(GST_BIN(pipeline), source_bin);

    // add
    g_object_set(G_OBJECT(nvdsanalytics), "config-file", ANALYTICS_CONFIG_FILE, NULL);
    get_lc_type(ANALYTICS_CONFIG_FILE);

    state_return = gst_element_set_state(g_source_bin_list[source_id], GST_STATE_PLAYING);
    switch (state_return) {
        case GST_STATE_CHANGE_SUCCESS:
            g_print("STATE CHANGE SUCCESS\n\n");
            source_id++;
            break;
        case GST_STATE_CHANGE_FAILURE:
            g_print("STATE CHANGE FAILURE\n\n");
            break;
        case GST_STATE_CHANGE_ASYNC:
            g_print("STATE CHANGE ASYNC\n\n");
            state_return = gst_element_get_state(g_source_bin_list[source_id], NULL, NULL, GST_CLOCK_TIME_NONE);
            source_id++;
            break;
        case GST_STATE_CHANGE_NO_PREROLL:
            g_print("STATE CHANGE NO PREROLL\n\n");
            break;
        default:
            break;
    }
    g_num_sources++;

    // if (g_num_sources == MAX_NUM_SOURCES) { */
    /* We have reached MAX_NUM_SOURCES to be added, no stop calling this function
     * and enable calling delete sources
     */

    // g_timeout_add_seconds (10, delete_sources, (gpointer) g_source_bin_list);
    // return FALSE;
    //}
    if (g_num_sources == MAX_NUM_SOURCES) {
        g_print("already reached maximum (%d) of video streams\n", MAX_NUM_SOURCES);
    }
    // return TRUE;
    return TRUE;
}

static gboolean delete_sources(gpointer data, gint source_id_delete) {
    g_source_enabled[source_id_delete] = FALSE;
    g_video_source[source_id_delete] = g_strdup("\0");
    g_print("Calling Stop %d \n", source_id_delete);
    stop_release_source(source_id_delete);

    if (g_num_sources == 0) {
        g_main_loop_quit(loop);
        g_print("All sources Stopped quitting\n");
        return FALSE;
    }
    /*
    if (g_num_sources == 0) {
      g_main_loop_quit (loop);
      g_print ("All sources Stopped quitting\n");
      return FALSE;
    } */

    return TRUE;
}

static gboolean check_eos_source(gpointer g_source_bin_list) {
    g_print("check eos source!!!\n");
    gint source_id;
    gchar txt_line[200];
    g_mutex_lock(&eos_lock);
    for (source_id = 0; source_id < MAX_NUM_SOURCES; source_id++) {
        if (g_eos_list[source_id] == TRUE && g_source_enabled[source_id] == TRUE) {
            g_print("deleting eos source: %d\n", source_id);
            g_source_enabled[source_id] = FALSE;
            g_eos_list[source_id] = FALSE;
            g_video_source[source_id] = g_strdup("\0");
            int p = 0; // pointer of g_video_source
            FILE *pFile = fopen(VIDEO_SOURCE_FILE, "r+");
            FILE *tmpFile = fopen("tmp.txt", "w");
            if (pFile == NULL)
                perror("Error opening file");
            else {
                while (fgets(txt_line, 200, pFile) != NULL) {
                    if (p == source_id) {
                        fputs("\n", tmpFile);
                    } else {
                        fputs(txt_line, tmpFile);
                    }
                    p++;
                }
                fclose(pFile);
                fclose(tmpFile);
                remove(VIDEO_SOURCE_FILE);
                rename("tmp.txt", VIDEO_SOURCE_FILE);
            }
            stop_release_source(source_id);
        }
    }
    g_mutex_unlock(&eos_lock);

    if (g_num_sources == 0) {
        g_main_loop_quit(loop);
        g_print("All sources Stopped quitting\n");
        return FALSE;
    }
    return TRUE;
}

static gboolean read_source_txt(gchar *sourcefile) {
    g_print("read source txt!!!\n");
    FILE *pFile = NULL;
    gchar txt_line[200];

    g_print("-------video source before------\n");
    for (int i = 0; i < MAX_NUM_SOURCES; i++) {
        g_print("%d: ", i);
        g_print("%s\n", g_video_source[i]);
    }

    int p = 0; // pointer of g_video_source
    pFile = fopen(sourcefile, "r");
    if (pFile == NULL)
        perror("Error opening file");
    else {
        while (fgets(txt_line, 200, pFile) != NULL) {
            int len = strlen(txt_line);
            // g_print("len: %d", len);
            if (txt_line[len - 1] == '\n') txt_line[len - 1] = '\0';
            gchar *tmp = (char *)g_malloc0(200 * sizeof(gchar));
            memcpy(tmp, txt_line, len);
            // g_print("tmp: %s", tmp);
            if (g_strcmp0(tmp, g_video_source[p]) != 0) {
                if (strlen(tmp) == 0 && g_video_source[p]) {
                    g_print("deleting stream source id = %d\n", p);
                    delete_sources((gpointer)g_source_bin_list, p);
                } else if (strlen(tmp)) {
                    g_video_source[p] = tmp;
                    g_print("add new stream [%s]\n", g_video_source[p]);
                    // g_num_sources ++;
                    add_sources((gpointer)g_source_bin_list, g_video_source[p], p);
                    // add_sources((gpointer) g_source_bin_list, g_video_source[p]);
                }
            }
            p++;
        }
        g_print("p: %d", p);
        g_print("g_num_sources: %d", g_num_sources);
        if (p < g_num_sources) {
            for (int i = p; i < g_num_sources; i++) {
                g_print("deleting stream source id = %d\n", i);
                g_video_source[i] = g_strdup("\0");
                delete_sources((gpointer)g_source_bin_list, i);
            }
        }
        fclose(pFile);
    }
    // check_eos_source((gpointer) g_source_bin_list);

    g_print("-------video source after------\n");
    for (int i = 0; i < MAX_NUM_SOURCES; i++) {
        g_print("%d: ", i);
        g_print("%s\n", g_video_source[i]);
    }
    return TRUE;
}

int main(int argc, char *argv[]) {
    GstElement *queue1, *queue2, *queue3, *queue4, *queue5, *queue6, *queue7, *queue8;
    GstElement *msgconv = NULL, *msgbroker = NULL, *tee = NULL;
#ifdef PLATFORM_TEGRA
    GstElement *transform = NULL;
#endif
    GstBus *bus = NULL;
    guint bus_watch_id;
    GstPad *nvdsanalytics_src_pad = NULL;
    GstPad *osd_sink_pad = NULL;
    GstPad *tee_render_pad = NULL;
    GstPad *tee_msg_pad = NULL;
    GstPad *sink_pad = NULL;
    GstPad *src_pad = NULL;
    guint i, num_sources;
    guint tiler_rows, tiler_columns;
    guint pgie_batch_size;

    /* Check input arguments */
    if (argc < 2) {
        g_printerr("Usage: %s <uri1> [uri2] ... [uriN] \n", argv[0]);
        return -1;
    }
    num_sources = argc - 1;

    g_setenv("GST_DEBUG_DUMP_DOT_DIR", "/tmp/", TRUE);

    /* Standard GStreamer initialization */
    gst_init(&argc, &argv);
    loop = g_main_loop_new(NULL, FALSE);

    /* Create gstreamer elements */
    /* Create Pipeline element that will form a connection of other elements */
    pipeline = gst_pipeline_new("nvdsanalytics-test-pipeline");

    /* Create nvstreammux instance to form batches from one or more sources. */
    streammux = gst_element_factory_make("nvstreammux", "stream-muxer");

    if (!pipeline || !streammux) {
        g_printerr("One element could not be created. Exiting.\n");
        return -1;
    }
    gst_bin_add(GST_BIN(pipeline), streammux);
    g_object_set(G_OBJECT(streammux), "live-source", 1, NULL);

    g_source_bin_list = (GstElement **)g_malloc0(sizeof(GstElement *) * MAX_NUM_SOURCES);

    get_lc_type(ANALYTICS_CONFIG_FILE);

    FILE *pFile = NULL;

    for (i = 0; i < num_sources; i++) {
        if (i == 0) {
            pFile = fopen(VIDEO_SOURCE_FILE, "w");
        } else {
            pFile = fopen(VIDEO_SOURCE_FILE, "a");
        }
        if (pFile == NULL)
            perror("Error opening file");
        else {
            fputs(argv[i + 1], pFile);
            fputs("\n", pFile);
            fclose(pFile);
        }
        GstPad *sinkpad, *srcpad;
        gchar pad_name[16] = {};
        // ReplaceSubStr(argv[i+1], "#", "&", argv[i+1]);
        GstElement *source_bin = create_uridecode_bin(i, argv[i + 1]);
        if (!source_bin) {
            g_printerr("Failed to create source bin. Exiting.\n");
            return -1;
        }
        // g_print("%s", argv[i + 1]);
        g_video_source[i] = argv[i + 1];
        g_source_bin_list[i] = source_bin;

        gst_bin_add(GST_BIN(pipeline), source_bin);
    }

    g_num_sources = num_sources;

    /* Use nvinfer to infer on batched frame. */
    pgie = gst_element_factory_make("nvinfer", "primary-nvinference-engine");

    /* Use nvtracker to track detections on batched frame. */
    nvtracker = gst_element_factory_make("nvtracker", "nvtracker");

    /* Use nvdsanalytics to perform analytics on object */
    nvdsanalytics = gst_element_factory_make("nvdsanalytics", "nvdsanalytics");

    /* Use nvtiler to composite the batched frames into a 2D tiled array based
     * on the source of the frames. */
    tiler = gst_element_factory_make("nvmultistreamtiler", "nvtiler");

    /* Use convertor to convert from NV12 to RGBA as required by nvosd */
    nvvidconv = gst_element_factory_make("nvvideoconvert", "nvvideo-converter");

    /* Create OSD to draw on the converted RGBA buffer */
    nvosd = gst_element_factory_make("nvdsosd", "nv-onscreendisplay");

    /* Create msg converter to generate payload from buffer metadata */
    msgconv = gst_element_factory_make("nvmsgconv", "nvmsg-converter");

    /* Create msg broker to send payload to server */
    msgbroker = gst_element_factory_make("nvmsgbroker", "nvmsg-broker");

    /* Create tee to render buffer and send message simultaneously*/
    tee = gst_element_factory_make("tee", "nvsink-tee");

    /* Add queue elements between every two elements */
    queue1 = gst_element_factory_make("queue", "queue1");
    queue2 = gst_element_factory_make("queue", "queue2");
    queue3 = gst_element_factory_make("queue", "queue3");
    queue4 = gst_element_factory_make("queue", "queue4");
    queue5 = gst_element_factory_make("queue", "queue5");
    queue6 = gst_element_factory_make("queue", "queue6");
    queue7 = gst_element_factory_make("queue", "queue7");
    queue8 = gst_element_factory_make("queue", "queue8");

    /* Finally render the osd output */
#ifdef PLATFORM_TEGRA
    transform = gst_element_factory_make("nvegltransform", "nvegl-transform");
#endif
    sink = gst_element_factory_make("nveglglessink", "nvvideo-renderer");

    if (!pgie || !nvtracker || !nvdsanalytics || !tiler || !nvvidconv || !nvosd || !msgconv || !msgbroker || !tee ||
        !sink || !queue1 || !queue2 || !queue3 || !queue4 || !queue5 || !queue6 || !queue7 || !queue8) {
        g_printerr("One element could not be created. Exiting.\n");
        return -1;
    }

#ifdef PLATFORM_TEGRA
    if (!transform) {
        g_printerr("One tegra element could not be created. Exiting.\n");
        return -1;
    }
#endif

    g_object_set(G_OBJECT(streammux), "width", MUXER_OUTPUT_WIDTH, "height", MUXER_OUTPUT_HEIGHT, "batch-size",
                 num_sources, "batched-push-timeout", MUXER_BATCH_TIMEOUT_USEC, NULL);

    /* Configure the nvinfer element using the nvinfer config file. */
    g_object_set(G_OBJECT(pgie), "config-file-path", PGIE_CONFIG_FILE, NULL);
    /* g_object_set (G_OBJECT (pgie),
        "config-file-path", "config_infer_primary_yoloV3.txt", NULL); */

    /* Configure the nvtracker element for using the particular tracker algorithm. */
    g_object_set(G_OBJECT(nvtracker), "ll-lib-file", "/opt/nvidia/deepstream/deepstream-5.0/lib/libnvds_nvdcf.so",
                 "ll-config-file", "tracker_config.yml", "tracker-width", 640, "tracker-height", 480, NULL);

    /* Configure the nvdsanalytics element for using the particular analytics config file*/
    g_object_set(G_OBJECT(nvdsanalytics), "config-file", ANALYTICS_CONFIG_FILE, NULL);

    /* Override the batch-size set in the config file with the number of sources. */
    // g_object_get (G_OBJECT (pgie), "batch-size", &pgie_batch_size, NULL);
    /* if (pgie_batch_size != num_sources) {
      g_printerr
          ("WARNING: Overriding infer-config batch-size (%d) with number of sources (%d)\n",
          pgie_batch_size, num_sources);
      g_object_set (G_OBJECT (pgie), "batch-size", num_sources, NULL);
    }*/

    g_object_set(G_OBJECT(msgconv), "config", MSCONV_CONFIG_FILE, NULL);
    g_object_set(G_OBJECT(msgconv), "payload-type", schema_type, NULL);

    g_object_set(G_OBJECT(msgbroker), "proto-lib", proto_lib, "conn-str", conn_str, "sync", FALSE, NULL);

    if (topic) {
        g_object_set(G_OBJECT(msgbroker), "topic", topic, NULL);
    }

    if (cfg_file) {
        g_object_set(G_OBJECT(msgbroker), "config", cfg_file, NULL);
    }

    tiler_rows = (guint)sqrt(num_sources);
    tiler_columns = (guint)ceil(1.0 * num_sources / tiler_rows);
    /* we set the tiler properties here */
    g_object_set(G_OBJECT(tiler), "rows", tiler_rows, "columns", tiler_columns, "width", TILED_OUTPUT_WIDTH, "height",
                 TILED_OUTPUT_HEIGHT, NULL);

    g_object_set(G_OBJECT(sink), "qos", 0, NULL);

    /* we add a message handler */
    bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline));
    bus_watch_id = gst_bus_add_watch(bus, bus_call, loop);
    gst_object_unref(bus);

    /* Set up the pipeline */
    /* we add all elements into the pipeline */
#ifdef PLATFORM_TEGRA
    gst_bin_add_many(GST_BIN(pipeline), queue1, pgie, queue2, nvtracker, queue3, nvdsanalytics, queue4, tiler, queue5,
                     nvosd, tee, msgconv, msgbroker, nvvidconv, queue6, queue7, queue8, transform, sink, NULL);

    /* we link the elements together, with queues in between
     * nvstreammux -> nvinfer -> nvtracker -> nvdsanalytics -> nvtiler ->
     * nvvideoconvert -> nvosd -> transform -> sink
     */
    if (!gst_element_link_many(streammux, queue1, pgie, queue2, nvtracker, queue3, nvdsanalytics, queue4, tiler, queue5,
                               nvvidconv, queue6, nvosd, tee, NULL)) {
        g_printerr("1Elements could not be linked. Exiting.\n");
        return -1;
    }
    if (!gst_element_link_many(queue7, msgconv, msgbroker, NULL)) {
        g_printerr("2Elements could not be linked. Exiting.\n");
        return -1;
    }
    if (!display_off) {
        if (!gst_element_link_many(queue8, transform, sink, NULL)) {
            g_printerr("3Elements could not be linked. Exiting.\n");
            return -1;
        }
    } else {
        if (!gst_element_link(queue8, sink)) {
            g_printerr("4Elements could not be linked. Exiting.\n");
            return -1;
        }
    }

#else

    gst_bin_add_many(GST_BIN(pipeline), queue1, pgie, queue2, nvtracker, queue3, nvdsanalytics, queue4, tiler, queue5,
                     nvosd, tee, msgconv, msgbroker, nvvidconv, queue6, queue7, queue8, sink, NULL);

    /* we link the elements together, with queues in between
     * nvstreammux -> nvinfer -> nvtracker -> nvdsanalytics -> nvtiler ->
     * nvvideoconvert -> nvosd -> transform -> sink
     */
    if (!gst_element_link_many(streammux, queue1, pgie, queue2, nvtracker, queue3, nvdsanalytics, queue4, tiler, queue5,
                               nvvidconv, queue6, nvosd, tee, NULL)) {
        g_printerr("1Elements could not be linked. Exiting.\n");
        return -1;
    }
    if (!gst_element_link_many(queue7, msgconv, msgbroker, NULL)) {
        g_printerr("2Elements could not be linked. Exiting.\n");
        return -1;
    }
    if (!gst_element_link(queue8, sink)) {
        g_printerr("4Elements could not be linked. Exiting.\n");
        return -1;
    }

#endif

    sink_pad = gst_element_get_static_pad(queue7, "sink");
    tee_msg_pad = gst_element_get_request_pad(tee, "src_%u");
    tee_render_pad = gst_element_get_request_pad(tee, "src_%u");
    if (!tee_msg_pad || !tee_render_pad) {
        g_printerr("Unable to get request pads\n");
        return -1;
    }

    if (gst_pad_link(tee_msg_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Unable to link tee and message converter\n");
        gst_object_unref(sink_pad);
        return -1;
    }

    gst_object_unref(sink_pad);

    sink_pad = gst_element_get_static_pad(queue8, "sink");
    if (gst_pad_link(tee_render_pad, sink_pad) != GST_PAD_LINK_OK) {
        g_printerr("Unable to link tee and render\n");
        gst_object_unref(sink_pad);
        return -1;
    }

    gst_object_unref(sink_pad);

    /* Lets add probe to get informed of the meta data generated, we add probe to
     * the sink pad of the nvdsanalytics element, since by that time, the buffer
     * would have had got all the metadata.
     */
    nvdsanalytics_src_pad = gst_element_get_static_pad(nvdsanalytics, "src");
    if (!nvdsanalytics_src_pad)
        g_print("Unable to get src pad\n");
    else
        gst_pad_add_probe(nvdsanalytics_src_pad, GST_PAD_PROBE_TYPE_BUFFER, nvdsanalytics_src_pad_buffer_probe, NULL,
                          NULL);
    gst_object_unref(nvdsanalytics_src_pad);

    // save dot file
    GST_DEBUG_BIN_TO_DOT_FILE((GstBin *)pipeline, GST_DEBUG_GRAPH_SHOW_ALL, "pipeline");

    /* Set the pipeline to "playing" state */
    g_print("Now playing:");
    for (i = 0; i < num_sources; i++) {
        g_print(" %s,", argv[i + 1]);
    }
    g_print("\n");
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    /* Wait till pipeline encounters an error or EOS */
    g_print("Running...\n");
    g_timeout_add_seconds(20, (GSourceFunc)read_source_txt, VIDEO_SOURCE_FILE);
    g_timeout_add_seconds(50, check_eos_source, (gpointer)g_source_bin_list);

    g_main_loop_run(loop);

    /* Out of the main loop, clean up nicely */
    g_print("Returned, stopping playback\n");
    gst_element_set_state(pipeline, GST_STATE_NULL);
    g_print("Deleting pipeline\n");
    gst_object_unref(GST_OBJECT(pipeline));
    g_source_remove(bus_watch_id);
    g_main_loop_unref(loop);
    return 0;
}
