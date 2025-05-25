// In your document.h file
#ifndef DOCUMENT_H
#define DOCUMENT_H

#include <stddef.h> // For size_t
#include <stdint.h> // For uint64_t

// Define MAX_DELETIONS_IN_VERSION before its use in document struct
#define MAX_DELETIONS_IN_VERSION 10 // Example, adjust as needed

typedef struct chunk { // Assuming this is your chunk definition
    char *text;
    size_t len;
    struct chunk *next;
    // If you have 'prev' for a doubly linked list, include it here
    // struct chunk *prev; 
} chunk;

// Define this new type for tracking deletions
typedef struct {
    size_t original_start;
    size_t original_len;
} deletion_record_for_version_t;

typedef struct document { // Ensure this is your actual document struct name
    chunk *head;
    // chunk *tail; // If you use a tail pointer for your chunk list
    size_t total_len;
    uint64_t version;
    char *committed_content_cache; // For deferred commit

    // New members for tracking deletions within the current version
    deletion_record_for_version_t active_deletions[MAX_DELETIONS_IN_VERSION];
    int num_active_deletions;
} document; 

// Any other function prototypes or definitions in your document.h
#endif // DOCUMENT_H