#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h> 
#include <ctype.h>

#include "markdown.h"

// Return codes
#ifndef SUCCESS
#define SUCCESS 0
#endif
#ifndef INVALID_CURSOR_POS
#define INVALID_CURSOR_POS -1
#endif
#ifndef DELETED_POSITION 
#define DELETED_POSITION -2 
#endif
#ifndef OUTDATED_VERSION
#define OUTDATED_VERSION -3
#endif

// === Forward Declarations for Static Helper Functions ===
static chunk *create_chunk(const char *text, size_t len); 
static int is_start_of_line_or_doc(document *doc, size_t pos);
static int internal_insert_string(document *doc, size_t original_pos, const char *str_to_insert, int perform_deletion_adjustment);
static char *flatten_live_content(const chunk *head_node, size_t list_total_len);
static size_t adjust_pos_for_deletions(document *doc, size_t original_pos);
static int get_last_ol_number_ending_before(document *doc, size_t position_before);
static char get_char_at_abs_pos(document *doc, size_t abs_pos);


// === Helper Functions ===
static chunk *create_chunk(const char *text, size_t len) {
    chunk *new_chunk = (chunk *)malloc(sizeof(chunk));
    if (!new_chunk) {
        perror("Failed to allocate chunk");
        return NULL;
    }
    new_chunk->text = (char *)malloc(len + 1);
    if (!new_chunk->text) {
        perror("Failed to allocate text in chunk");
        free(new_chunk);
        return NULL;
    }
    if (text && len > 0) {
        memcpy(new_chunk->text, text, len);
    }
    new_chunk->text[len] = '\0';
    new_chunk->len = len;
    new_chunk->next = NULL;
    return new_chunk;
}

// === Init and Free ===
document *markdown_init(void) {
    document *doc = (document *)malloc(sizeof(document));
    if (!doc) {
        perror("Failed to allocate document");
        return NULL;
    }
    doc->head = NULL; 
    doc->total_len = 0;
    doc->version = 0; 

    doc->committed_content_cache = strdup("");
    if (!doc->committed_content_cache) {
        perror("Failed to allocate for initial committed_content_cache");
        free(doc);
        return NULL;
    }
    doc->num_active_deletions = 0; 
    return doc;
}

void markdown_free(document *doc) {
    if (!doc) {
        return;
    }
    chunk *current = doc->head;
    chunk *next_chunk;
    while (current != NULL) {
        next_chunk = current->next;
        if (current->text) {
            free(current->text);
        }
        free(current);
        current = next_chunk;
    }
    if (doc->committed_content_cache) {
        free(doc->committed_content_cache);
    }
    free(doc);
}

static size_t adjust_pos_for_deletions(document *doc, size_t original_pos) {
    size_t adjusted_pos = original_pos;
    for (int i = 0; i < doc->num_active_deletions; ++i) {
        deletion_record_for_version_t *del = &doc->active_deletions[i];
        if (original_pos >= del->original_start && original_pos < del->original_start + del->original_len) {
            adjusted_pos = del->original_start; 
            break; 
        }
    }
    return adjusted_pos;
}


// === Edit Commands ===
int markdown_insert(document *doc, uint64_t version, size_t pos, const char *content) {
    if (!doc || !content) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) { 
        return OUTDATED_VERSION;
    }
    
    size_t adjusted_pos = adjust_pos_for_deletions(doc, pos);

    if (adjusted_pos > doc->total_len) { 
        return INVALID_CURSOR_POS; 
    }

    size_t content_len = strlen(content);
    if (content_len == 0) {
        return SUCCESS; 
    }
    if (strchr(content, '\n') != NULL) {
        return INVALID_CURSOR_POS; 
    }
    
    return internal_insert_string(doc, adjusted_pos, content, 0); 
}

int markdown_delete(document *doc, uint64_t version, size_t pos, size_t len) {
    if (!doc) {
        return INVALID_CURSOR_POS;
    }
    if (version != doc->version) { 
        return OUTDATED_VERSION;
    }
    
    size_t adjusted_pos_for_delete_op_start = adjust_pos_for_deletions(doc, pos);

    if (adjusted_pos_for_delete_op_start > doc->total_len || (adjusted_pos_for_delete_op_start == doc->total_len && len > 0) ) { 
        return INVALID_CURSOR_POS; 
    }
    if (len == 0) {
        return SUCCESS; 
    }

    size_t effective_len = len;
    if (adjusted_pos_for_delete_op_start + effective_len > doc->total_len) {
        effective_len = doc->total_len - adjusted_pos_for_delete_op_start;
    }
    if (effective_len == 0) { 
        return SUCCESS;
    }

    chunk *current = doc->head;
    chunk *prev = NULL;
    size_t current_chunk_abs_start_pos = 0; 
    size_t num_actually_deleted = 0;

    while (current != NULL && num_actually_deleted < effective_len) { 
        size_t current_chunk_abs_end_pos = current_chunk_abs_start_pos + current->len;
        
        size_t current_deletion_op_start_abs = adjusted_pos_for_delete_op_start + num_actually_deleted; 
        size_t current_deletion_op_len_remaining = effective_len - num_actually_deleted;

        size_t overlap_start = (current_deletion_op_start_abs > current_chunk_abs_start_pos) ? current_deletion_op_start_abs : current_chunk_abs_start_pos;
        size_t overlap_end_candidate = current_deletion_op_start_abs + current_deletion_op_len_remaining;
        size_t overlap_end = (overlap_end_candidate < current_chunk_abs_end_pos) ? overlap_end_candidate : current_chunk_abs_end_pos;

        if (overlap_start < overlap_end) { 
            size_t offset_in_chunk = overlap_start - current_chunk_abs_start_pos;
            size_t num_to_delete_in_this_chunk_iteration = overlap_end - overlap_start;

            if (offset_in_chunk == 0 && num_to_delete_in_this_chunk_iteration == current->len) { 
                chunk *to_delete = current;
                if (prev) {
                    prev->next = current->next;
                } else {
                    doc->head = current->next;
                }
                current = current->next; 
                if(to_delete->text) free(to_delete->text);
                free(to_delete);
            } else { 
                if(current->text) { 
                    memmove(current->text + offset_in_chunk,
                            current->text + offset_in_chunk + num_to_delete_in_this_chunk_iteration,
                            current->len - (offset_in_chunk + num_to_delete_in_this_chunk_iteration));
                    current->text[current->len - num_to_delete_in_this_chunk_iteration] = '\0'; 
                }
                current->len -= num_to_delete_in_this_chunk_iteration;
                
                prev = current; 
                current_chunk_abs_start_pos += current->len; 
                current = current->next;
            }
            doc->total_len -= num_to_delete_in_this_chunk_iteration;
            num_actually_deleted += num_to_delete_in_this_chunk_iteration;
        } else { 
            prev = current;
            current_chunk_abs_start_pos += current->len; 
            current = current->next;
        }
    }
    
    // Remember the deletion so later commands in this version can map positions inside it
    if (doc->num_active_deletions < MAX_DELETIONS_IN_VERSION) {
        doc->active_deletions[doc->num_active_deletions].original_start = pos; 
        doc->active_deletions[doc->num_active_deletions].original_len = len;   
        doc->num_active_deletions++;
    }
    return SUCCESS;
}


// === Formatting Commands ===

int markdown_newline(document *doc, uint64_t version, size_t pos) {
    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION; 
    
    size_t adjusted_pos = adjust_pos_for_deletions(doc, pos);
    if (adjusted_pos > doc->total_len) return INVALID_CURSOR_POS;
    
    const char newline_char_str[] = "\n"; 
    return internal_insert_string(doc, adjusted_pos, newline_char_str, 0); 
}

static int is_start_of_line_or_doc(document *doc, size_t pos) {
    if (pos == 0) return 1; 
    if (!doc || pos > doc->total_len) return -1; 

    if (pos > 0 && pos <= doc->total_len) { 
        char char_before = get_char_at_abs_pos(doc, pos - 1);
        if (char_before == '\n') {
            return 1;
        }
    }
    return 0; 
}

static char get_char_at_abs_pos(document *doc, size_t abs_pos) {
    if (!doc || abs_pos >= doc->total_len) { 
        return '\0'; 
    }
    chunk *c = doc->head;
    size_t current_chunk_start = 0;
    while (c) {
        if (abs_pos >= current_chunk_start && abs_pos < current_chunk_start + c->len) {
            return c->text[abs_pos - current_chunk_start];
        }
        current_chunk_start += c->len;
        c = c->next;
    }
    return '\0'; 
}

// Number of the ordered-list item on the line ending just before `position_before`, or 0
static int get_last_ol_number_ending_before(document *doc, size_t position_before) {
    if (position_before == 0) return 0;

    size_t p = position_before - 1; // Index of char just before where new item would start

    // Step 1: Find the end of the relevant line (skip any trailing newlines at p)
    while (p != (size_t)-1 && p < doc->total_len && get_char_at_abs_pos(doc, p) == '\n') {
        if (p == 0) return 0; // All newlines up to the start or empty
        p--;
    }
    if (p == (size_t)-1) return 0; // Only newlines or empty before insertion_pos

    size_t line_content_end_idx = p; // p now points to the last non-newline char of the line to inspect

    // Step 2: Find the start of this line
    size_t line_content_start_idx = line_content_end_idx;
    while (line_content_start_idx > 0) {
        if (get_char_at_abs_pos(doc, line_content_start_idx - 1) == '\n') {
            break; 
        }
        line_content_start_idx--;
    }
    // Now, line_content_start_idx is the beginning of the line content.

    // Step 3: Extract and parse the line
    char line_buf[80]; // Buffer for the line
    int line_buf_idx = 0;
    for (size_t k = line_content_start_idx; k <= line_content_end_idx; ++k) {
        if (line_buf_idx >= (int)sizeof(line_buf) - 1) break; // Prevent buffer overflow
        char ch = get_char_at_abs_pos(doc, k);
        // No need to check for '\0' here if get_char_at_abs_pos handles bounds
        line_buf[line_buf_idx++] = ch;
    }
    line_buf[line_buf_idx] = '\0';

    int num = 0;
    // Try to parse "N. "
    if (sscanf(line_buf, "%d.", &num) == 1) {
        // Check for ". " after the number
        char* dot_ptr = strchr(line_buf, '.');
        if (dot_ptr && *(dot_ptr + 1) == ' ' && num > 0 && num <= 9) {
            return num;
        }
    }
    return 0; // No valid ordered list item found
}


static int internal_insert_string(document *doc, size_t pos, const char *str_to_insert, int perform_deletion_adjustment) {
    size_t insert_pos = pos;
    if (perform_deletion_adjustment) { 
        insert_pos = adjust_pos_for_deletions(doc, pos);
    }
    if (insert_pos > doc->total_len) { 
        return INVALID_CURSOR_POS;
    }

    size_t len_to_insert = strlen(str_to_insert);
    if (len_to_insert == 0) return SUCCESS;

    chunk *new_content_chunk = create_chunk(str_to_insert, len_to_insert); 
    if (!new_content_chunk) return -1; 

    if (insert_pos == 0) {
        new_content_chunk->next = doc->head;
        doc->head = new_content_chunk;
    } else {
        chunk *curr_iter = doc->head; 
        chunk *prev_iter = NULL;    
        size_t current_pos_iter = 0;
        while(curr_iter) {
            if (insert_pos <= current_pos_iter + curr_iter->len) break;
            prev_iter = curr_iter;
            current_pos_iter += curr_iter->len;
            curr_iter = curr_iter->next;
        }

        if (curr_iter != NULL && insert_pos == current_pos_iter) {
            if (prev_iter) prev_iter->next = new_content_chunk;
            else doc->head = new_content_chunk;
            new_content_chunk->next = curr_iter;
        } else if (curr_iter != NULL && insert_pos > current_pos_iter && insert_pos <= current_pos_iter + curr_iter->len) { 
            size_t split_idx = insert_pos - current_pos_iter;
            chunk *suffix_chunk = create_chunk(curr_iter->text + split_idx, curr_iter->len - split_idx); 
            if (!suffix_chunk) { free(new_content_chunk->text); free(new_content_chunk); return -1; }
            
            char *temp_text = (char*)realloc(curr_iter->text, split_idx + 1);
            if(!temp_text && split_idx > 0) { free(suffix_chunk->text); free(suffix_chunk); free(new_content_chunk->text); free(new_content_chunk); return -1; }
            
            if (split_idx == 0) { if(curr_iter->text) free(curr_iter->text); curr_iter->text = create_chunk("", 0)->text; }
            else { curr_iter->text = temp_text; }

            if(curr_iter->text) curr_iter->text[split_idx] = '\0';
            curr_iter->len = split_idx;
            new_content_chunk->next = suffix_chunk;
            suffix_chunk->next = curr_iter->next;
            curr_iter->next = new_content_chunk;
        } else { 
            if(prev_iter && curr_iter == NULL) prev_iter->next = new_content_chunk; 
            else if (curr_iter && insert_pos == current_pos_iter + curr_iter->len) { 
                 new_content_chunk->next = curr_iter->next; curr_iter->next = new_content_chunk;
            }
            else if (!prev_iter && !curr_iter && insert_pos == 0) doc->head = new_content_chunk; 
            else { free(new_content_chunk->text); free(new_content_chunk); return INVALID_CURSOR_POS; }
        }
    }
    doc->total_len += len_to_insert;
    return SUCCESS;
}


int markdown_heading(document *doc, uint64_t version, size_t level, size_t pos) {
    char heading_marker[5]; 
    char final_insert_str[10]; 
    int check_sol;
    size_t adjusted_pos_for_line_check;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    if (level < 1 || level > 3) return -1; 
    
    if (pos > doc->total_len && doc->num_active_deletions == 0) {
         return INVALID_CURSOR_POS;
    }

    snprintf(heading_marker, sizeof(heading_marker), "%.*s ", (int)level, "###"); 
    final_insert_str[0] = '\0';
    
    adjusted_pos_for_line_check = adjust_pos_for_deletions(doc, pos);

    check_sol = is_start_of_line_or_doc(doc, adjusted_pos_for_line_check); 
    if (check_sol == 0) { 
        strcpy(final_insert_str, "\n");
    } else if (check_sol == -1 && adjusted_pos_for_line_check > 0) { 
         if (adjusted_pos_for_line_check > 0) strcpy(final_insert_str, "\n"); 
    }
    strcat(final_insert_str, heading_marker);
    
    return internal_insert_string(doc, pos, final_insert_str, 1); 
}


int markdown_bold(document *doc, uint64_t version, size_t start, size_t end) {
    int res;
    size_t adjusted_start;
    size_t adjusted_end;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    
    adjusted_start = adjust_pos_for_deletions(doc, start);
    adjusted_end = adjust_pos_for_deletions(doc, end);

    if (adjusted_start >= adjusted_end || adjusted_end > doc->total_len) return INVALID_CURSOR_POS; 

    res = internal_insert_string(doc, adjusted_end, "**", 0); 
    if (res != SUCCESS) return res;
    
    res = internal_insert_string(doc, adjusted_start, "**", 0); 
    return res;
}

int markdown_italic(document *doc, uint64_t version, size_t start, size_t end) {
    int res;
    size_t adjusted_start;
    size_t adjusted_end;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    
    adjusted_start = adjust_pos_for_deletions(doc, start);
    adjusted_end = adjust_pos_for_deletions(doc, end);

    if (adjusted_start >= adjusted_end || adjusted_end > doc->total_len) return INVALID_CURSOR_POS; 

    res = internal_insert_string(doc, adjusted_end, "*", 0);
    if (res != SUCCESS) return res;
    res = internal_insert_string(doc, adjusted_start, "*", 0);
    return res;
}

int markdown_blockquote(document *doc, uint64_t version, size_t pos) {
    const char* blockquote_marker; 
    char final_insert_str[10]; 
    int check_sol;
    size_t pos_for_line_check;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->total_len && doc->num_active_deletions == 0) return INVALID_CURSOR_POS; 

    blockquote_marker = "> "; 
    final_insert_str[0] = '\0';
    pos_for_line_check = adjust_pos_for_deletions(doc, pos);

    check_sol = is_start_of_line_or_doc(doc, pos_for_line_check);
    if (check_sol == 0) { 
        strcpy(final_insert_str, "\n");
    } else if (check_sol == -1 && pos_for_line_check > 0) {
        if (pos_for_line_check > 0) strcpy(final_insert_str, "\n");
    }
    strcat(final_insert_str, blockquote_marker);
    
    return internal_insert_string(doc, pos, final_insert_str, 1); 
}

int markdown_ordered_list(document *doc, uint64_t version, size_t pos) {
    char list_marker_str[15]; 
    char final_insert_str[15];
    int check_sol;
    size_t insertion_point; 
    int list_item_number;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;

    insertion_point = adjust_pos_for_deletions(doc, pos);
    // Validate insertion_point against current live document total_len
    // Allow insertion at total_len (end of document) or at 0 for an empty document.
    if (insertion_point > doc->total_len) {
        return INVALID_CURSOR_POS;
    }


    int prev_num = get_last_ol_number_ending_before(doc, insertion_point);
    if (prev_num == 0) { 
        list_item_number = 1;
    } else {
        list_item_number = prev_num + 1;
        if (list_item_number > 9) list_item_number = 1; // Cycle 1-9
    }
    snprintf(list_marker_str, sizeof(list_marker_str), "%d. ", list_item_number);

    final_insert_str[0] = '\0';
    check_sol = is_start_of_line_or_doc(doc, insertion_point); 
    if (check_sol == 0) { 
        strcpy(final_insert_str, "\n");
    } else if (check_sol == -1 && insertion_point > 0) { 
         if (insertion_point > 0) strcpy(final_insert_str, "\n"); 
    }
    strcat(final_insert_str, list_marker_str);
    
    // Insert at the (potentially adjusted) insertion_point. No further adjustment inside internal_insert_string.
    return internal_insert_string(doc, insertion_point, final_insert_str, 0); 
}

int markdown_unordered_list(document *doc, uint64_t version, size_t pos) {
    const char* ulist_marker;
    char final_insert_str[10];
    int check_sol;
    size_t pos_for_line_check; 

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->total_len && doc->num_active_deletions == 0) return INVALID_CURSOR_POS; 

    ulist_marker = "- ";
    final_insert_str[0] = '\0';
    pos_for_line_check = adjust_pos_for_deletions(doc, pos);

    check_sol = is_start_of_line_or_doc(doc, pos_for_line_check);
    if (check_sol == 0) { 
        strcpy(final_insert_str, "\n");
    } else if (check_sol == -1 && pos_for_line_check > 0) {
       if (pos_for_line_check > 0) strcpy(final_insert_str, "\n");
    }
    strcat(final_insert_str, ulist_marker);
    
    return internal_insert_string(doc, pos, final_insert_str, 1); 
}

int markdown_code(document *doc, uint64_t version, size_t start, size_t end) {
    int res;
    size_t adjusted_start;
    size_t adjusted_end;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;

    adjusted_start = adjust_pos_for_deletions(doc, start);
    adjusted_end = adjust_pos_for_deletions(doc, end);

    if (adjusted_start >= adjusted_end || adjusted_end > doc->total_len) return INVALID_CURSOR_POS; 

    res = internal_insert_string(doc, adjusted_end, "`", 0);
    if (res != SUCCESS) return res;
    res = internal_insert_string(doc, adjusted_start, "`", 0);
    return res;
}

int markdown_horizontal_rule(document *doc, uint64_t version, size_t pos) {
    const char* hr_marker; 
    char final_hr_str[10]; 
    int check_sol;
    size_t pos_for_line_check;

    if (!doc) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;
    if (pos > doc->total_len && doc->num_active_deletions == 0) return INVALID_CURSOR_POS; 
    
    hr_marker = "---";
    final_hr_str[0] = '\0';
    pos_for_line_check = adjust_pos_for_deletions(doc, pos);

    check_sol = is_start_of_line_or_doc(doc, pos_for_line_check);
    if (check_sol == 0) { 
        strcpy(final_hr_str, "\n");
    } else if (check_sol == -1 && pos_for_line_check > 0) {
        if (pos_for_line_check > 0) strcpy(final_hr_str, "\n");
    }
    strcat(final_hr_str, hr_marker);
    strcat(final_hr_str, "\n"); 
    
    return internal_insert_string(doc, pos, final_hr_str, 1); 
}

int markdown_link(document *doc, uint64_t version, size_t start, size_t end, const char *url) {
    int res;
    size_t url_len;
    char *link_suffix;
    size_t adjusted_start;
    size_t adjusted_end;

    if (!doc || !url) return INVALID_CURSOR_POS;
    if (version != doc->version) return OUTDATED_VERSION;

    adjusted_start = adjust_pos_for_deletions(doc, start);
    adjusted_end = adjust_pos_for_deletions(doc, end);
    
    if (adjusted_start >= adjusted_end || adjusted_end > doc->total_len) return INVALID_CURSOR_POS; 

    url_len = strlen(url);
    link_suffix = (char *)malloc(url_len + 3); 
    if (!link_suffix) return -1; 
    sprintf(link_suffix, "(%s)", url); 

    res = internal_insert_string(doc, adjusted_end, link_suffix, 0); 
    free(link_suffix);
    if (res != SUCCESS) return res;

    res = internal_insert_string(doc, adjusted_end, "]", 0); 
    if (res != SUCCESS) {
        return res;
    }
    
    res = internal_insert_string(doc, adjusted_start, "[", 0);
    return res;
}


// === Utilities ===
static char *flatten_live_content(const chunk *head_node, size_t list_total_len) {
    if (list_total_len == 0) { 
        char *empty_str = (char *)malloc(1);
        if (!empty_str) {
            perror("Failed to allocate for empty_str in flatten_live_content");
            return NULL; 
        }
        empty_str[0] = '\0';
        return empty_str;
    }
    if (!head_node && list_total_len > 0) {
        fprintf(stderr, "Warning: flatten_live_content called with NULL head but non-zero total_len.\n");
        char *empty_str = strdup(""); 
        if(!empty_str) perror("strdup failed for inconsistent state in flatten_live_content");
        return empty_str; 
    }
    
    char *flattened_str = (char *)malloc(list_total_len + 1);
    if (!flattened_str) {
        perror("Failed to allocate for flatten_live_content");
        return NULL;
    }

    char *ptr = flattened_str;
    const chunk *current_chunk = head_node; 
    size_t bytes_copied = 0;
    while (current_chunk != NULL) {
        if (current_chunk->len > 0 && current_chunk->text) { 
            size_t to_copy = current_chunk->len;
            if (bytes_copied + to_copy > list_total_len) {
                if (list_total_len > bytes_copied) { 
                    to_copy = list_total_len - bytes_copied;
                } else {
                    to_copy = 0; 
                }
            }
            if (to_copy > 0) { 
                memcpy(ptr, current_chunk->text, to_copy);
                ptr += to_copy;
                bytes_copied += to_copy;
            }
        }
        if (bytes_copied >= list_total_len && list_total_len > 0) break; 
        current_chunk = current_chunk->next;
    }
    *ptr = '\0';
    return flattened_str;
}


void markdown_print(const document *doc, FILE *stream) {
    if (!doc || !stream) {
        return;
    }
    if (doc->committed_content_cache) {
        fprintf(stream, "%s", doc->committed_content_cache);
    }
}

char *markdown_flatten(const document *doc) {
    if (!doc || !doc->committed_content_cache) {
        char *empty_str = strdup("");
        if (!empty_str) perror("strdup failed in markdown_flatten for empty");
        return empty_str; 
    }
    char* result = strdup(doc->committed_content_cache);
    if (!result) {
        perror("strdup failed in markdown_flatten");
    }
    return result;
}

// === Versioning ===
void markdown_increment_version(document *doc) {
    if (!doc) return;

    char *new_committed_content = flatten_live_content(doc->head, doc->total_len);
    if (!new_committed_content) {
        perror("Failed to flatten live content for version increment");
        if (!doc->committed_content_cache) { 
             doc->committed_content_cache = strdup(""); 
        }
        return; 
    }

    if (doc->committed_content_cache) {
        free(doc->committed_content_cache);
    }
    doc->committed_content_cache = new_committed_content;
    
    doc->version++; 
    doc->num_active_deletions = 0; 
}
