#pragma once

#include <Arduino.h>

#include <vector>

struct AzureSasUrlParts {
    String base;
    String query;
    bool https = true;
};

// Parse a container SAS URL of the form: https://<account>.blob.core.windows.net/<container>?<sas>
bool azure_blob_parse_sas_url(const char *url, AzureSasUrlParts &out);

// Build a blob URL inside the container using URL-encoding (keeps '/' intact).
String azure_blob_build_blob_url(const AzureSasUrlParts &sas, const String &blob_name);

// List a single page of blobs under a prefix (never list the entire container).
// Returns names (may include non-.g4) and an optional continuation marker.
bool azure_blob_list_page(
    const AzureSasUrlParts &sas,
    const String &prefix,
    const String &marker,
    uint16_t max_results,
    std::vector<String> &out_names,
    String &out_next_marker,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
);

// Download a blob into a heap buffer. Caller owns the returned buffer (heap_caps_free).
bool azure_blob_download_to_buffer(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint8_t **out_buf,
    size_t *out_size,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
);

// Like azure_blob_download_to_buffer but also returns the last HTTP status code (0 if begin() failed).
bool azure_blob_download_to_buffer_ex(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint8_t **out_buf,
    size_t *out_size,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms,
    int *out_http_code
);

// Delete a blob. Returns true when the server accepted the delete.
bool azure_blob_delete(
    const AzureSasUrlParts &sas,
    const String &blob_name,
    uint32_t timeout_ms,
    uint8_t retries,
    uint32_t retry_delay_ms
);
