#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <stddef.h>
#include <stdint.h>

/* Deletions remembered within one version, so later commands in the same
 * version can map positions that fell inside a deleted range. */
#define MAX_DELETIONS_IN_VERSION 10

/* A piece of the document's text. The document is a linked list of chunks. */
typedef struct chunk {
    char *text;
    size_t len;
    struct chunk *next;
} chunk;

/* One deletion made in the current version, in that version's positions. */
typedef struct {
    size_t original_start;
    size_t original_len;
} deletion_record_for_version_t;

typedef struct document {
    chunk *head;                       /* live text, edited in place */
    size_t total_len;                  /* length of the live text */
    uint64_t version;                  /* bumped by markdown_increment_version() */
    char *committed_content_cache;     /* text as of the last version; what readers see */

    deletion_record_for_version_t active_deletions[MAX_DELETIONS_IN_VERSION];
    int num_active_deletions;
} document;

#endif /* DOCUMENT_H */
