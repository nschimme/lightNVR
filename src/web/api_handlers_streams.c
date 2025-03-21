#define _XOPEN_SOURCE
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h>

#include "core/config.h"
#include "core/logger.h"
#include "database/database_manager.h"
#include "video/stream_manager.h"
#include "video/stream_state.h"
#include "video/streams.h"
#include "web/request_response.h"
#include "web/api_handlers_streams.h"
#include "web/api_handlers_common.h"
#include "video/detection_stream.h"

// Forward declarations of helper functions
static char* create_stream_json(const stream_config_t *stream);
static char* create_streams_json_array();
static int parse_stream_json(const char *json, stream_config_t *stream);

/**
 * Handle GET request for streams
 */
void handle_get_streams(const http_request_t *request, http_response_t *response) {
    char *json_array = create_streams_json_array();
    if (!json_array) {
        create_json_response(response, 500, "{\"error\": \"Failed to create streams JSON\"}");
        return;
    }
    
    // Create response
    create_json_response(response, 200, json_array);
    
    // Free resources
    free(json_array);
}

/**
 * Improved handler for stream API endpoints that correctly handles URL-encoded identifiers
 */
void handle_get_stream(const http_request_t *request, http_response_t *response) {
    // Extract stream ID from the URL
    // URL pattern can be:
    // 1. /api/streams/123 (numeric ID)
    // 2. /api/streams/Front%20Door (URL-encoded stream name)

    const char *path = request->path;
    const char *prefix = "/api/streams/";

    // Log the request
    log_debug("Stream request path: %s", path);

    // Verify path starts with expected prefix
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        log_error("Invalid request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid request path\"}");
        return;
    }

    // Extract the stream identifier (everything after the prefix)
    const char *stream_id = path + strlen(prefix);

    // Skip any leading slashes in the ID part
    while (*stream_id == '/') {
        stream_id++;
    }

    // Find query string if present and truncate
    char *stream_id_copy = strdup(stream_id);
    if (!stream_id_copy) {
        log_error("Memory allocation failed for stream ID");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    char *query_start = strchr(stream_id_copy, '?');
    if (query_start) {
        *query_start = '\0'; // Truncate at query string
    }

    // URL-decode the stream identifier
    char decoded_id[256];
    url_decode(stream_id_copy, decoded_id, sizeof(decoded_id));
    free(stream_id_copy);

    log_info("Looking up stream with identifier: %s", decoded_id);

    // First try to find the stream by ID (if it's a number)
    stream_handle_t stream = NULL;

    // try by name
    if (!stream) {
        stream = get_stream_by_name(decoded_id);
        log_debug("Tried to find stream by name '%s', result: %s",
                 decoded_id, stream ? "found" : "not found");
    }

    // If still not found, return error
    if (!stream) {
        log_error("Stream not found: %s", decoded_id);
        create_json_response(response, 404, "{\"error\": \"Stream not found\"}");
        return;
    }

    // Get stream configuration
    stream_config_t config;
    if (get_stream_config(stream, &config) != 0) {
        log_error("Failed to get stream configuration for: %s", decoded_id);
        create_json_response(response, 500, "{\"error\": \"Failed to get stream configuration\"}");
        return;
    }

    // Create JSON response
    char *json = create_stream_json(&config);
    if (!json) {
        log_error("Failed to create stream JSON for: %s", decoded_id);
        create_json_response(response, 500, "{\"error\": \"Failed to create stream JSON\"}");
        return;
    }

    // Create response
    create_json_response(response, 200, json);

    // Free resources
    free(json);

    log_info("Successfully served stream details for: %s", decoded_id);
}

/**
 * Handle POST request to create a new stream with improved error handling
 * and duplicate prevention
 */
void handle_post_stream(const http_request_t *request, http_response_t *response) {
    // No need to copy config settings anymore, using database

    // Ensure we have a request body
    if (!request->body || request->content_length == 0) {
        log_error("Empty request body in stream creation");
        create_json_response(response, 400, "{\"error\": \"Empty request body\"}");
        return;
    }

    // Make a null-terminated copy of the request body
    char *json = malloc(request->content_length + 1);
    if (!json) {
        log_error("Memory allocation failed for request body");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    memcpy(json, request->body, request->content_length);
    json[request->content_length] = '\0';

    // Parse JSON into stream configuration
    stream_config_t config;
    memset(&config, 0, sizeof(stream_config_t)); // Ensure complete initialization

    if (parse_stream_json(json, &config) != 0) {
        free(json);
        log_error("Invalid stream configuration");
        create_json_response(response, 400, "{\"error\": \"Invalid stream configuration\"}");
        return;
    }

    free(json);

    // Validate stream name and URL
    if (config.name[0] == '\0') {
        log_error("Stream name cannot be empty");
        create_json_response(response, 400, "{\"error\": \"Stream name cannot be empty\"}");
        return;
    }

    if (config.url[0] == '\0') {
        log_error("Stream URL cannot be empty");
        create_json_response(response, 400, "{\"error\": \"Stream URL cannot be empty\"}");
        return;
    }

    log_info("Attempting to add stream: name='%s', url='%s'", config.name, config.url);

    // Check if stream already exists - thorough check for duplicates
    stream_handle_t existing_stream = get_stream_by_name(config.name);
    if (existing_stream != NULL) {
        log_warn("Stream with name '%s' already exists", config.name);
        create_json_response(response, 409, "{\"error\": \"Stream with this name already exists\"}");
        return;
    }

    // Check if we've reached the maximum number of streams
    int stream_count = count_stream_configs();
    if (stream_count < 0) {
        log_error("Failed to count stream configurations");
        create_json_response(response, 500, "{\"error\": \"Failed to count stream configurations\"}");
        return;
    }
    
    // Get max streams from global config
    extern config_t global_config;
    int max_streams = global_config.max_streams;
    
    if (stream_count >= max_streams) {
        log_error("Maximum number of streams reached (%d)", max_streams);
        create_json_response(response, 507, "{\"error\": \"Maximum number of streams reached\"}");
        return;
    }

    // Add the stream
    stream_handle_t stream = add_stream(&config);
    if (!stream) {
        log_error("Failed to add stream: %s", config.name);
        create_json_response(response, 500, "{\"error\": \"Failed to add stream\"}");
        return;
    }

    // Also add to database
    uint64_t stream_id = add_stream_config(&config);
    if (stream_id == 0) {
        log_error("Failed to add stream to database: %s", config.name);
        // Continue anyway, the stream is added to memory
    } else {
        log_info("Stream added to database with ID %llu: %s", (unsigned long long)stream_id, config.name);
    }

    log_info("Stream added successfully: %s", config.name);

    // Start the stream if enabled
    if (config.enabled) {
        log_info("Starting stream: %s", config.name);
        if (start_stream(stream) != 0) {
            log_warn("Failed to start stream: %s", config.name);
            // Continue anyway, the stream is added
        } else {
            log_info("Stream started: %s", config.name);

            // Start recording if record flag is set
            if (config.record) {
                log_info("Starting recording for stream: %s", config.name);
                
                // Start HLS streaming
                if (start_hls_stream(config.name) == 0) {
                    log_info("HLS streaming started for stream: %s", config.name);
                } else {
                    log_warn("Failed to start HLS streaming for stream: %s", config.name);
                }
                
                // Also start MP4 recording, regardless of HLS streaming status
                if (start_mp4_recording(config.name) == 0) {
                    log_info("MP4 recording started for stream: %s", config.name);
                } else {
                    log_warn("Failed to start MP4 recording for stream: %s", config.name);
                }
            }
            
            // Start detection if detection-based recording is enabled
            if (config.detection_based_recording && config.detection_model[0] != '\0') {
                log_info("Starting detection for stream: %s with model %s", 
                        config.name, config.detection_model);
                
                if (start_detection_stream_reader(config.name, config.detection_interval) == 0) {
                    log_info("Detection started for stream: %s", config.name);
                } else {
                    log_warn("Failed to start detection for stream: %s", config.name);
                }
            }
        }
    } else {
        log_info("Stream is disabled, not starting: %s", config.name);
    }

    // Create success response
    char response_json[256];
    snprintf(response_json, sizeof(response_json),
             "{\"success\": true, \"name\": \"%s\"}",
             config.name);
    create_json_response(response, 201, response_json);

    log_info("Stream creation completed successfully: %s", config.name);
}

/**
 * Improved handler for updating a stream that correctly handles URL-encoded identifiers
 */
void handle_put_stream(const http_request_t *request, http_response_t *response) {
    // No need to copy config settings anymore, using database

    // Extract stream identifier from the URL
    const char *path = request->path;
    const char *prefix = "/api/streams/";

    // Verify path starts with expected prefix
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        log_error("Invalid request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid request path\"}");
        return;
    }

    // Extract the stream identifier (everything after the prefix)
    const char *stream_id = path + strlen(prefix);

    // Skip any leading slashes in the ID part
    while (*stream_id == '/') {
        stream_id++;
    }

    // Find query string if present and truncate
    char *stream_id_copy = strdup(stream_id);
    if (!stream_id_copy) {
        log_error("Memory allocation failed for stream ID");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    char *query_start = strchr(stream_id_copy, '?');
    if (query_start) {
        *query_start = '\0'; // Truncate at query string
    }

    // URL-decode the stream identifier
    char decoded_id[256];
    url_decode(stream_id_copy, decoded_id, sizeof(decoded_id));
    free(stream_id_copy);

    log_info("Updating stream with identifier: %s", decoded_id);

    // Find the stream by ID or name
    stream_handle_t stream = NULL;

    // try by name
    if (!stream) {
        stream = get_stream_by_name(decoded_id);
        log_debug("Tried to find stream by name '%s', result: %s",
                 decoded_id, stream ? "found" : "not found");
    }

    // If still not found, return error
    if (!stream) {
        log_error("Stream not found: %s", decoded_id);
        create_json_response(response, 404, "{\"error\": \"Stream not found\"}");
        return;
    }

    // Ensure we have a request body
    if (!request->body || request->content_length == 0) {
        log_error("Empty request body for stream update");
        create_json_response(response, 400, "{\"error\": \"Empty request body\"}");
        return;
    }

    // Make a null-terminated copy of the request body
    char *json = malloc(request->content_length + 1);
    if (!json) {
        log_error("Memory allocation failed for request body");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    memcpy(json, request->body, request->content_length);
    json[request->content_length] = '\0';

    // Parse JSON into stream configuration
    stream_config_t config;
    if (parse_stream_json(json, &config) != 0) {
        free(json);
        log_error("Invalid stream configuration in request body");
        create_json_response(response, 400, "{\"error\": \"Invalid stream configuration\"}");
        return;
    }

    free(json);

    // Get current stream config to check name
    stream_config_t current_config;
    if (get_stream_config(stream, &current_config) != 0) {
        log_error("Failed to get current stream configuration");
        create_json_response(response, 500, "{\"error\": \"Failed to get current stream configuration\"}");
        return;
    }

    // Special handling for name changes - log both names for clarity
    if (strcmp(current_config.name, config.name) != 0) {
        log_info("Stream name change detected: '%s' -> '%s'", current_config.name, config.name);
    }

    // Get current stream status
    stream_status_t current_status = get_stream_status(stream);

    // Stop the stream if it's running
    if (current_status == STREAM_STATUS_RUNNING || current_status == STREAM_STATUS_STARTING) {
        log_info("Stopping stream before update: %s", current_config.name);
        if (stop_stream(stream) != 0) {
            log_warn("Failed to stop stream: %s", current_config.name);
            // Continue anyway, we'll try to update
        }
    }

    // Update the stream configuration in database
    log_info("Updating stream configuration in database for: %s", current_config.name);
    if (update_stream_config(current_config.name, &config) != 0) {
        log_error("Failed to update stream configuration in database");
        create_json_response(response, 500, "{\"error\": \"Failed to update stream configuration\"}");
        return;
    }
    
    // Also update the in-memory stream configuration
    log_info("Updating in-memory stream configuration for: %s", current_config.name);
    
    // Log the enabled state for debugging
    log_info("Stream enabled state: %s", config.enabled ? "true" : "false");
    
    // We need to update the in-memory stream configuration
    // Instead of removing and re-adding the stream, let's use the available API functions
    
    // Log the updated configuration
    log_info("Updating in-memory stream configuration: name=%s, enabled=%s", 
             config.name, config.enabled ? "true" : "false");
    
    // First, stop the stream if it's running (we already did this above)
    
    // Verify that the stream was properly updated
    stream_config_t verified_config;
    if (get_stream_config(stream, &verified_config) != 0) {
        log_error("Failed to get verified stream configuration");
        create_json_response(response, 500, "{\"error\": \"Failed to get verified stream configuration\"}");
        return;
    }
    
    // Always perform a full update by removing and re-adding the stream
    log_info("Performing full update by removing and re-adding stream");
    
    // Log changes for debugging
    if (verified_config.enabled != config.enabled) {
        log_info("Enabled flag changed: %s -> %s", 
                verified_config.enabled ? "true" : "false", 
                config.enabled ? "true" : "false");
    }
    
    if (strcmp(verified_config.codec, config.codec) != 0) {
        log_info("Codec changed: %s -> %s", verified_config.codec, config.codec);
    }
    
    if (verified_config.width != config.width || verified_config.height != config.height) {
        log_info("Resolution changed: %dx%d -> %dx%d", 
                verified_config.width, verified_config.height, 
                config.width, config.height);
    }
    
    if (verified_config.fps != config.fps) {
        log_info("FPS changed: %d -> %d", verified_config.fps, config.fps);
    }
    
    if (verified_config.priority != config.priority) {
        log_info("Priority changed: %d -> %d", verified_config.priority, config.priority);
    }
    
    if (verified_config.record != config.record) {
        log_info("Record flag changed: %s -> %s", 
                verified_config.record ? "true" : "false", 
                config.record ? "true" : "false");
    }
    
    if (verified_config.streaming_enabled != config.streaming_enabled) {
        log_info("Streaming enabled flag changed: %s -> %s", 
                verified_config.streaming_enabled ? "true" : "false", 
                config.streaming_enabled ? "true" : "false");
    }
        
    // Remove the stream
    if (remove_stream(stream) != 0) {
        log_error("Failed to remove stream for full update: %s", config.name);
        create_json_response(response, 500, "{\"error\": \"Failed to update stream configuration\"}");
        return;
    }
    
    // Add the stream back with the updated configuration
    stream = add_stream(&config);
    if (!stream) {
        log_error("Failed to re-add stream with updated configuration: %s", config.name);
        create_json_response(response, 500, "{\"error\": \"Failed to update stream configuration\"}");
        return;
    }
    
    // Get the updated configuration to verify
    if (get_stream_config(stream, &verified_config) != 0) {
        log_error("Failed to get verified stream configuration after full update");
        create_json_response(response, 500, "{\"error\": \"Failed to verify stream configuration\"}");
        return;
    }
    
    log_info("Stream configuration fully updated");
    
    // Log the verified configuration
    log_info("Verified stream configuration: name=%s, enabled=%s, streaming_enabled=%s, codec=%s", 
             verified_config.name, 
             verified_config.enabled ? "true" : "false", 
             verified_config.streaming_enabled ? "true" : "false",
             verified_config.codec);
    
    // Check if detection settings have changed
    bool detection_settings_changed = false;
    
    if (verified_config.detection_based_recording != config.detection_based_recording) {
        log_info("Detection enabled state changed: %s -> %s", 
                verified_config.detection_based_recording ? "true" : "false", 
                config.detection_based_recording ? "true" : "false");
        detection_settings_changed = true;
    }
    
    if (strcmp(verified_config.detection_model, config.detection_model) != 0) {
        log_info("Detection model changed: %s -> %s", 
                verified_config.detection_model, config.detection_model);
        detection_settings_changed = true;
    }
    
    if (verified_config.detection_threshold != config.detection_threshold) {
        log_info("Detection threshold changed: %.2f -> %.2f", 
                verified_config.detection_threshold, config.detection_threshold);
        detection_settings_changed = true;
    }
    
    if (verified_config.detection_interval != config.detection_interval) {
        log_info("Detection interval changed: %d -> %d", 
                verified_config.detection_interval, config.detection_interval);
        detection_settings_changed = true;
    }
    
    // Update detection parameters in memory
    if (config.detection_based_recording) {
        log_info("Updating detection parameters: threshold=%.2f, interval=%d", 
                config.detection_threshold, config.detection_interval);
        
        set_stream_detection_recording(stream, true, config.detection_model);
        set_stream_detection_params(stream, config.detection_interval, 
                                   config.detection_threshold, 
                                   config.pre_detection_buffer, 
                                   config.post_detection_buffer);
    } else {
        // Disable detection-based recording
        set_stream_detection_recording(stream, false, NULL);
    }
    
    // Reload detection if settings changed
    if (detection_settings_changed) {
        log_info("Detection settings changed, reloading detection for stream %s", config.name);
        
        // First stop any existing detection stream
        if (is_detection_stream_reader_running(config.name)) {
            log_info("Stopping existing detection stream for %s", config.name);
            stop_detection_stream_reader(config.name);
        }
        
        // Start detection if enabled
        if (config.detection_based_recording) {
            log_info("Starting detection stream for %s with model %s", 
                    config.name, config.detection_model);
            start_detection_stream_reader(config.name, config.detection_interval);
        }
    }

    // Start the stream if enabled
    if (config.enabled) {
        log_info("Stream is enabled, starting it: %s", config.name);
        if (start_stream(stream) != 0) {
            log_warn("Failed to start stream: %s", config.name);
            // Continue anyway, the configuration is updated
        } else {
            // Start recording if record flag is set
            if (config.record) {
                log_info("Starting recording for stream: %s", config.name);
                
                // Start HLS streaming
                if (start_hls_stream(config.name) == 0) {
                    log_info("HLS streaming started for stream: %s", config.name);
                } else {
                    log_warn("Failed to start HLS streaming for stream: %s", config.name);
                }
                
                // Also start MP4 recording, regardless of HLS streaming status
                if (start_mp4_recording(config.name) == 0) {
                    log_info("MP4 recording started for stream: %s", config.name);
                } else {
                    log_warn("Failed to start MP4 recording for stream: %s", config.name);
                }
            }
        }
    } else {
        log_info("Stream is disabled, not starting it: %s", config.name);
    }

    // Stream configuration is updated in the database by update_stream_config

    // Create success response
    char response_json[256];
    snprintf(response_json, sizeof(response_json),
             "{\"success\": true, \"name\": \"%s\", \"id\": \"%s\"}",
             config.name, decoded_id);
    create_json_response(response, 200, response_json);

    log_info("Stream updated successfully: %s", config.name);
}

/**
 * Improved handler for deleting a stream that correctly handles URL-encoded identifiers
 */
void handle_delete_stream(const http_request_t *request, http_response_t *response) {
    // No need to copy config settings anymore, using database

    // Extract stream identifier from the URL
    const char *path = request->path;
    const char *prefix = "/api/streams/";

    // Verify path starts with expected prefix
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        log_error("Invalid request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid request path\"}");
        return;
    }

    // Extract the stream identifier (everything after the prefix)
    const char *stream_id = path + strlen(prefix);

    // Skip any leading slashes in the ID part
    while (*stream_id == '/') {
        stream_id++;
    }

    // Find query string if present and truncate
    char *stream_id_copy = strdup(stream_id);
    if (!stream_id_copy) {
        log_error("Memory allocation failed for stream ID");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }

    char *query_start = strchr(stream_id_copy, '?');
    if (query_start) {
        *query_start = '\0'; // Truncate at query string
    }

    // URL-decode the stream identifier
    char decoded_id[256];
    url_decode(stream_id_copy, decoded_id, sizeof(decoded_id));
    free(stream_id_copy);

    log_info("Deleting stream with identifier: %s", decoded_id);

    // Find the stream by ID or name
    stream_handle_t stream = NULL;

    // try by name
    if (!stream) {
        stream = get_stream_by_name(decoded_id);
        log_debug("Tried to find stream by name '%s', result: %s",
                 decoded_id, stream ? "found" : "not found");
    }

    // If still not found, return error
    if (!stream) {
        log_error("Stream not found: %s", decoded_id);
        create_json_response(response, 404, "{\"error\": \"Stream not found\"}");
        return;
    }

    // Get stream name for logging and config updates
    stream_config_t config;
    if (get_stream_config(stream, &config) != 0) {
        log_error("Failed to get stream configuration for: %s", decoded_id);
        create_json_response(response, 500, "{\"error\": \"Failed to get stream configuration\"}");
        return;
    }

    // Save stream name before removal
    char stream_name[MAX_STREAM_NAME];
    strncpy(stream_name, config.name, MAX_STREAM_NAME - 1);
    stream_name[MAX_STREAM_NAME - 1] = '\0';

    log_info("Found stream to delete: %s (name: %s)", decoded_id, stream_name);

    // Stop detection if it's running
    if (is_detection_stream_reader_running(stream_name)) {
        log_info("Stopping detection for stream: %s", stream_name);
        if (stop_detection_stream_reader(stream_name) != 0) {
            log_warn("Failed to stop detection for stream: %s", stream_name);
            // Continue anyway
        } else {
            log_info("Detection stopped for stream: %s", stream_name);
        }
    }

    // Stop and remove the stream
    log_info("Stopping stream: %s", stream_name);
    if (stop_stream(stream) != 0) {
        log_warn("Failed to stop stream: %s", stream_name);
        // Continue anyway, we'll try to remove it
    }

    log_info("Removing stream: %s", stream_name);
    if (remove_stream(stream) != 0) {
        log_error("Failed to remove stream: %s", stream_name);
        create_json_response(response, 500, "{\"error\": \"Failed to remove stream\"}");
        return;
    }

    // Also remove from database
    if (delete_stream_config(stream_name) != 0) {
        log_error("Failed to remove stream from database: %s", stream_name);
        // Continue anyway, the stream is removed from memory
    } else {
        log_info("Stream removed from database: %s", stream_name);
    }

    // Create success response
    char response_json[256];
    snprintf(response_json, sizeof(response_json),
             "{\"success\": true, \"name\": \"%s\", \"id\": \"%s\"}",
             stream_name, decoded_id);
    create_json_response(response, 200, response_json);

    log_info("Stream removed successfully: %s", stream_name);
}

/**
 * Handle POST request to test a stream connection
 */
void handle_test_stream(const http_request_t *request, http_response_t *response) {
    // Ensure we have a request body
    if (!request->body || request->content_length == 0) {
        create_json_response(response, 400, "{\"error\": \"Empty request body\"}");
        return;
    }
    
    // Make a null-terminated copy of the request body
    char *json = malloc(request->content_length + 1);
    if (!json) {
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }
    
    memcpy(json, request->body, request->content_length);
    json[request->content_length] = '\0';
    
    // Parse the URL from the JSON
    char *url = get_json_string_value(json, "url");
    if (!url) {
        free(json);
        create_json_response(response, 400, "{\"error\": \"URL not provided\"}");
        return;
    }
    
    free(json);
    
    // In a real implementation, here we would attempt to connect to the stream and verify it works
    // For now, we'll just simulate a successful connection
    // In a more complete implementation, you'd use libavformat/ffmpeg to test the connection
    
    log_info("Testing stream connection: %s", url);
    
    // Simulate success
    char response_json[512];
    snprintf(response_json, sizeof(response_json), 
             "{\"success\": true, \"url\": \"%s\", \"details\": {\"codec\": \"h264\", \"width\": 1280, \"height\": 720, \"fps\": 30}}", 
             url);
    
    create_json_response(response, 200, response_json);
    
    free(url);
}

/**
 * Create a JSON string for stream configuration
 */
static char* create_stream_json(const stream_config_t *stream) {
    if (!stream) return NULL;
    
    // Allocate more space for the JSON to accommodate detection settings
    char *json = malloc(2048);
    if (!json) return NULL;
    
    // Start with the basic stream configuration
    int pos = snprintf(json, 2048,
             "{"
             "\"name\": \"%s\","
             "\"url\": \"%s\","
             "\"enabled\": %s,"
             "\"streaming_enabled\": %s,"
             "\"width\": %d,"
             "\"height\": %d,"
             "\"fps\": %d,"
             "\"codec\": \"%s\","
             "\"priority\": %d,"
             "\"record\": %s,"
             "\"segment_duration\": %d,"
             "\"status\": \"%s\"",
             stream->name,
             stream->url,
             stream->enabled ? "true" : "false",
             stream->streaming_enabled ? "true" : "false",
             stream->width,
             stream->height,
             stream->fps,
             stream->codec,
             stream->priority,
             stream->record ? "true" : "false",
             stream->segment_duration,
             "Running"); // In a real implementation, we would get the actual status
    
    // Add detection-based recording settings
    pos += snprintf(json + pos, 2048 - pos,
             ","
             "\"detection_based_recording\": %s",
             stream->detection_based_recording ? "true" : "false");
    
    // Always include detection model and parameters, even when detection-based recording is disabled
    // This ensures the frontend always has access to these settings
    
    // Convert threshold from 0.0-1.0 to percentage (0-100)
    int threshold_percent = (int)(stream->detection_threshold * 100.0f);
    
    pos += snprintf(json + pos, 2048 - pos,
             ","
             "\"detection_model\": \"%s\","
             "\"detection_threshold\": %d,"
             "\"detection_interval\": %d,"
             "\"pre_detection_buffer\": %d,"
             "\"post_detection_buffer\": %d,"
             "\"protocol\": %d",
             stream->detection_model,
             threshold_percent,
             stream->detection_interval,
             stream->pre_detection_buffer,
             stream->post_detection_buffer,
             (int)stream->protocol);
    
    // Close the JSON object
    pos += snprintf(json + pos, 2048 - pos, "}");
    
    return json;
}

/**
 * Create a JSON array of all streams
 */
static char* create_streams_json_array() {
    // Get all stream configurations from database
    stream_config_t db_streams[MAX_STREAMS];
    int count = get_all_stream_configs(db_streams, MAX_STREAMS);
    
    if (count <= 0) {
        // Return empty array
        char *json = malloc(32);
        if (!json) return NULL;
        strcpy(json, "[]");
        return json;
    }
    
    // Allocate buffer for JSON array with estimated size
    char *json = malloc(1024 * count + 32);
    if (!json) return NULL;
    
    strcpy(json, "[");
    int pos = 1;
    
    // Iterate through all streams
    for (int i = 0; i < count; i++) {
        // Add comma if not first element
        if (pos > 1) {
            json[pos++] = ',';
        }
        
        // Create stream JSON
        char *stream_json = create_stream_json(&db_streams[i]);
        if (!stream_json) continue;
        
        // Append to array
        strcpy(json + pos, stream_json);
        pos += strlen(stream_json);
        
        free(stream_json);
    }
    
    // Close array
    json[pos++] = ']';
    json[pos] = '\0';
    
    return json;
}

/**
 * Handle POST request to toggle streaming for a stream
 */
void handle_toggle_streaming(const http_request_t *request, http_response_t *response) {
    // Extract stream identifier from the URL
    // URL format: /api/streams/{stream_name}/toggle_streaming
    const char *path = request->path;
    const char *prefix = "/api/streams/";
    const char *suffix = "/toggle_streaming";
    
    // Verify path starts with expected prefix
    if (strncmp(path, prefix, strlen(prefix)) != 0) {
        log_error("Invalid request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid request path\"}");
        return;
    }
    
    // Find the suffix position
    const char *suffix_pos = strstr(path + strlen(prefix), suffix);
    if (!suffix_pos) {
        log_error("Invalid toggle_streaming request path: %s", path);
        create_json_response(response, 400, "{\"error\": \"Invalid toggle_streaming request path\"}");
        return;
    }
    
    // Extract the stream name between prefix and suffix
    size_t name_len = suffix_pos - (path + strlen(prefix));
    if (name_len >= MAX_STREAM_NAME) {
        log_error("Stream name too long in toggle_streaming request");
        create_json_response(response, 400, "{\"error\": \"Stream name too long\"}");
        return;
    }
    
    char stream_name[MAX_STREAM_NAME];
    memcpy(stream_name, path + strlen(prefix), name_len);
    stream_name[name_len] = '\0';
    
    // URL-decode the stream name
    char decoded_name[MAX_STREAM_NAME];
    url_decode(stream_name, decoded_name, sizeof(decoded_name));
    
    log_info("Toggle streaming request for stream: %s", decoded_name);
    
    // Find the stream by name
    stream_handle_t stream = get_stream_by_name(decoded_name);
    if (!stream) {
        log_error("Stream not found: %s", decoded_name);
        create_json_response(response, 404, "{\"error\": \"Stream not found\"}");
        return;
    }
    
    // Parse request body to get enabled flag
    if (!request->body || request->content_length == 0) {
        log_error("Empty request body in toggle_streaming request");
        create_json_response(response, 400, "{\"error\": \"Empty request body\"}");
        return;
    }
    
    // Make a null-terminated copy of the request body
    char *json = malloc(request->content_length + 1);
    if (!json) {
        log_error("Memory allocation failed for request body");
        create_json_response(response, 500, "{\"error\": \"Memory allocation failed\"}");
        return;
    }
    
    memcpy(json, request->body, request->content_length);
    json[request->content_length] = '\0';
    
    // Parse the enabled flag from the JSON
    bool enabled = get_json_boolean_value(json, "enabled", true);
    free(json);
    
    log_info("Toggle streaming for stream %s: enabled=%s", decoded_name, enabled ? "true" : "false");
    
    // Get current stream configuration to preserve recording state
    stream_config_t config;
    if (get_stream_config(stream, &config) != 0) {
        log_error("Failed to get stream configuration for: %s", decoded_name);
        create_json_response(response, 500, "{\"error\": \"Failed to get stream configuration\"}");
        return;
    }
    
    // Store the current recording state
    bool recording_enabled = config.record;
    log_info("Current recording state for stream %s: %s", 
             decoded_name, recording_enabled ? "enabled" : "disabled");
    
    // Set the streaming_enabled flag in the stream configuration
    if (set_stream_streaming_enabled(stream, enabled) != 0) {
        log_error("Failed to set streaming_enabled flag for stream: %s", decoded_name);
        create_json_response(response, 500, "{\"error\": \"Failed to set streaming_enabled flag\"}");
        return;
    }
    
    // Update only the streaming_enabled flag in the config
    config.streaming_enabled = enabled;
    
    // Update the stream configuration in database
    log_info("Updating streaming_enabled flag in database for: %s", decoded_name);
    if (update_stream_config(decoded_name, &config) != 0) {
        log_error("Failed to update stream configuration in database");
        create_json_response(response, 500, "{\"error\": \"Failed to update stream configuration in database\"}");
        return;
    }
    
    // Check if the stream is in the process of stopping
    stream_state_manager_t *state = get_stream_state_by_name(decoded_name);
    
    // Toggle the streaming
    if (enabled) {
        // Check if the stream is in the process of stopping
        if (state && is_stream_state_stopping(state)) {
            log_warn("Cannot start HLS stream %s while it is in the process of being stopped", decoded_name);
            create_json_response(response, 503, "{\"error\": \"Stream is in the process of stopping, please try again later\"}");
            return;
        }
        
        // Start HLS stream if not already running
        if (start_hls_stream(decoded_name) != 0) {
            log_error("Failed to start HLS stream for: %s", decoded_name);
            create_json_response(response, 500, "{\"error\": \"Failed to start HLS stream\"}");
            return;
        }
        log_info("Started HLS stream for %s", decoded_name);
    } else {
        // Stop HLS stream if running
        if (stop_hls_stream(decoded_name) != 0) {
            log_error("Failed to stop HLS stream for: %s", decoded_name);
            create_json_response(response, 500, "{\"error\": \"Failed to stop HLS stream\"}");
            return;
        }
        log_info("Stopped HLS stream for %s", decoded_name);
        
        // If the stream is in the process of stopping, wait for it to complete
        if (state && is_stream_state_stopping(state)) {
            log_info("Waiting for stream %s to complete stopping process", decoded_name);
            wait_for_stream_stop(state, 5000); // Wait up to 5 seconds
        }
    }
    
    // Always check recording settings regardless of streaming state
    // This ensures recording is active based on the stream's record flag
    if (recording_enabled) {
        log_info("Ensuring recording is active for stream %s based on record flag", decoded_name);
        
        // Check current recording state
        int recording_state = get_recording_state(decoded_name);
        
        if (recording_state == 0) {
            // Recording is not active, start it
            log_info("Starting recording for stream %s", decoded_name);
            if (start_mp4_recording(decoded_name) != 0) {
                log_warn("Failed to start recording for stream %s", decoded_name);
                // Continue anyway, this is not a fatal error
            } else {
                log_info("Successfully started recording for stream %s", decoded_name);
            }
        } else if (recording_state == 1) {
            log_info("Recording is already active for stream %s", decoded_name);
        } else {
            log_warn("Could not determine recording state for stream %s", decoded_name);
        }
    } else {
        log_info("Recording is not enabled for stream %s", decoded_name);
    }
    
    // Create success response
    char response_json[256];
    snprintf(response_json, sizeof(response_json),
             "{\"success\": true, \"name\": \"%s\", \"streaming_enabled\": %s, \"recording_enabled\": %s}",
             decoded_name, enabled ? "true" : "false", recording_enabled ? "true" : "false");
    
    create_json_response(response, 200, response_json);
    
    log_info("Successfully toggled streaming for stream: %s (streaming=%s, recording=%s)", 
             decoded_name, enabled ? "enabled" : "disabled", recording_enabled ? "enabled" : "disabled");
}

/**
 * Parse JSON into stream configuration
 */
static int parse_stream_json(const char *json, stream_config_t *stream) {
    if (!json || !stream) return -1;

    memset(stream, 0, sizeof(stream_config_t));

    // Parse JSON to extract stream configuration
    char *name = get_json_string_value(json, "name");
    if (!name) return -1;

    char *url = get_json_string_value(json, "url");
    if (!url) {
        free(name);
        return -1;
    }

    strncpy(stream->name, name, MAX_STREAM_NAME - 1);
    stream->name[MAX_STREAM_NAME - 1] = '\0';

    strncpy(stream->url, url, MAX_URL_LENGTH - 1);
    stream->url[MAX_URL_LENGTH - 1] = '\0';

    free(name);
    free(url);

    stream->enabled = get_json_boolean_value(json, "enabled", true);
    stream->streaming_enabled = get_json_boolean_value(json, "streaming_enabled", true);
    stream->width = get_json_integer_value(json, "width", 1280);
    stream->height = get_json_integer_value(json, "height", 720);
    stream->fps = get_json_integer_value(json, "fps", 15);

    char *codec = get_json_string_value(json, "codec");
    if (codec) {
        // Ensure we don't overflow the codec buffer (which is 16 bytes)
        size_t codec_size = sizeof(stream->codec);
        strncpy(stream->codec, codec, codec_size - 1);
        stream->codec[codec_size - 1] = '\0';
        free(codec);
    } else {
        // Use safe string copy for default codec
        strncpy(stream->codec, "h264", sizeof(stream->codec) - 1);
        stream->codec[sizeof(stream->codec) - 1] = '\0';
    }

    stream->priority = get_json_integer_value(json, "priority", 5);
    stream->record = get_json_boolean_value(json, "record", true);
    stream->segment_duration = get_json_integer_value(json, "segment_duration", 900);

    // Parse detection-based recording options
    stream->detection_based_recording = get_json_boolean_value(json, "detection_based_recording", false);
    
    // Always parse detection options, even if detection-based recording is disabled
    // This ensures we preserve the detection settings even when detection is temporarily disabled
    char *detection_model = get_json_string_value(json, "detection_model");
    if (detection_model) {
        strncpy(stream->detection_model, detection_model, MAX_PATH_LENGTH - 1);
        stream->detection_model[MAX_PATH_LENGTH - 1] = '\0';
        free(detection_model);
    }
    
    // Parse detection threshold (convert from percentage to 0.0-1.0 range)
    if (get_json_has_key(json, "detection_threshold")) {
        int threshold_percent = get_json_integer_value(json, "detection_threshold", 50);
        stream->detection_threshold = (float)threshold_percent / 100.0f;
        log_info("Setting stream detection threshold to %.2f (from %d%%)", 
                stream->detection_threshold, threshold_percent);
    }
    
    // Parse detection interval
    if (get_json_has_key(json, "detection_interval")) {
        stream->detection_interval = get_json_integer_value(json, "detection_interval", 10);
    }
    
    // Parse pre/post detection buffers
    if (get_json_has_key(json, "pre_detection_buffer")) {
        stream->pre_detection_buffer = get_json_integer_value(json, "pre_detection_buffer", 5);
    }
    
    if (get_json_has_key(json, "post_detection_buffer")) {
        stream->post_detection_buffer = get_json_integer_value(json, "post_detection_buffer", 10);
    }
    
    // Parse protocol (TCP or UDP)
    if (get_json_has_key(json, "protocol")) {
        stream->protocol = (stream_protocol_t)get_json_integer_value(json, "protocol", 0);
        log_info("Stream protocol set to: %s", stream->protocol == STREAM_PROTOCOL_TCP ? "TCP" : "UDP");
    } else {
        stream->protocol = STREAM_PROTOCOL_TCP; // Default to TCP
    }
    
    log_info("Stream config parsed: name=%s, enabled=%s, streaming_enabled=%s, detection=%s", 
            stream->name, 
            stream->enabled ? "true" : "false",
            stream->streaming_enabled ? "true" : "false",
            stream->detection_based_recording ? "true" : "false");
    
    if (detection_model || get_json_has_key(json, "detection_threshold")) {
        log_info("Detection options parsed: model=%s, threshold=%.2f, interval=%d, pre_buffer=%d, post_buffer=%d",
                stream->detection_model, stream->detection_threshold, stream->detection_interval,
                stream->pre_detection_buffer, stream->post_detection_buffer);
    }

    return 0;
}
