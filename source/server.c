#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include "markdown.h"

#define FIFO_PATH_BUFFER_SIZE 64
#define MAX_COMMAND_LOG_ENTRIES 1024

static volatile sig_atomic_t client_signal_pending = 0;
static pid_t connecting_client_pid = 0;
static document *global_document_ptr = NULL;
static pthread_mutex_t document_access_mutex = PTHREAD_MUTEX_INITIALIZER;

static int active_client_connections = 0;
static pthread_mutex_t client_counter_mutex = PTHREAD_MUTEX_INITIALIZER;

char *server_command_log[MAX_COMMAND_LOG_ENTRIES];
int server_log_entry_count = 0;

void record_server_command(const char *username, const char *command_details, const char *outcome_message);

void client_connection_signal_receiver(int signal_num, siginfo_t *signal_info, void *context) {
    if (signal_num == SIGRTMIN) {
        client_signal_pending = 1;
        connecting_client_pid = signal_info->si_pid;
    }
}

void remove_leading_trailing_whitespace(char *input_string) {
    char *string_end;
    while (*input_string == ' ' || *input_string == '\t') input_string++;
    string_end = input_string + strlen(input_string) - 1;
    while (string_end > input_string && (*string_end == ' ' || *string_end == '\t' || *string_end == '\n')) {
        *string_end-- = '\0';
    }
}

int find_user_permission(const char *username, char *permission_buffer, size_t buffer_size) {
    FILE *roles_file_ptr = fopen("roles.txt", "r");
    if (!roles_file_ptr) return 0;
    char current_user[64], current_permission[16];
    while (fscanf(roles_file_ptr, "%63s %15s", current_user, current_permission) == 2) {
        if (strcmp(current_user, username) == 0) {
            strncpy(permission_buffer, current_permission, buffer_size - 1);
            permission_buffer[buffer_size - 1] = '\0';
            fclose(roles_file_ptr);
            return 1;
        }
    }
    fclose(roles_file_ptr);
    return 0;
}

int setup_client_fifos(pid_t client_pid, char *c2s_path, char *s2c_path) {
    snprintf(c2s_path, FIFO_PATH_BUFFER_SIZE, "FIFO_C2S_%d", client_pid);
    snprintf(s2c_path, FIFO_PATH_BUFFER_SIZE, "FIFO_S2C_%d", client_pid);

    unlink(c2s_path);
    unlink(s2c_path);

    if (mkfifo(c2s_path, 0666) == -1) {
        perror("Failed to create C2S FIFO");
        return -1;
    }
    if (mkfifo(s2c_path, 0666) == -1) {
        perror("Failed to create S2C FIFO");
        unlink(c2s_path);
        return -1;
    }
    return 0;
}

int open_client_fifos(const char *c2s_path, const char *s2c_path, int *c2s_fd, int *s2c_fd) {
    *c2s_fd = open(c2s_path, O_RDONLY);
    if (*c2s_fd == -1) {
        perror("Failed to open C2S FIFO for reading");
        return -1;
    }
    *s2c_fd = open(s2c_path, O_WRONLY);
    if (*s2c_fd == -1) {
        perror("Failed to open S2C FIFO for writing");
        close(*c2s_fd);
        return -1;
    }
    return 0;
}

void cleanup_client_fifos(const char *c2s_path, const char *s2c_path, int c2s_fd, int s2c_fd, FILE* c2s_stream) {
    if(c2s_stream) fclose(c2s_stream);
    else if (c2s_fd != -1) close(c2s_fd);
    if (s2c_fd != -1) close(s2c_fd);
    unlink(c2s_path);
    unlink(s2c_path);
}


int transmit_initial_data_to_client(int s2c_fd, const char *user_role) {
    dprintf(s2c_fd, "%s\n", user_role);

    pthread_mutex_lock(&document_access_mutex);
    uint64_t current_version = global_document_ptr->version;
    char *flattened_document = markdown_flatten(global_document_ptr);
    size_t document_length = flattened_document ? strlen(flattened_document) : 0;
    dprintf(s2c_fd, "%llu\n%zu\n", (unsigned long long)current_version, document_length);
    if (document_length > 0) {
        if (write(s2c_fd, flattened_document, document_length) != (ssize_t)document_length) {
            perror("Failed to write document content to client");
            free(flattened_document);
            pthread_mutex_unlock(&document_access_mutex);
            return -1;
        }
    }
    free(flattened_document);
    pthread_mutex_unlock(&document_access_mutex);
    return 0;
}

void handle_insert_command(const char* cmd_params, int s2c_fd, const char* username, char* response_msg, int is_writer) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    size_t position;
    char text_to_insert[192];
    if (sscanf(cmd_params, "%zu %[^\n]", &position, text_to_insert) == 2) {
        int op_result = markdown_insert(global_document_ptr, global_document_ptr->version, position, text_to_insert);
        if (op_result == 0) markdown_increment_version(global_document_ptr);
        else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
        else if (op_result == -3) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
    } else {
        strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
    }
}

void handle_delete_command(const char* cmd_params, int s2c_fd, const char* username, char* response_msg, int is_writer, int with_version) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    unsigned long version_val;
    size_t position, length;
    int op_result;

    if (with_version) {
        if (sscanf(cmd_params, "%lu %zu %zu", &version_val, &position, &length) == 3) {
            op_result = markdown_delete(global_document_ptr, version_val, position, length);
        } else {
            strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
            return;
        }
    } else {
         if (sscanf(cmd_params, "%zu %zu", &position, &length) == 2) {
            op_result = markdown_delete(global_document_ptr, global_document_ptr->version, position, length);
        } else {
            strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
            return;
        }
    }

    if (op_result == 0) markdown_increment_version(global_document_ptr);
    else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
    else if (op_result == -2) strcpy(response_msg, "Reject DELETED_POSITION\n");
    else if (op_result == -3 && with_version) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
}


void handle_simple_pos_command(int (*md_func)(document*, uint64_t, size_t), const char* cmd_params, char* response_msg, int is_writer) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    size_t position;
    if (sscanf(cmd_params, "%zu", &position) == 1) {
        int op_result = md_func(global_document_ptr, global_document_ptr->version, position);
        if (op_result == 0) markdown_increment_version(global_document_ptr);
        else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
        else if (op_result == -3) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
    } else {
        strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
    }
}

void handle_heading_command(const char* cmd_params, char* response_msg, int is_writer) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    size_t level, position;
    if (sscanf(cmd_params, "%zu %zu", &level, &position) == 2) {
        int op_result = markdown_heading(global_document_ptr, global_document_ptr->version, level, position);
        if (op_result == 0) markdown_increment_version(global_document_ptr);
        else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
        else if (op_result == -3) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
    } else {
        strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
    }
}

void handle_range_command(int (*md_func)(document*, uint64_t, size_t, size_t), const char* cmd_params, char* response_msg, int is_writer) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    size_t start_pos, end_pos;
    if (sscanf(cmd_params, "%zu %zu", &start_pos, &end_pos) == 2) {
        int op_result = md_func(global_document_ptr, global_document_ptr->version, start_pos, end_pos);
        if (op_result == 0) markdown_increment_version(global_document_ptr);
        else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
        else if (op_result == -3) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
    } else {
        strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
    }
}

void handle_link_command(const char* cmd_params, char* response_msg, int is_writer) {
    if (!is_writer) {
        strcpy(response_msg, "Reject UNAUTHORISED\n");
        return;
    }
    size_t start_pos, end_pos;
    char url_buffer[128];
    if (sscanf(cmd_params, "%zu %zu %127s", &start_pos, &end_pos, url_buffer) == 3) {
        int op_result = markdown_link(global_document_ptr, global_document_ptr->version, start_pos, end_pos, url_buffer);
        if (op_result == 0) markdown_increment_version(global_document_ptr);
        else if (op_result == -1) strcpy(response_msg, "Reject INVALID_POSITION\n");
        else if (op_result == -3) strcpy(response_msg, "Reject OUTDATED_VERSION\n");
    } else {
        strcpy(response_msg, "Reject INVALID_COMMAND_FORMAT\n");
    }
}

void handle_doc_query_command(int s2c_fd, char* response_msg) {
    strcpy(response_msg, "SUCCESS\n");
    dprintf(s2c_fd, "%s", response_msg);
    char *flattened_doc = markdown_flatten(global_document_ptr);
    if (flattened_doc) {
        dprintf(s2c_fd, "%s\n", flattened_doc);
        free(flattened_doc);
    } else {
        dprintf(s2c_fd, "\n");
    }
}

void handle_log_query_command(int s2c_fd, char* response_msg) {
    strcpy(response_msg, "SUCCESS\n");
    dprintf(s2c_fd, "%s", response_msg);
    for (int i = 0; i < server_log_entry_count; ++i) {
        dprintf(s2c_fd, "%s", server_command_log[i]);
    }
    dprintf(s2c_fd, "END\n");
}


void *manage_client_connection(void *arg_ptr) {
    pid_t current_client_pid = *(pid_t *)arg_ptr;
    free(arg_ptr);

    char c2s_fifo_name[FIFO_PATH_BUFFER_SIZE], s2c_fifo_name[FIFO_PATH_BUFFER_SIZE];
    int c2s_descriptor = -1, s2c_descriptor = -1;
    FILE *c2s_file_stream = NULL;

    if (setup_client_fifos(current_client_pid, c2s_fifo_name, s2c_fifo_name) == -1) {
        return NULL;
    }

    kill(current_client_pid, SIGRTMIN + 1);

    if (open_client_fifos(c2s_fifo_name, s2c_fifo_name, &c2s_descriptor, &s2c_descriptor) == -1) {
        cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, c2s_descriptor, s2c_descriptor, NULL);
        return NULL;
    }

    char client_username[64];
    ssize_t bytes_read = read(c2s_descriptor, client_username, sizeof(client_username) - 1);
    if (bytes_read <= 0) {
        cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, c2s_descriptor, s2c_descriptor, NULL);
        return NULL;
    }
    client_username[strcspn(client_username, "\n")] = '\0';

    char client_role[16];
    if (!find_user_permission(client_username, client_role, sizeof(client_role))) {
        dprintf(s2c_descriptor, "Reject UNAUTHORISED\n");
        sleep(1);
        cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, c2s_descriptor, s2c_descriptor, NULL);
        return NULL;
    }

    pthread_mutex_lock(&client_counter_mutex);
    active_client_connections++;
    pthread_mutex_unlock(&client_counter_mutex);

    if (transmit_initial_data_to_client(s2c_descriptor, client_role) == -1) {
        cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, c2s_descriptor, s2c_descriptor, NULL);
        pthread_mutex_lock(&client_counter_mutex);
        active_client_connections--;
        pthread_mutex_unlock(&client_counter_mutex);
        return NULL;
    }

    c2s_file_stream = fdopen(c2s_descriptor, "r");
    if (!c2s_file_stream) {
        perror("fdopen c2s_descriptor failed");
        cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, c2s_descriptor, s2c_descriptor, NULL);
        pthread_mutex_lock(&client_counter_mutex);
        active_client_connections--;
        pthread_mutex_unlock(&client_counter_mutex);
        return NULL;
    }


    char command_input_buffer[256];
    while (fgets(command_input_buffer, sizeof(command_input_buffer), c2s_file_stream)) {
        command_input_buffer[strcspn(command_input_buffer, "\n")] = '\0';
        if (strcmp(command_input_buffer, "DISCONNECT") == 0) {
            break;
        }

        char response_message[128] = "SUCCESS\n";
        int user_has_write_permission = (strcmp(client_role, "write") == 0);
        int needs_logging_and_edit_broadcast = 1;


        pthread_mutex_lock(&document_access_mutex);

        if (strncmp(command_input_buffer, "INSERT ", 7) == 0) {
            handle_insert_command(command_input_buffer + 7, s2c_descriptor, client_username, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "DELETE ", 7) == 0) {
            handle_delete_command(command_input_buffer + 7, s2c_descriptor, client_username, response_message, user_has_write_permission, 1);
        } else if (strncmp(command_input_buffer, "DEL ", 4) == 0) {
            handle_delete_command(command_input_buffer + 4, s2c_descriptor, client_username, response_message, user_has_write_permission, 0);
        } else if (strncmp(command_input_buffer, "NEWLINE ", 8) == 0) {
            handle_simple_pos_command(markdown_newline, command_input_buffer + 8, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "HEADING ", 8) == 0) {
            handle_heading_command(command_input_buffer + 8, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "BOLD ", 5) == 0) {
            handle_range_command(markdown_bold, command_input_buffer + 5, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "ITALIC ", 7) == 0) {
            handle_range_command(markdown_italic, command_input_buffer + 7, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "BLOCKQUOTE ", 11) == 0) {
            handle_simple_pos_command(markdown_blockquote, command_input_buffer + 11, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "ORDERED_LIST ", 13) == 0) {
            handle_simple_pos_command(markdown_ordered_list, command_input_buffer + 13, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "UNORDERED_LIST ", 15) == 0) {
            handle_simple_pos_command(markdown_unordered_list, command_input_buffer + 15, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "CODE ", 5) == 0) {
            handle_range_command(markdown_code, command_input_buffer + 5, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "HORIZONTAL_RULE ", 16) == 0) {
            handle_simple_pos_command(markdown_horizontal_rule, command_input_buffer + 16, response_message, user_has_write_permission);
        } else if (strncmp(command_input_buffer, "LINK ", 5) == 0) {
            handle_link_command(command_input_buffer + 5, response_message, user_has_write_permission);
        } else if (strcmp(command_input_buffer, "DOC?") == 0) {
            handle_doc_query_command(s2c_descriptor, response_message);
            needs_logging_and_edit_broadcast = 0;
        } else if (strcmp(command_input_buffer, "LOG?") == 0) {
            handle_log_query_command(s2c_descriptor, response_message);
            needs_logging_and_edit_broadcast = 0;
        } else if (strcmp(command_input_buffer, "PERM?") == 0) {
            snprintf(response_message, sizeof(response_message), "%s\n", client_role);
            needs_logging_and_edit_broadcast = 0;
        } else {
            strcpy(response_message, "Reject INVALID_COMMAND\n");
            needs_logging_and_edit_broadcast = 0;
        }
        pthread_mutex_unlock(&document_access_mutex);

        if (needs_logging_and_edit_broadcast && strcmp(response_message, "Reject INVALID_COMMAND_FORMAT\n") !=0 && strcmp(response_message,"Reject INVALID_COMMAND\n") != 0) {
             record_server_command(client_username, command_input_buffer, response_message);
             if (server_log_entry_count > 0) {
                 printf("%s", server_command_log[server_log_entry_count - 1]);
                 fflush(stdout);
                 dprintf(s2c_descriptor, "%s", server_command_log[server_log_entry_count - 1]);
             }
        }
        dprintf(s2c_descriptor, "%s", response_message);
    }

    cleanup_client_fifos(c2s_fifo_name, s2c_fifo_name, -1, s2c_descriptor, c2s_file_stream); // c2s_descriptor is handled by fdopen/fclose

    pthread_mutex_lock(&client_counter_mutex);
    active_client_connections--;
    pthread_mutex_unlock(&client_counter_mutex);

    return NULL;
}

void record_server_command(const char *username, const char *command_details, const char *outcome_message) {
    if (server_log_entry_count < MAX_COMMAND_LOG_ENTRIES) {
        size_t required_length = strlen(username) + strlen(command_details) + strlen(outcome_message) + 32;
        server_command_log[server_log_entry_count] = malloc(required_length);
        if (server_command_log[server_log_entry_count]) {
            snprintf(server_command_log[server_log_entry_count], required_length, "EDIT %s %s %s", username, command_details, outcome_message);
            server_log_entry_count++;
        }
    }
}

void initialize_server_signals(void) {
    struct sigaction signal_action_config = {0};
    signal_action_config.sa_sigaction = client_connection_signal_receiver;
    signal_action_config.sa_flags = SA_SIGINFO;
    sigaction(SIGRTMIN, &signal_action_config, NULL);
}

void process_server_shutdown(void) {
    pthread_mutex_lock(&client_counter_mutex);
    if (active_client_connections == 0) {
        FILE *output_doc_file = fopen("doc.md", "w");
        if (output_doc_file) {
            pthread_mutex_lock(&document_access_mutex);
            char *final_document_content = markdown_flatten(global_document_ptr);
            pthread_mutex_unlock(&document_access_mutex);
            if (final_document_content) {
                fwrite(final_document_content, 1, strlen(final_document_content), output_doc_file);
                free(final_document_content);
            }
            fclose(output_doc_file);
        }
        printf("Server shutting down. Document saved to doc.md.\n");
        markdown_free(global_document_ptr);
        for(int i=0; i < server_log_entry_count; ++i) free(server_command_log[i]);
        exit(EXIT_SUCCESS);
    } else {
        printf("Shutdown rejected: %d client(s) still connected.\n", active_client_connections);
    }
    pthread_mutex_unlock(&client_counter_mutex);
}

void server_main_loop(void) {
    while (1) {
        fd_set read_file_descriptors;
        FD_ZERO(&read_file_descriptors);
        FD_SET(STDIN_FILENO, &read_file_descriptors);
        struct timeval timeout_interval = {0, 100000};
        int select_ready_count = select(STDIN_FILENO + 1, &read_file_descriptors, NULL, NULL, &timeout_interval);

        if (select_ready_count > 0 && FD_ISSET(STDIN_FILENO, &read_file_descriptors)) {
            char shutdown_command_buffer[16];
            if (fgets(shutdown_command_buffer, sizeof(shutdown_command_buffer), stdin)) {
                if (strncmp(shutdown_command_buffer, "QUIT", 4) == 0) {
                    process_server_shutdown();
                }
            }
        }

        if (client_signal_pending) {
            pthread_t client_thread_id;
            pid_t *new_client_pid_ptr = malloc(sizeof(pid_t));
            if (!new_client_pid_ptr) {
                perror("Failed to allocate memory for client PID");
                client_signal_pending = 0; // Reset flag to avoid busy loop if malloc keeps failing
                continue;
            }
            *new_client_pid_ptr = connecting_client_pid;
            if (pthread_create(&client_thread_id, NULL, manage_client_connection, new_client_pid_ptr) != 0) {
                perror("Failed to create client thread");
                free(new_client_pid_ptr); // Clean up if thread creation failed
            } else {
                pthread_detach(client_thread_id);
            }
            client_signal_pending = 0;
        }
    }
}

int main(int argc, char *argv[]) {
    printf("Server Process ID: %d\n", getpid());

    initialize_server_signals();
    global_document_ptr = markdown_init();
    if (!global_document_ptr) {
        fprintf(stderr, "Failed to initialize markdown document.\n");
        return EXIT_FAILURE;
    }

    server_main_loop();

    markdown_free(global_document_ptr);
    for(int i=0; i < server_log_entry_count; ++i) free(server_command_log[i]);
    return EXIT_SUCCESS;
}
