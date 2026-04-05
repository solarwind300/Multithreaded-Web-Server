# Makefile for Multi-threaded Web Server

CC = gcc
CFLAGS = -Wall -Wextra -pthread -O2
TARGET = webserver
SOURCES = webserver.c

.PHONY: all clean run test

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CC) $(CFLAGS) -o $(TARGET) $(SOURCES)

clean:
	rm -f $(TARGET) server.log
	rm -rf www

run: $(TARGET)
	./$(TARGET) 8080

# Create test files
setup-test:
	mkdir -p www/images www/styles
	echo '<!DOCTYPE html><html><head><title>Test Page</title></head><body><h1>Hello World!</h1><img src="images/test.png" alt="test"></body></html>' > www/test.html
	echo 'body { background-color: #f0f0f0; }' > www/styles/style.css
	echo 'This is a plain text file.' > www/readme.txt
	# Create a simple test image (1x1 PNG)
	printf '\x89PNG\r\n\x1a\n\x00\x00\x00\rIHDR\x00\x00\x00\x01\x00\x00\x00\x01\x08\x02\x00\x00\x00\x90wS\xde\x00\x00\x00\x0cIDATx\x9cc\xf8\x0f\x00\x00\x01\x01\x00\x05\x18\xd8N\x00\x00\x00\x00IEND\xaeB`\x82' > www/images/test.png
	chmod 000 www/forbidden.html 2>/dev/null || echo '<html><body>Forbidden</body></html>' > www/forbidden.html && chmod 000 www/forbidden.html

test: $(TARGET) setup-test
	@echo "Starting server for testing..."
	@./$(TARGET) 8080 &
	@sleep 1
	@echo "\n=== Testing GET request ==="
	curl -v http://127.0.0.1:8080/
	@echo "\n=== Testing HEAD request ==="
	curl -I http://127.0.0.1:8080/
	@echo "\n=== Testing 404 Not Found ==="
	curl -v http://127.0.0.1:8080/nonexistent.html
	@echo "\n=== Testing If-Modified-Since ==="
	curl -v -H "If-Modified-Since: Wed, 01 Jan 2099 00:00:00 GMT" http://127.0.0.1:8080/
	@pkill -f webserver || true
