# Multi-threaded Web Server

A simple HTTP/1.1 web server implemented in C using POSIX sockets and pthreads.

## Features

- Multi-threaded request handling
- HTTP/1.1 protocol support
- GET and HEAD methods
- Persistent connections (keep-alive)
- Conditional requests (If-Modified-Since)
- MIME type detection
- Request logging
- Status codes: 200, 304, 400, 403, 404

## Requirements

- GCC compiler
- POSIX-compliant system (Linux/macOS)
- pthread library

## Directory Structure

```
project/
├── webserver.c     # Main source code
├── Makefile        # Build automation
├── README.md       # Usage instructions
├── server.log      # Request log (generated)
└── www/            # Document root
    ├── index.html  # Default page
    ├── images/     # Image files
    └── styles/     # CSS files
```

## Compilation

```bash
# Using make
make

# Or compile manually
gcc -Wall -pthread -o webserver webserver.c
```

## Create test content

```bash
make setup-test
```

## Run the server

```bash
# Start server on default port (8080)
./webserver

# Start server on custom port
./webserver 3000
```

## Test in browser

Open in web browser: `http://127.0.0.1:8080/`

## Test with curl

```bash
# GET request
curl http://127.0.0.1:8080/

# HEAD request
curl -I http://127.0.0.1:8080/

# Conditional request
curl -H "If-Modified-Since: Wed, 01 Jan 2025 00:00:00 GMT" http://127.0.0.1:8080/ 

# Persistent connection
curl -H "Connection: keep-alive" http://127.0.0.1:8080/
```

## Stopping the Server

Press **Ctrl + C** to shutdown the server. 

## View logs

```bash
cat server.log
```
