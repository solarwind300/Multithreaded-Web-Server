/*
 * Multi-threaded Web Server
 * Supports HTTP/1.1 with GET and HEAD methods
 * Handles persistent and non-persistent connections
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <pthread.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>

#define DEFAULT_PORT 8080
#define BUFFER_SIZE 8192
#define MAX_HEADERS 50
#define MAX_PATH_LENGTH 512
#define LOG_FILE "server.log"
#define ROOT_DIR "./www"
#define KEEP_ALIVE_TIMEOUT 30
#define MAX_KEEP_ALIVE_REQUESTS 100

// Mutex for log file access
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

// Server running flag
volatile sig_atomic_t server_running = 1;

// HTTP Header structure
typedef struct {
    char name[256];
    char value[512];
} HttpHeader;

// HTTP Request structure
typedef struct {
    char method[16];
    char uri[MAX_PATH_LENGTH];
    char version[16];
    HttpHeader headers[MAX_HEADERS];
    int header_count;
    char *body;
    int body_length;
} HttpRequest;

// HTTP Response structure
typedef struct {
    int status_code;
    char status_text[64];
    HttpHeader headers[MAX_HEADERS];
    int header_count;
    char *body;
    long body_length;
    int send_body;  // For HEAD requests, we don't send body
} HttpResponse;

// Client info structure
typedef struct {
    int client_socket;
    struct sockaddr_in client_addr;
} ClientInfo;

// MIME type mapping
typedef struct {
    char *extension;
    char *mime_type;
} MimeType;

MimeType mime_types[] = {
    {".html", "text/html"},
    {".htm", "text/html"},
    {".css", "text/css"},
    {".js", "application/javascript"},
    {".json", "application/json"},
    {".xml", "application/xml"},
    {".txt", "text/plain"},
    {".png", "image/png"},
    {".jpg", "image/jpeg"},
    {".jpeg", "image/jpeg"},
    {".gif", "image/gif"},
    {".ico", "image/x-icon"},
    {".svg", "image/svg+xml"},
    {".pdf", "application/pdf"},
    {".zip", "application/zip"},
    {".mp3", "audio/mpeg"},
    {".mp4", "video/mp4"},
    {NULL, NULL}
};

void *handle_client(void *arg);
int parse_request(char *buffer, int length, HttpRequest *request);
void handle_request(int client_socket, HttpRequest *request, struct sockaddr_in *client_addr);
void send_response(int client_socket, HttpResponse *response);
void send_error_response(int client_socket, int status_code, const char *status_text);
char *get_mime_type(const char *filename);
void log_request(struct sockaddr_in *client_addr, const char *request_line, int status_code);
char *get_header_value(HttpRequest *request, const char *header_name);
void format_http_date(time_t t, char *buffer, size_t size);
time_t parse_http_date(const char *date_str);
int check_file_permissions(const char *filepath);
void cleanup_request(HttpRequest *request);
void cleanup_response(HttpResponse *response);
void signal_handler(int sig);
void url_decode(char *dst, const char *src);

// Signal handler for shutdown
void signal_handler(int sig) {
    if (sig == SIGINT || sig == SIGTERM) {
        printf("\nShutting down server...\n");
        server_running = 0;
		printf("Server stopped.\n");
		exit(0);
    }
}

// URL decode function
void url_decode(char *dst, const char *src) {
    char a, b;
    while (*src) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            (isxdigit(a) && isxdigit(b))) {
            if (a >= 'a') a -= 'a' - 'A';
            if (a >= 'A') a -= ('A' - 10);
            else a -= '0';
            if (b >= 'a') b -= 'a' - 'A';
            if (b >= 'A') b -= ('A' - 10);
            else b -= '0';
            *dst++ = 16 * a + b;
            src += 3;
        } else if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// Get MIME type based on file extension
char *get_mime_type(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (ext == NULL) {
        return "application/octet-stream";
    }
    
    for (int i = 0; mime_types[i].extension != NULL; i++) {
        if (strcasecmp(ext, mime_types[i].extension) == 0) {
            return mime_types[i].mime_type;
        }
    }
    
    return "application/octet-stream";
}

// Format time as HTTP date
void format_http_date(time_t t, char *buffer, size_t size) {
    struct tm *tm_info = gmtime(&t);
    strftime(buffer, size, "%a, %d %b %Y %H:%M:%S GMT", tm_info);
}

// Parse HTTP date string to time_t
time_t parse_http_date(const char *date_str) {
    struct tm tm_info;
    memset(&tm_info, 0, sizeof(struct tm));
    
    // Try RFC 1123 format: "Sun, 06 Nov 1994 08:49:37 GMT"
    if (strptime(date_str, "%a, %d %b %Y %H:%M:%S GMT", &tm_info) != NULL) {
        return timegm(&tm_info);
    }
    
    // Try RFC 850 format: "Sunday, 06-Nov-94 08:49:37 GMT"
    if (strptime(date_str, "%A, %d-%b-%y %H:%M:%S GMT", &tm_info) != NULL) {
        return timegm(&tm_info);
    }
    
    // Try ANSI C's asctime() format: "Sun Nov  6 08:49:37 1994"
    if (strptime(date_str, "%a %b %d %H:%M:%S %Y", &tm_info) != NULL) {
        return timegm(&tm_info);
    }
    
    return 0;
}

// Log request to file
void log_request(struct sockaddr_in *client_addr, const char *request_line, int status_code) {
    pthread_mutex_lock(&log_mutex);
    
    FILE *log_file = fopen(LOG_FILE, "a");
    if (log_file != NULL) {
        char time_str[64];
        time_t now = time(NULL);
        struct tm *tm_info = localtime(&now);
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);
        
        char *client_ip = inet_ntoa(client_addr->sin_addr);
        
        // Determine response type
        const char *response_type;
        switch (status_code) {
            case 200: response_type = "200 OK"; break;
            case 304: response_type = "304 Not Modified"; break;
            case 400: response_type = "400 Bad Request"; break;
            case 403: response_type = "403 Forbidden"; break;
            case 404: response_type = "404 Not Found"; break;
            default: response_type = "Unknown"; break;
        }
        
        fprintf(log_file, "%s | %s | %s | %s\n", 
                client_ip, time_str, request_line, response_type);
        fclose(log_file);
    }
    
    pthread_mutex_unlock(&log_mutex);
}

// Get header value from request
char *get_header_value(HttpRequest *request, const char *header_name) {
    for (int i = 0; i < request->header_count; i++) {
        if (strcasecmp(request->headers[i].name, header_name) == 0) {
            return request->headers[i].value;
        }
    }
    return NULL;
}

// Check file permissions
int check_file_permissions(const char *filepath) {
    struct stat file_stat;
    if (stat(filepath, &file_stat) != 0) {
        return -1;  // File doesn't exist
    }
    
    // Check if readable by others (simplified permission check)
    if (!(file_stat.st_mode & S_IROTH)) {
        // Check if we can read it as the owner/group
        if (access(filepath, R_OK) != 0) {
            return 0;  // Forbidden
        }
    }
    
    return 1;  // OK
}

// Parse HTTP request
int parse_request(char *buffer, int length, HttpRequest *request) {
    memset(request, 0, sizeof(HttpRequest));
    
    char *line_end = strstr(buffer, "\r\n");
    if (line_end == NULL) {
        return -1;  // Bad request
    }
    
    // Parse request line
    char request_line[1024];
    int line_length = line_end - buffer;
    if (line_length >= sizeof(request_line)) {
        return -1;
    }
    strncpy(request_line, buffer, line_length);
    request_line[line_length] = '\0';
    
    // Parse method, URI, and version
    if (sscanf(request_line, "%15s %511s %15s", 
               request->method, request->uri, request->version) != 3) {
        return -1;
    }
    
    // URL decode the URI
    char decoded_uri[MAX_PATH_LENGTH];
    url_decode(decoded_uri, request->uri);
    strncpy(request->uri, decoded_uri, MAX_PATH_LENGTH - 1);
    
    // Parse headers
    char *current = line_end + 2;
    while (current < buffer + length) {
        char *header_end = strstr(current, "\r\n");
        if (header_end == NULL) break;
        
        // Empty line indicates end of headers
        if (header_end == current) {
            current = header_end + 2;
            break;
        }
        
        // Parse header
        if (request->header_count < MAX_HEADERS) {
            char *colon = strchr(current, ':');
            if (colon != NULL && colon < header_end) {
                int name_len = colon - current;
                if (name_len < sizeof(request->headers[0].name)) {
                    strncpy(request->headers[request->header_count].name, 
                            current, name_len);
                    request->headers[request->header_count].name[name_len] = '\0';
                    
                    // Skip colon and whitespace
                    char *value_start = colon + 1;
                    while (*value_start == ' ' || *value_start == '\t') {
                        value_start++;
                    }
                    
                    int value_len = header_end - value_start;
                    if (value_len < sizeof(request->headers[0].value)) {
                        strncpy(request->headers[request->header_count].value,
                                value_start, value_len);
                        request->headers[request->header_count].value[value_len] = '\0';
                        request->header_count++;
                    }
                }
            }
        }
        
        current = header_end + 2;
    }
    
    return 0;
}

// Send HTTP response
void send_response(int client_socket, HttpResponse *response) {
    char header_buffer[4096];
    int offset = 0;
    
    // Status line
    offset += snprintf(header_buffer + offset, sizeof(header_buffer) - offset,
                       "HTTP/1.1 %d %s\r\n", 
                       response->status_code, response->status_text);
    
    // Headers
    for (int i = 0; i < response->header_count; i++) {
        offset += snprintf(header_buffer + offset, sizeof(header_buffer) - offset,
                          "%s: %s\r\n",
                          response->headers[i].name, response->headers[i].value);
    }
    
    // End of headers
    offset += snprintf(header_buffer + offset, sizeof(header_buffer) - offset, "\r\n");
    
    // Send headers
    send(client_socket, header_buffer, offset, 0);
    
    // Send body if present and if we should send it (not HEAD request)
    if (response->send_body && response->body != NULL && response->body_length > 0) {
        long bytes_sent = 0;
        while (bytes_sent < response->body_length) {
            long to_send = response->body_length - bytes_sent;
            if (to_send > BUFFER_SIZE) to_send = BUFFER_SIZE;
            
            ssize_t sent = send(client_socket, response->body + bytes_sent, to_send, 0);
            if (sent <= 0) break;
            bytes_sent += sent;
        }
    }
}

// Send error response
void send_error_response(int client_socket, int status_code, const char *status_text) {
    HttpResponse response;
    memset(&response, 0, sizeof(HttpResponse));
    
    response.status_code = status_code;
    strncpy(response.status_text, status_text, sizeof(response.status_text) - 1);
    
    // Create error page body
    char body[1024];
    snprintf(body, sizeof(body),
             "<!DOCTYPE html>\n"
             "<html>\n"
             "<head><title>%d %s</title></head>\n"
             "<body>\n"
             "<h1>%d %s</h1>\n"
             "<p>The requested operation could not be completed.</p>\n"
             "<hr>\n"
             "<p><em>Simple Web Server</em></p>\n"
             "</body>\n"
             "</html>\n",
             status_code, status_text, status_code, status_text);
    
    response.body = body;
    response.body_length = strlen(body);
    response.send_body = 1;
    
    // Add headers
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%ld", response.body_length);
    
    strcpy(response.headers[0].name, "Content-Type");
    strcpy(response.headers[0].value, "text/html");
    strcpy(response.headers[1].name, "Content-Length");
    strcpy(response.headers[1].value, content_length);
    strcpy(response.headers[2].name, "Connection");
    strcpy(response.headers[2].value, "close");
    
    char date_str[64];
    format_http_date(time(NULL), date_str, sizeof(date_str));
    strcpy(response.headers[3].name, "Date");
    strcpy(response.headers[3].value, date_str);
    
    strcpy(response.headers[4].name, "Server");
    strcpy(response.headers[4].value, "SimpleWebServer/1.0");
    
    response.header_count = 5;
    
    send_response(client_socket, &response);
}

// Handle HTTP request
void handle_request(int client_socket, HttpRequest *request, struct sockaddr_in *client_addr) {
    HttpResponse response;
    memset(&response, 0, sizeof(HttpResponse));
    response.send_body = 1;
    
    char request_line[1024];
    snprintf(request_line, sizeof(request_line), "%s %s %s",
             request->method, request->uri, request->version);
    
    // Check for valid HTTP version
    if (strncmp(request->version, "HTTP/", 5) != 0) {
        log_request(client_addr, request_line, 400);
        send_error_response(client_socket, 400, "Bad Request");
        return;
    }
    
    // Check for supported methods
    int is_get = (strcmp(request->method, "GET") == 0);
    int is_head = (strcmp(request->method, "HEAD") == 0);
    
    if (!is_get && !is_head) {
        log_request(client_addr, request_line, 400);
        send_error_response(client_socket, 400, "Bad Request");
        return;
    }
    
    // For HEAD request, don't send body
    if (is_head) {
        response.send_body = 0;
    }
    
    // Build file path
    char filepath[MAX_PATH_LENGTH];
    char *uri = request->uri;
    
    // Remove query string if present
    char *query = strchr(uri, '?');
    if (query != NULL) {
        *query = '\0';
    }
    
    // Default to index.html for root
    if (strcmp(uri, "/") == 0) {
        snprintf(filepath, sizeof(filepath), "%s/index.html", ROOT_DIR);
    } else {
        snprintf(filepath, sizeof(filepath), "%s%s", ROOT_DIR, uri);
    }
    
    // Security check: prevent directory traversal
    char *real_path = realpath(filepath, NULL);
    char *root_real_path = realpath(ROOT_DIR, NULL);
    
    if (real_path == NULL || root_real_path == NULL ||
        strncmp(real_path, root_real_path, strlen(root_real_path)) != 0) {
        free(real_path);
        free(root_real_path);
        log_request(client_addr, request_line, 403);
        send_error_response(client_socket, 403, "Forbidden");
        return;
    }
    
    free(real_path);
    free(root_real_path);
    
    // Check if file exists
    struct stat file_stat;
    if (stat(filepath, &file_stat) != 0) {
        log_request(client_addr, request_line, 404);
        send_error_response(client_socket, 404, "Not Found");
        return;
    }
    
    // Check if it's a directory
    if (S_ISDIR(file_stat.st_mode)) {
        // Try to serve index.html from directory
        char index_path[MAX_PATH_LENGTH];
        snprintf(index_path, sizeof(index_path), "%s/index.html", filepath);
        if (stat(index_path, &file_stat) != 0) {
            log_request(client_addr, request_line, 404);
            send_error_response(client_socket, 404, "Not Found");
            return;
        }
        strncpy(filepath, index_path, sizeof(filepath) - 1);
    }
    
    // Check file permissions
    if (access(filepath, R_OK) != 0) {
        log_request(client_addr, request_line, 403);
        send_error_response(client_socket, 403, "Forbidden");
        return;
    }
    
    // Check If-Modified-Since header
    char *if_modified_since = get_header_value(request, "If-Modified-Since");
    if (if_modified_since != NULL) {
        time_t if_mod_time = parse_http_date(if_modified_since);
        if (if_mod_time > 0 && file_stat.st_mtime <= if_mod_time) {
            // File has not been modified
            response.status_code = 304;
            strcpy(response.status_text, "Not Modified");
            
            char date_str[64];
            format_http_date(time(NULL), date_str, sizeof(date_str));
            strcpy(response.headers[0].name, "Date");
            strcpy(response.headers[0].value, date_str);
            
            strcpy(response.headers[1].name, "Server");
            strcpy(response.headers[1].value, "SimpleWebServer/1.0");
            
            response.header_count = 2;
            response.body = NULL;
            response.body_length = 0;
            response.send_body = 0;
            
            log_request(client_addr, request_line, 304);
            send_response(client_socket, &response);
            return;
        }
    }
    
    // Open and read file
    FILE *file = fopen(filepath, "rb");
    if (file == NULL) {
        log_request(client_addr, request_line, 404);
        send_error_response(client_socket, 404, "Not Found");
        return;
    }
    
    // Get file size
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    // Allocate buffer and read file
    char *file_buffer = NULL;
    if (response.send_body) {
        file_buffer = (char *)malloc(file_size);
        if (file_buffer == NULL) {
            fclose(file);
            log_request(client_addr, request_line, 404);
            send_error_response(client_socket, 404, "Not Found");
            return;
        }
        
        size_t bytes_read = fread(file_buffer, 1, file_size, file);
        if (bytes_read != file_size) {
            free(file_buffer);
            fclose(file);
            log_request(client_addr, request_line, 404);
            send_error_response(client_socket, 404, "Not Found");
            return;
        }
    }
    
    fclose(file);
    
    // Build response
    response.status_code = 200;
    strcpy(response.status_text, "OK");
    response.body = file_buffer;
    response.body_length = file_size;
    
    // Add headers
    int header_idx = 0;
    
    // Date header
    char date_str[64];
    format_http_date(time(NULL), date_str, sizeof(date_str));
    strcpy(response.headers[header_idx].name, "Date");
    strcpy(response.headers[header_idx].value, date_str);
    header_idx++;
    
    // Server header
    strcpy(response.headers[header_idx].name, "Server");
    strcpy(response.headers[header_idx].value, "SimpleWebServer/1.0");
    header_idx++;
    
    // Last-Modified header
    char last_modified[64];
    format_http_date(file_stat.st_mtime, last_modified, sizeof(last_modified));
    strcpy(response.headers[header_idx].name, "Last-Modified");
    strcpy(response.headers[header_idx].value, last_modified);
    header_idx++;
    
    // Content-Type header
    strcpy(response.headers[header_idx].name, "Content-Type");
    strcpy(response.headers[header_idx].value, get_mime_type(filepath));
    header_idx++;
    
    // Content-Length header
    char content_length[32];
    snprintf(content_length, sizeof(content_length), "%ld", file_size);
    strcpy(response.headers[header_idx].name, "Content-Length");
    strcpy(response.headers[header_idx].value, content_length);
    header_idx++;
    
    // Connection header
    char *connection = get_header_value(request, "Connection");
    strcpy(response.headers[header_idx].name, "Connection");
    if (connection != NULL && strcasecmp(connection, "keep-alive") == 0) {
        strcpy(response.headers[header_idx].value, "keep-alive");
    } else {
        strcpy(response.headers[header_idx].value, "close");
    }
    header_idx++;
    
    response.header_count = header_idx;
    
    log_request(client_addr, request_line, 200);
    send_response(client_socket, &response);
    
    // Free file buffer
    if (file_buffer != NULL) {
        free(file_buffer);
    }
}

// Thread function to handle client
void *handle_client(void *arg) {
    ClientInfo *client_info = (ClientInfo *)arg;
    int client_socket = client_info->client_socket;
    struct sockaddr_in client_addr = client_info->client_addr;
    free(client_info);
    
    char buffer[BUFFER_SIZE];
    int keep_alive = 1;
    int request_count = 0;
    
    // Set socket timeout for keep-alive
    struct timeval timeout;
    timeout.tv_sec = KEEP_ALIVE_TIMEOUT;
    timeout.tv_usec = 0;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    
    while (keep_alive && request_count < MAX_KEEP_ALIVE_REQUESTS) {
        memset(buffer, 0, BUFFER_SIZE);
        
        // Receive request
        int total_received = 0;
        int header_end_found = 0;
        
        while (total_received < BUFFER_SIZE - 1 && !header_end_found) {
            ssize_t bytes = recv(client_socket, buffer + total_received, 
                                BUFFER_SIZE - 1 - total_received, 0);
            
            if (bytes <= 0) {
                // Connection closed or error
                keep_alive = 0;
                break;
            }
            
            total_received += bytes;
            buffer[total_received] = '\0';
            
            // Check for end of headers
            if (strstr(buffer, "\r\n\r\n") != NULL) {
                header_end_found = 1;
            }
        }
        
        if (!header_end_found || total_received == 0) {
            break;
        }
        
        // Parse request
        HttpRequest request;
        if (parse_request(buffer, total_received, &request) != 0) {
            log_request(&client_addr, "Invalid Request", 400);
            send_error_response(client_socket, 400, "Bad Request");
            break;
        }
        
        // Handle request
        handle_request(client_socket, &request, &client_addr);
        
        request_count++;
        
        // Check Connection header
        char *connection = get_header_value(&request, "Connection");
        if (connection != NULL) {
            if (strcasecmp(connection, "close") == 0) {
                keep_alive = 0;
            } else if (strcasecmp(connection, "keep-alive") == 0) {
                keep_alive = 1;
            }
        } else {
            // HTTP/1.1 default is keep-alive, HTTP/1.0 default is close
            if (strstr(request.version, "1.0") != NULL) {
                keep_alive = 0;
            }
        }
    }
    
    close(client_socket);
    printf("Client disconnected: %s:%d (handled %d requests)\n",
           inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port),
           request_count);
    
    return NULL;
}

int main(int argc, char *argv[]) {
    int port = DEFAULT_PORT;
    
    // Parse command line arguments
    if (argc > 1) {
        port = atoi(argv[1]);
        if (port <= 0 || port > 65535) {
            fprintf(stderr, "Invalid port number. Using default port %d\n", DEFAULT_PORT);
            port = DEFAULT_PORT;
        }
    }
    
    // Set up signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);  // Ignore broken pipe
    
    // Create root directory if it doesn't exist
    struct stat st = {0};
    if (stat(ROOT_DIR, &st) == -1) {
        mkdir(ROOT_DIR, 0755);
        printf("Created document root directory: %s\n", ROOT_DIR);
        
        // Create a default index.html
        char index_path[MAX_PATH_LENGTH];
        snprintf(index_path, sizeof(index_path), "%s/index.html", ROOT_DIR);
        FILE *index_file = fopen(index_path, "w");
        if (index_file != NULL) {
            fprintf(index_file,
                "<!DOCTYPE html>\n"
                "<html>\n"
                "<head>\n"
                "    <title>Welcome to Simple Web Server</title>\n"
                "    <style>\n"
                "        body { font-family: Arial, sans-serif; margin: 40px; }\n"
                "        h1 { color: #333; }\n"
                "    </style>\n"
                "</head>\n"
                "<body>\n"
                "    <h1>Welcome to Simple Web Server!</h1>\n"
                "    <p>The server is running successfully.</p>\n"
                "    <p>Place your files in the <code>www</code> directory.</p>\n"
                "</body>\n"
                "</html>\n");
            fclose(index_file);
            printf("Created default index.html\n");
        }
    }
    
    // Create server socket
    int server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket < 0) {
        perror("Failed to create socket");
        exit(EXIT_FAILURE);
    }
    
    // Set socket options
    int opt = 1;
    if (setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0) {
        perror("Failed to set socket options");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    // Bind socket
    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);
    
    if (bind(server_socket, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Failed to bind socket");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    // Listen for connections
    if (listen(server_socket, 10) < 0) {
        perror("Failed to listen");
        close(server_socket);
        exit(EXIT_FAILURE);
    }
    
    printf("========================================\n");
    printf("  Multi-threaded Web Server Started\n");
    printf("========================================\n");
    printf("  Port: %d\n", port);
    printf("  Document Root: %s\n", ROOT_DIR);
    printf("  Log File: %s\n", LOG_FILE);
    printf("  URL: http://127.0.0.1:%d/\n", port);
    printf("========================================\n");
    printf("Press Ctrl+C to stop the server\n\n");
    
    // Accept connections
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket, (struct sockaddr *)&client_addr, &client_len);
        
        if (client_socket < 0) {
            if (server_running) {
                perror("Failed to accept connection");
            }
            continue;
        }
        
        printf("Client connected: %s:%d\n",
               inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        
        // Create client info
        ClientInfo *client_info = (ClientInfo *)malloc(sizeof(ClientInfo));
        if (client_info == NULL) {
            close(client_socket);
            continue;
        }
        client_info->client_socket = client_socket;
        client_info->client_addr = client_addr;
        
        // Create thread to handle client
        pthread_t thread;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        
        if (pthread_create(&thread, &attr, handle_client, client_info) != 0) {
            perror("Failed to create thread");
            free(client_info);
            close(client_socket);
        }
        
        pthread_attr_destroy(&attr);
    }
    
    close(server_socket);
    
    return 0;
}
