#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <sys/stat.h>
#include <curl/curl.h>
#include <cJSON.h>
#include <pthread.h>

#include "core/logger.h"
#include "core/config.h"
#include "core/shutdown_coordinator.h"
#include "video/api_detection.h"
#include "video/detection_result.h"
#include "video/stream_manager.h"
#include "video/stream_state.h"
#include "database/db_detections.h"

// Global variables
static bool initialized = false;
static CURL *curl_handle = NULL;
static pthread_mutex_t curl_mutex = PTHREAD_MUTEX_INITIALIZER;

// Structure to hold memory for curl response
typedef struct {
    char *memory;
    size_t size;
} memory_struct_t;

// Callback function for curl to write data
static size_t write_memory_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    memory_struct_t *mem = (memory_struct_t *)userp;

    char *ptr = realloc(mem->memory, mem->size + realsize + 1);
    if (!ptr) {
        log_error("Not enough memory for curl response");
        return 0;
    }

    mem->memory = ptr;
    memcpy(&(mem->memory[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->memory[mem->size] = 0;

    return realsize;
}

/**
 * Initialize the API detection system
 */
int init_api_detection_system(void) {
    if (initialized && curl_handle) {
        log_info("API detection system already initialized");
        return 0;  // Already initialized and curl handle is valid
    }

    // If we have a curl handle but initialized is false, clean it up first
    if (curl_handle) {
        log_warn("API detection system has a curl handle but is marked as uninitialized, cleaning up");
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }

    // Initialize curl
    CURLcode global_init_result = curl_global_init(CURL_GLOBAL_ALL);
    if (global_init_result != CURLE_OK) {
        log_error("Failed to initialize curl global: %s", curl_easy_strerror(global_init_result));
        return -1;
    }

    curl_handle = curl_easy_init();
    if (!curl_handle) {
        log_error("Failed to initialize curl handle");
        curl_global_cleanup();
        return -1;
    }

    initialized = true;
    log_info("API detection system initialized successfully");
    return 0;
}

/**
 * Shutdown the API detection system
 */
void shutdown_api_detection_system(void) {
    // CRITICAL FIX: Always attempt to clean up resources, even if not marked as initialized
    log_info("Shutting down API detection system (initialized: %s, curl_handle: %p)",
             initialized ? "yes" : "no", (void*)curl_handle);

    // Cleanup curl handle if it exists
    if (curl_handle) {
        log_info("Cleaning up curl handle");
        curl_easy_cleanup(curl_handle);
        curl_handle = NULL;
    }

    // Only call global cleanup if we were initialized
    if (initialized) {
        log_info("Cleaning up curl global resources");
        curl_global_cleanup();
    }

    initialized = false;
    log_info("API detection system shutdown complete");
}

/**
 * Detect objects using the API
 */
int detect_objects_api(const char *api_url, const unsigned char *frame_data,
                      int width, int height, int channels, detection_result_t *result,
                      const char *stream_name) {
    // CRITICAL FIX: Check if we're in shutdown mode or if the stream has been stopped
    if (is_shutdown_initiated()) {
        log_info("API Detection: System shutdown in progress, skipping detection");
        return -1;
    }

    // CRITICAL FIX: Add thread safety for curl operations
    pthread_mutex_lock(&curl_mutex);
    
    // Initialize result to empty at the beginning to prevent segmentation fault
    if (result) {
        memset(result, 0, sizeof(detection_result_t));
    } else {
        log_error("API Detection: NULL result pointer provided");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }
    
    // CRITICAL FIX: Check if api_url is the special "api-detection" string
    // If so, get the actual URL from the global config
    const char *actual_api_url = api_url;
    if (api_url && strcmp(api_url, "api-detection") == 0) {
        // Get the API URL from the global config
        extern config_t g_config;
        actual_api_url = g_config.api_detection_url;
        log_info("API Detection: Using API URL from config: %s", actual_api_url ? actual_api_url : "NULL");
    }

    log_info("API Detection: Starting detection with API URL: %s", actual_api_url);
    log_info("API Detection: Frame dimensions: %dx%d, channels: %d", width, height, channels);
    log_info("API Detection: Stream name: %s", stream_name ? stream_name : "NULL");

    if (!initialized || !curl_handle) {
        log_error("API detection system not initialized");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    if (!actual_api_url || !frame_data || !result) {
        log_error("Invalid parameters for detect_objects_api");
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Check if the URL is valid (must start with http:// or https://)
    if (strncmp(actual_api_url, "http://", 7) != 0 && strncmp(actual_api_url, "https://", 8) != 0) {
        log_error("API Detection: Invalid URL format: %s (must start with http:// or https://)", actual_api_url);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Set up curl for multipart/form-data
    struct curl_httppost *formpost = NULL;
    struct curl_httppost *lastptr = NULL;

    // Create a temporary file for the image data using mkstemp (safer than tmpnam)
    char temp_filename[] = "/tmp/lightnvr_api_detection_XXXXXX";
    int temp_fd = mkstemp(temp_filename);
    if (temp_fd < 0) {
        log_error("Failed to create temporary file: %s", strerror(errno));
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    char image_filename[256];
    snprintf(image_filename, sizeof(image_filename), "%s.jpg", temp_filename);

    // Convert file descriptor to FILE pointer
    FILE *temp_file = fdopen(temp_fd, "wb");
    if (!temp_file) {
        log_error("Failed to open temporary file for writing: %s", strerror(errno));
        close(temp_fd);
        unlink(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Write the raw image data to the file
    size_t bytes_written = fwrite(frame_data, 1, width * height * channels, temp_file);
    fclose(temp_file);

    if (bytes_written != width * height * channels) {
        log_error("Failed to write image data to temporary file");
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // CRITICAL FIX: Modify the request to match the API server's expected format
    // Based on working curl examples:
    // curl -X 'POST' 'http://127.0.0.1:9001/api/v1/detect?backend=tflite&confidence_threshold=.5&return_image=false'
    //   -H 'accept: application/json' -H 'Content-Type: multipart/form-data'
    //   -F 'file=@image.jpg;type=image/jpeg'

    // CRITICAL FIX: Determine the correct ImageMagick parameters based on channels with safety checks
    const char *pixel_format = NULL;

    // Validate channels to prevent segmentation faults
    if (channels <= 0 || channels > 4) {
        log_error("API Detection: Invalid number of channels: %d (must be 1, 3, or 4)", channels);
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Set pixel format based on validated channels
    if (channels == 1) {
        pixel_format = "gray";
        log_info("API Detection: Using grayscale pixel format for %d channel", channels);
    } else if (channels == 3) {
        pixel_format = "rgb";
        log_info("API Detection: Using RGB pixel format for %d channels", channels);
    } else if (channels == 4) {
        pixel_format = "rgba";
        log_info("API Detection: Using RGBA pixel format for %d channels", channels);
    }

    // Double-check that pixel_format is set
    if (!pixel_format) {
        log_error("API Detection: Failed to determine pixel format for %d channels", channels);
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Validate width and height to prevent buffer overflows
    if (width <= 0 || width > 10000 || height <= 0 || height > 10000) {
        log_error("API Detection: Invalid image dimensions: %dx%d", width, height);
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }


    // Try an alternative approach using ffmpeg with safety checks
    log_info("API Detection: Trying alternative conversion with ffmpeg");
    char ffmpeg_cmd[1024] = {0}; // Initialize to zeros

    // Determine the correct ffmpeg pixel format with safety checks
    const char *ffmpeg_pixel_format = NULL;
    if (channels == 1) {
        ffmpeg_pixel_format = "gray";
    } else if (channels == 3) {
        ffmpeg_pixel_format = "rgb24";
    } else if (channels == 4) {
        ffmpeg_pixel_format = "rgba";
    } else {
        log_error("API Detection: Invalid number of channels for ffmpeg: %d", channels);
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Format the command with safety checks
    int cmd_result = snprintf(ffmpeg_cmd, sizeof(ffmpeg_cmd),
            "ffmpeg -f rawvideo -pixel_format %s -video_size %dx%d -i %s -y %s",
            ffmpeg_pixel_format, width, height, temp_filename, image_filename);

    // Check for buffer overflow
    if (cmd_result < 0 || cmd_result >= (int)sizeof(ffmpeg_cmd)) {
        log_error("API Detection: Command buffer overflow when formatting ffmpeg command");
        remove(temp_filename);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    log_info("API Detection: Running ffmpeg command: %s", ffmpeg_cmd);

    int ffmpeg_result = system(ffmpeg_cmd);
    if (ffmpeg_result != 0) {
        log_error("API Detection: Failed to convert with ffmpeg (error code: %d)", ffmpeg_result);
        log_info("API Detection: Falling back to raw data with application/octet-stream MIME type");

        // CRITICAL FIX: Verify that temp_filename exists before adding it to the form
        struct stat temp_stat;
        if (stat(temp_filename, &temp_stat) != 0 || temp_stat.st_size == 0) {
            log_error("API Detection: Temporary file is missing or empty: %s", temp_filename);
            remove(temp_filename);
            pthread_mutex_unlock(&curl_mutex);
            return -1;
        }

        // Add the file to the form with the correct field name 'file' (not 'image')
        CURLFORMcode form_result = curl_formadd(&formpost, &lastptr,
                    CURLFORM_COPYNAME, "file",
                    CURLFORM_FILE, temp_filename,
                    CURLFORM_CONTENTTYPE, "application/octet-stream",
                    CURLFORM_END);

        // Check for curl form errors
        if (form_result != CURL_FORMADD_OK) {
            log_error("API Detection: Failed to add file to form (error code: %d)", form_result);
            remove(temp_filename);
            pthread_mutex_unlock(&curl_mutex);
            return -1;
        }
    } else {
        log_info("API Detection: Successfully converted raw data to JPEG with ffmpeg: %s", image_filename);

        // CRITICAL FIX: Verify that image_filename exists before using it
        struct stat image_stat;
        if (stat(image_filename, &image_stat) != 0 || image_stat.st_size == 0) {
            log_error("API Detection: Converted image file is missing or empty: %s", image_filename);
            remove(temp_filename);
            pthread_mutex_unlock(&curl_mutex);
            return -1;
        }
    }

    // Add the JPEG file to the form
    // CRITICAL FIX: Check if image_filename exists before adding to form
    struct stat image_stat;
    memory_struct_t chunk = {0};
    chunk.memory = NULL;
    struct curl_slist *headers = NULL;
    if (stat(image_filename, &image_stat) == 0 && image_stat.st_size > 0) {
        CURLFORMcode form_result = curl_formadd(&formpost, &lastptr,
                    CURLFORM_COPYNAME, "file",
                    CURLFORM_FILE, image_filename,
                    CURLFORM_CONTENTTYPE, "image/jpeg",
                    CURLFORM_END);
                    
        if (form_result != CURL_FORMADD_OK) {
            log_error("API Detection: Failed to add JPEG file to form (error code: %d)", form_result);
            free(chunk.memory);
            curl_formfree(formpost);
            curl_slist_free_all(headers);
            remove(temp_filename);
            remove(image_filename);
            result->count = 0;
            pthread_mutex_unlock(&curl_mutex);
            return -1;
        }
        
        log_info("API Detection: Successfully added JPEG file to form: %s", image_filename);
    } else {
        log_error("API Detection: JPEG file missing or empty: %s", image_filename);
        // Now we can safely free them
        if (chunk.memory) free(chunk.memory);
        if (formpost) curl_formfree(formpost);
        if (headers) curl_slist_free_all(headers);
        
        remove(temp_filename);
        if (stat(image_filename, &image_stat) == 0) {
            remove(image_filename);
        }
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Construct the URL with query parameters
    char url_with_params[1024];
    snprintf(url_with_params, sizeof(url_with_params),
             "%s?backend=tflite&confidence_threshold=0.5&return_image=false",
             actual_api_url);
    log_info("API Detection: Using URL with parameters: %s", url_with_params);

    // Set up the request with the URL including query parameters
    curl_easy_setopt(curl_handle, CURLOPT_URL, url_with_params);
    curl_easy_setopt(curl_handle, CURLOPT_HTTPPOST, formpost);

    // Add the accept header
    headers = curl_slist_append(headers, "accept: application/json");
    curl_easy_setopt(curl_handle, CURLOPT_HTTPHEADER, headers);

    // Set up the response buffer
    chunk.memory = malloc(1);
    chunk.size = 0;

    curl_easy_setopt(curl_handle, CURLOPT_WRITEFUNCTION, write_memory_callback);
    curl_easy_setopt(curl_handle, CURLOPT_WRITEDATA, (void *)&chunk);

    // Set a timeout
    curl_easy_setopt(curl_handle, CURLOPT_TIMEOUT, 10);

    // Perform the request
    log_info("API Detection: Sending request to %s", url_with_params);

    // CRITICAL FIX: Check if curl handle is still valid before performing the request
    if (!curl_handle || !initialized) {
        log_error("API Detection: curl handle is invalid or API detection system not initialized");
        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);

        // Clean up the temporary files
        remove(temp_filename);
        // Remove the JPEG file if it was created by either ImageMagick or ffmpeg
        struct stat st;
        if (stat(image_filename, &st) == 0) {
            log_info("API Detection: Removing temporary JPEG file: %s", image_filename);
            remove(image_filename);
        }

        // Initialize result to empty to prevent segmentation fault
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    CURLcode res = curl_easy_perform(curl_handle);

    // Clean up the temporary files
    remove(temp_filename);
    // Remove the JPEG file if it was created by either ImageMagick or ffmpeg
    struct stat st;
    if (stat(image_filename, &st) == 0) {
        log_info("API Detection: Removing temporary JPEG file: %s", image_filename);
        remove(image_filename);
    }

    // Check for errors
    if (res != CURLE_OK) {
        log_error("API Detection: curl_easy_perform() failed: %s", curl_easy_strerror(res));

        // Check if it's a connection error
        if (res == CURLE_COULDNT_CONNECT) {
            log_error("API Detection: Could not connect to server at %s. Is the API server running?", url_with_params);
        } else if (res == CURLE_OPERATION_TIMEDOUT) {
            log_error("API Detection: Connection to %s timed out. Server might be slow or unreachable.", url_with_params);
        } else if (res == CURLE_COULDNT_RESOLVE_HOST) {
            log_error("API Detection: Could not resolve host %s. Check your network connection and DNS settings.", url_with_params);
        } else if (res == CURLE_FAILED_INIT) {
            log_error("API Detection: Curl initialization failed. Reinitializing curl handle...");

            // Attempt to reinitialize curl
            if (curl_handle) {
                curl_easy_cleanup(curl_handle);
                curl_handle = NULL;
            }

            curl_handle = curl_easy_init();
            if (!curl_handle) {
                log_error("API Detection: Failed to reinitialize curl handle");
                initialized = false; // Mark as uninitialized to force reinitialization next time
            } else {
                log_info("API Detection: Successfully reinitialized curl handle");
            }
        }

        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);

        // Initialize result to empty to prevent segmentation fault
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Get the HTTP response code
    long http_code = 0;
    curl_easy_getinfo(curl_handle, CURLINFO_RESPONSE_CODE, &http_code);

    if (http_code != 200) {
        log_error("API request failed with HTTP code %ld", http_code);
        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Parse the JSON response
    // CRITICAL FIX: Add additional logging and validation for JSON parsing
    if (!chunk.memory || chunk.size == 0) {
        log_error("API Detection: Empty response from server");
        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
        // Initialize result to empty to prevent segmentation fault
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Log the first few bytes of the response for debugging
    char preview[64] = {0};
    int preview_len = chunk.size < 63 ? chunk.size : 63;
    memcpy(preview, chunk.memory, preview_len);
    preview[preview_len] = '\0';
    // Replace non-printable characters with dots
    for (int i = 0; i < preview_len; i++) {
        if (preview[i] < 32 || preview[i] > 126) {
            preview[i] = '.';
        }
    }
    log_info("API Detection: Response preview: %s", preview);

    cJSON *root = cJSON_Parse(chunk.memory);

    if (!root) {
        const char *error_ptr = cJSON_GetErrorPtr();
        log_error("Failed to parse JSON response: %s", error_ptr ? error_ptr : "Unknown error");
        // Log more details about the response
        log_error("API Detection: Response size: %zu bytes", chunk.size);
        log_error("API Detection: Response preview: %s", preview);
        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
        // Initialize result to empty to prevent segmentation fault
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Extract the detections
    cJSON *detections = cJSON_GetObjectItem(root, "detections");
    if (!detections || !cJSON_IsArray(detections)) {
        log_error("Invalid JSON response: missing or invalid 'detections' array");
        // Log the JSON for debugging
        char *json_str = cJSON_Print(root);
        if (json_str) {
            log_error("API Detection: Full JSON response: %s", json_str);
            free(json_str);
        }
        cJSON_Delete(root);
        free(chunk.memory);
        curl_formfree(formpost);
        curl_slist_free_all(headers);
        // Initialize result to empty to prevent segmentation fault
        result->count = 0;
        pthread_mutex_unlock(&curl_mutex);
        return -1;
    }

    // Process each detection
    int array_size = cJSON_GetArraySize(detections);
    for (int i = 0; i < array_size; i++) {
        if (result->count >= MAX_DETECTIONS) {
            log_warn("Maximum number of detections reached (%d)", MAX_DETECTIONS);
            break;
        }

        cJSON *detection = cJSON_GetArrayItem(detections, i);
        if (!detection) continue;

        // Extract the detection data
        cJSON *label = cJSON_GetObjectItem(detection, "label");
        cJSON *confidence = cJSON_GetObjectItem(detection, "confidence");

        // The bounding box coordinates might be in a nested object
        cJSON *bounding_box = cJSON_GetObjectItem(detection, "bounding_box");
        cJSON *x_min = NULL;
        cJSON *y_min = NULL;
        cJSON *x_max = NULL;
        cJSON *y_max = NULL;

        if (bounding_box) {
            // Get coordinates from the nested bounding_box object
            x_min = cJSON_GetObjectItem(bounding_box, "x_min");
            y_min = cJSON_GetObjectItem(bounding_box, "y_min");
            x_max = cJSON_GetObjectItem(bounding_box, "x_max");
            y_max = cJSON_GetObjectItem(bounding_box, "y_max");
            log_info("API Detection: Found bounding_box object in JSON response");
        } else {
            // Try to get coordinates directly from the detection object (old format)
            x_min = cJSON_GetObjectItem(detection, "x_min");
            y_min = cJSON_GetObjectItem(detection, "y_min");
            x_max = cJSON_GetObjectItem(detection, "x_max");
            y_max = cJSON_GetObjectItem(detection, "y_max");
            log_info("API Detection: Using direct coordinates from JSON response");
        }

        if (!label || !cJSON_IsString(label) ||
            !confidence || !cJSON_IsNumber(confidence) ||
            !x_min || !cJSON_IsNumber(x_min) ||
            !y_min || !cJSON_IsNumber(y_min) ||
            !x_max || !cJSON_IsNumber(x_max) ||
            !y_max || !cJSON_IsNumber(y_max)) {
            log_warn("Invalid detection data in JSON response");
            // Log the actual JSON for debugging
            char *json_str = cJSON_Print(detection);
            if (json_str) {
                log_warn("Detection JSON: %s", json_str);
                free(json_str);
            }
            continue;
        }

        // Add the detection to the result
        strncpy(result->detections[result->count].label, label->valuestring, MAX_LABEL_LENGTH - 1);
        result->detections[result->count].label[MAX_LABEL_LENGTH - 1] = '\0';
        result->detections[result->count].confidence = confidence->valuedouble;
        result->detections[result->count].x = x_min->valuedouble;
        result->detections[result->count].y = y_min->valuedouble;
        result->detections[result->count].width = x_max->valuedouble - x_min->valuedouble;
        result->detections[result->count].height = y_max->valuedouble - y_min->valuedouble;

        result->count++;
    }

    // Store the detections in the database if we have a valid stream name
    if (stream_name && stream_name[0] != '\0') {
        store_detections_in_db(stream_name, result, 0); // 0 means use current time
    } else {
        log_warn("No stream name provided, skipping database storage");
    }

    // Clean up
    cJSON_Delete(root);
    free(chunk.memory);
    curl_formfree(formpost);
    curl_slist_free_all(headers);

    pthread_mutex_unlock(&curl_mutex);
    return 0;
}
