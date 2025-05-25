#ifndef MARKDOWN_H
#define MARKDOWN_H
#include <stdio.h>
#include <stdint.h>
#include "document.h"  
/**
 * The given file contains all the functions you will be required to complete. You are free to and encouraged to create
 * more helper functions to help assist you when creating the document. For the automated marking you can expect unit tests
 * for the following tests, verifying if the document functionalities are correctly implemented. All the commands are explained 
 * in detail in the assignment spec.
 */

// Return -1 if the cursor position is invalid
void free_chunk_node(chunk *node);
char get_char_at(document *doc, size_t pos);

// Initialize and free a document
document * markdown_init(void);
void markdown_free(document *doc);

// === Edit Commands ===
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content);
int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len);

// === Formatting Commands ===
int markdown_newline(document *doc, uint64_t version, size_t pos);
int markdown_heading(document *doc, uint64_t version, size_t level, size_t pos);
int markdown_bold(document *doc, uint64_t version, size_t start, size_t end);
int markdown_italic(document *doc, uint64_t version, size_t start, size_t end);
int markdown_blockquote(document *doc, uint64_t version, size_t pos);
int markdown_ordered_list(document *doc, uint64_t version, size_t pos);
int markdown_unordered_list(document *doc, uint64_t version, size_t pos);
int markdown_code(document *doc, uint64_t version, size_t start, size_t end);
int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos);
int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url);

// === Utilities ===
void markdown_print(const document *doc, FILE *stream);
char *markdown_flatten(const document *doc);

// === Versioning ===
void markdown_increment_version(document *doc);
#endif // MARKDOWN_H