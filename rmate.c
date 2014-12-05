/*
 * Copyright (c) 2014 Mael Clerambault <mael@clerambault.fr>
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */


#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define HOST "localhost"
#define PORT "52698"

#define MAXDATASIZE 1024

enum CMD_STATE {
    CMD_HEADER,
    CMD_CMD,
    CMD_VAR,
    CMD_END
};

struct cmd {
    enum CMD_STATE state;
    enum {UNKNOWN, CLOSE, SAVE} cmd_type;
    char* filename;
    size_t file_len;
};

int connect_mate(const char* host, const char* port) {
	int sockfd = -1;  
	struct addrinfo hints, *servinfo, *p;
	int rv;

	memset(&hints, 0, sizeof hints);
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	if ((rv = getaddrinfo(host, port, &hints, &servinfo)) != 0) {
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(rv));
		return 1;
	}

	// loop through all the results and connect to the first we can
	for(p = servinfo; p != NULL; p = p->ai_next) {
		if ((sockfd = socket(p->ai_family, p->ai_socktype,
				p->ai_protocol)) == -1) {
			continue;
		}

		if (connect(sockfd, p->ai_addr, p->ai_addrlen) == -1) {
			close(sockfd);
            sockfd = -1;
			continue;
		}

		break;
	}

	freeaddrinfo(servinfo); // all done with this structure
    
    return sockfd;
}

int send_open(int sockfd, const char* filename, int fd) {
    char *fdata;
    char resolved[PATH_MAX];
    struct stat st;
    
    if(fstat(fd, &st) == -1) {
        perror("stat");
        return -1;
    }
    
    if((fdata = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    
    dprintf(sockfd, "open\n");
    dprintf(sockfd, "display-name: %s\n", filename);
    dprintf(sockfd, "real-path: %s\n", realpath(filename, resolved));
    dprintf(sockfd, "data-on-save: yes\n");
    dprintf(sockfd, "re-activate: yes\n");
    dprintf(sockfd, "token: %s\n", filename);
    dprintf(sockfd, "data: %zd\n", st.st_size);
    write(sockfd, fdata, st.st_size);
    dprintf(sockfd, "\n.\n");
    
    munmap(fdata, st.st_size);
    return 0;
}

int receive_save(int sockfd, char* rem_buf, size_t rem_buf_len, const char* filename, size_t filesize) {
    char *fdata;
    int fd, numbytes;

    if((fd = open(filename, O_RDWR)) == -1) {
        perror("open");
        return -1;
    }
    
    if(ftruncate(fd, filesize) == -1) {
        perror("ftruncate");
        return -1;
    }
    
    if((fdata = mmap(NULL, filesize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        perror("mmap");
        return -1;
    }
    
    if(rem_buf_len > filesize)
        rem_buf_len = filesize;
    
    memcpy(fdata, rem_buf, rem_buf_len);
    if((numbytes = read(sockfd, fdata + rem_buf_len, filesize - rem_buf_len)) == -1) {
        perror("read");
        return -1;
    }
    
    if(munmap(fdata, filesize) == -1) {
        perror("munmap");
        return -1;
    }
        
    close(fd);
    return 0;
}

ssize_t readline(char* buf, size_t len) {
    char *cmd_str;
    ssize_t line_len;
    
    cmd_str = memchr(buf, '\n', len);
    if(!cmd_str)
        return -1;
    
    line_len = cmd_str - buf;
    if(line_len > 0 && cmd_str[-1] == '\r')
        cmd_str[-1] = '\0';
    cmd_str[0] = '\0';
    
    return line_len + 1;
}

void handle_var(const char* name, const char* value, struct cmd *cmd_state) {
    if(!strcmp(name, "token"))
        cmd_state->filename = strdup(value);
    
    if(!strcmp(name, "data"))
        cmd_state->file_len = strtoul(value, NULL, 10);
}

ssize_t handle_line(int sockfd, char* buf, size_t len, struct cmd *cmd_state) {
    ssize_t read_len = -1;
    size_t token_len;
    char *name, *value;
    
    switch(cmd_state->state) {
    case CMD_HEADER:
        if((read_len = readline(buf, len)) > 0) {
            cmd_state->state = CMD_CMD;
        }
        
        break;
    case CMD_CMD:
        if((read_len = readline(buf, len)) > 0 && *buf != '\0') {
            free(cmd_state->filename);
            memset(cmd_state, 0, sizeof(*cmd_state));
            
            if(!strncmp(buf, "close", read_len))
                cmd_state->cmd_type = CLOSE;
            
            if(!strncmp(buf, "save", read_len))
                cmd_state->cmd_type = SAVE;
            
            cmd_state->state = CMD_VAR;
        }
        
        break;
    case CMD_VAR:
        if((read_len = readline(buf, len)) < 0) 
            goto err;
        
        if(*buf == '\0')
            goto err;
        
        if((token_len = strcspn(buf, ":")) >= read_len)
            goto err;
            
        cmd_state->state = CMD_VAR;
        name = buf;
        name[token_len] = '\0';
        value = name + token_len + 1;
        value += strspn(value, " ");
    
        handle_var(name, value, cmd_state);
        if(!strcmp(name, "data"))
            receive_save(sockfd, buf + read_len, len - read_len, cmd_state->filename, cmd_state->file_len);
        break;
        
        err:
        cmd_state->state = CMD_CMD;
        break;
    default:
        break;
    }
    
    return read_len;
}

ssize_t handle_cmds(int sockfd, char* buf, size_t len, struct cmd *cmd_state) {
    size_t total_read_len = 0;
    
    while(total_read_len < len) {
        ssize_t read_len;
        if((read_len = handle_line(sockfd, buf, len, cmd_state)) == -1)
            return -1;
        
        buf += read_len;
        total_read_len += read_len;
    }
    
    return total_read_len;
}

int main(int argc, char *argv[])
{
	int sockfd, fd, numbytes;
    char* filename;
    struct cmd cmd_state = {0};
    
    if((sockfd = connect_mate(HOST, PORT)) == -1) {
        fprintf(stderr, "Could not connect\n");
        return -1;
    }
    
    filename = argv[1];
    if((fd = open(filename, O_RDONLY)) == -1) {
        perror("open");
        return -1;
    }
    
    send_open(sockfd, filename, fd);
    close(fd);

    while(1) {
	    char buf[MAXDATASIZE];
        
        if((numbytes = read(sockfd, buf, MAXDATASIZE-1)) == -1) {
            perror("read");
            return -1;
        }
    
        if(numbytes == 0)
            break;
        buf[numbytes] = '\0';
        
        handle_cmds(sockfd, buf, numbytes, &cmd_state);
    }

	close(sockfd);

	return 0;
}
