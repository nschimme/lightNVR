#ifndef LIGHTNVR_DB_STREAMS_H
#define LIGHTNVR_DB_STREAMS_H

#include <stdint.h>
#include "core/config.h"

/**
 * Add a stream configuration to the database
 *
 * @param stream Stream configuration to add
 * @return Stream ID on success, 0 on failure
 */
uint64_t add_stream_config(const stream_config_t *stream);

/**
 * Update a stream configuration in the database
 *
 * @param name Stream name to update
 * @param stream Updated stream configuration
 * @return 0 on success, non-zero on failure
 */
int update_stream_config(const char *name, const stream_config_t *stream);

/**
 * Delete a stream configuration from the database (soft delete by disabling)
 *
 * @param name Stream name to delete
 * @return 0 on success, non-zero on failure
 */
int delete_stream_config(const char *name);

/**
 * Delete a stream configuration from the database with option for permanent deletion
 *
 * @param name Stream name to delete
 * @param permanent If true, permanently delete the stream; if false, just disable it
 * @return 0 on success, non-zero on failure
 */
int delete_stream_config_internal(const char *name, bool permanent);

/**
 * Get a stream configuration from the database
 *
 * @param name Stream name to get
 * @param stream Stream configuration to fill
 * @return 0 on success, non-zero on failure
 */
int get_stream_config_by_name(const char *name, stream_config_t *stream);

/**
 * Get all stream configurations from the database
 *
 * @param streams Array to fill with stream configurations
 * @param max_count Maximum number of streams to return
 * @return Number of streams found, or -1 on error
 */
int get_all_stream_configs(stream_config_t *streams, int max_count);

/**
 * Count the number of stream configurations in the database
 *
 * @return Number of streams, or -1 on error
 */
int count_stream_configs(void);

/**
 * Count the number of enabled stream configurations in the database
 *
 * @return Number of enabled streams, or -1 on error
 */
int get_enabled_stream_count(void);

/**
 * Check if a stream is eligible for live streaming
 *
 * @param stream_name Name of the stream to check
 * @return 1 if eligible, 0 if not eligible, -1 on error
 */
int is_stream_eligible_for_live_streaming(const char *stream_name);

#endif // LIGHTNVR_DB_STREAMS_H
