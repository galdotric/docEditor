#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdint.h>

#define FIFO_PATH_BUFFER_SIZE 64

static volatile sig_atomic_t server_signal_received = 0;

void server_response_handler(int signal_number) {
    if (signal_number == SIGRTMIN + 1) {
        server_signal_received = 1;
    }
}

int initialize_signal_handling(void) {
    struct sigaction signal_action = {0};
    signal_action.sa_handler = server_response_handler;
    if (sigaction(SIGRTMIN + 1, &signal_action, NULL) == -1) {
        perror("Failed to set up signal handler");
        return -1;
    }
    return 0;
}

int connect_to_fifos(pid_t client_process_id, int *client_to_server_fd, int *server_to_client_fd) {
    char client_to_server_fifo_path[FIFO_PATH_BUFFER_SIZE];
    char server_to_client_fifo_path[FIFO_PATH_BUFFER_SIZE];

    snprintf(client_to_server_fifo_path, sizeof(client_to_server_fifo_path), "FIFO_C2S_%d", client_process_id);
    snprintf(server_to_client_fifo_path, sizeof(server_to_client_fifo_path), "FIFO_S2C_%d", client_process_id);

    *client_to_server_fd = open(client_to_server_fifo_path, O_WRONLY);
    if (*client_to_server_fd == -1) {
        perror("Failed to open client-to-server FIFO");
        return -1;
    }

    *server_to_client_fd = open(server_to_client_fifo_path, O_RDONLY);
    if (*server_to_client_fd == -1) {
        perror("Failed to open server-to-client FIFO");
        close(*client_to_server_fd);
        return -1;
    }
    return 0;
}

int send_data_to_server(int fd, const char *data) {
    if (write(fd, data, strlen(data)) == -1) {
        perror("Failed to write data");
        return -1;
    }
    if (write(fd, "\n", 1) == -1) {
        perror("Failed to write newline");
        return -1;
    }
    return 0;
}

int receive_initial_document_info(FILE *server_stream, char **document_content, uint64_t *version, size_t *length) {
    char buffer[128];
    if (!fgets(buffer, sizeof(buffer), server_stream)) return -1;
    *version = strtoull(buffer, NULL, 10);

    if (!fgets(buffer, sizeof(buffer), server_stream)) return -1;
    *length = strtoull(buffer, NULL, 10);

    *document_content = malloc(*length + 1);
    if (!*document_content) {
        perror("Failed to allocate memory for document");
        return -1;
    }
    if (fread(*document_content, 1, *length, server_stream) != *length) {
        free(*document_content);
        *document_content = NULL;
        return -1;
    }
    (*document_content)[*length] = '\0';
    return 0;
}

void process_log_response(FILE *server_stream) {
    char response_buffer[1024];
    while (fgets(response_buffer, sizeof(response_buffer), server_stream)) {
        printf("%s", response_buffer);
        if (strncmp(response_buffer, "END", 3) == 0) break;
    }
}

void process_doc_response(FILE *server_stream) {
    char response_buffer[1024];
    if (fgets(response_buffer, sizeof(response_buffer), server_stream)) printf("%s", response_buffer);
    if (fgets(response_buffer, sizeof(response_buffer), server_stream)) printf("%s", response_buffer);
}

void process_other_response(FILE *server_stream) {
    char response_buffer[1024];
    while (fgets(response_buffer, sizeof(response_buffer), server_stream)) {
        printf("%s", response_buffer);
        if (strncmp(response_buffer, "EDIT", 4) == 0) continue;
        if (strncmp(response_buffer, "SUCCESS", 7) == 0) break;
        if (strncmp(response_buffer, "Reject", 6) == 0) break;
    }
}

void command_interaction_loop(int client_to_server_fd, FILE *server_stream) {
    char input_buffer[256];
    while (1) {
        printf("> ");
        if (!fgets(input_buffer, sizeof(input_buffer), stdin)) break;
        input_buffer[strcspn(input_buffer, "\n")] = '\0';

        if (strcmp(input_buffer, "DISCONNECT") == 0) {
            send_data_to_server(client_to_server_fd, "DISCONNECT");
            break;
        }

        if (send_data_to_server(client_to_server_fd, input_buffer) == -1) {
            fprintf(stderr, "Failed to send command to server.\n");
            break;
        }

        if (strcmp(input_buffer, "LOG?") == 0) {
            process_log_response(server_stream);
        } else if (strcmp(input_buffer, "DOC?") == 0) {
            process_doc_response(server_stream);
        } else {
            process_other_response(server_stream);
        }
    }
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: %s <server_pid> <username>\n", argv[0]);
        return EXIT_FAILURE;
    }

    pid_t server_process_id = atoi(argv[1]);
    char *user_identifier = argv[2];

    if (kill(server_process_id, SIGRTMIN) == -1) {
        perror("Failed to send signal to server");
        return EXIT_FAILURE;
    }

    if (initialize_signal_handling() == -1) {
        return EXIT_FAILURE;
    }

    while (!server_signal_received) {
        pause();
    }

    int client_to_server_fd, server_to_client_fd;
    if (connect_to_fifos(getpid(), &client_to_server_fd, &server_to_client_fd) == -1) {
        return EXIT_FAILURE;
    }

    if (send_data_to_server(client_to_server_fd, user_identifier) == -1) {
        close(client_to_server_fd);
        close(server_to_client_fd);
        return EXIT_FAILURE;
    }

    FILE *server_stream = fdopen(server_to_client_fd, "r");
    if (!server_stream) {
        perror("Failed to fdopen server-to-client FIFO");
        close(client_to_server_fd);
        close(server_to_client_fd);
        return EXIT_FAILURE;
    }

    char assigned_role[32];
    if (!fgets(assigned_role, sizeof(assigned_role), server_stream)) {
        fprintf(stderr, "Failed to read role from server.\n");
        close(client_to_server_fd);
        fclose(server_stream);
        return EXIT_FAILURE;
    }
    assigned_role[strcspn(assigned_role, "\n")] = '\0';

    if (strncmp(assigned_role, "Reject", 6) == 0) {
        printf("%s\n", assigned_role);
        close(client_to_server_fd);
        fclose(server_stream);
        return EXIT_FAILURE;
    }
    printf("Role: %s\n", assigned_role);

    char *document_content = NULL;
    uint64_t document_version = 0;
    size_t document_length = 0;

    if (receive_initial_document_info(server_stream, &document_content, &document_version, &document_length) == -1) {
        fprintf(stderr, "Failed to receive initial document information.\n");
        close(client_to_server_fd);
        fclose(server_stream);
        return EXIT_FAILURE;
    }
    printf("Document (v%llu, %zu bytes):\n%s\n", (unsigned long long)document_version, document_length, document_content);

    command_interaction_loop(client_to_server_fd, server_stream);

    free(document_content);
    close(client_to_server_fd);
    fclose(server_stream);

    return EXIT_SUCCESS;
}
