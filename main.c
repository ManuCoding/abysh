#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>

#define MAX_CMD_LEN 4096

typedef struct {
	char** items;
	size_t cap;
	size_t len;
} StrArr;

#define DA_INIT_CAP 4
#define da_append(arr,item)                                         \
	do {                                                            \
		if((arr)->len>=(arr)->cap) {                                \
			if((arr)->cap) (arr)->cap*=2;                           \
			else (arr)->cap=DA_INIT_CAP;                            \
			void* items=malloc((arr)->cap*sizeof(item));            \
			if((arr)->items) {                                      \
				memcpy(items,(arr)->items,(arr)->len*sizeof(item)); \
				free((arr)->items);                                 \
			}                                                       \
			(arr)->items=items;                                     \
		}                                                           \
		(arr)->items[(arr)->len++]=item;                            \
	} while(0)

size_t trim(char** str) {
	size_t len=strnlen(*str,MAX_CMD_LEN);
	while(len && isspace((*str)[--len])) {}
	while(len && (*str)[0] && isspace((*str)[0])) {
		(*str)++;
		len--;
	}
	if(len+1<MAX_CMD_LEN) (*str)[len+1]='\0';
	return len;
}

void parse_args(StrArr* cmd,char* command) {
	size_t len=trim(&command);
	size_t tlen=0;
	for(size_t i=0; i<len; i++) {
		if(isspace(command[i])) {
			if(tlen) {
				command[i]='\0';
				da_append(cmd,command+i-tlen);
				tlen=0;
			}
		} else {
			tlen++;
		}
	}
	if(tlen) da_append(cmd,command+len-tlen);
}

int main() {
	char command[MAX_CMD_LEN];
	StrArr cmd={0};
	while(1) {
		printf("> ");
		command[0]='\0';
		if(!fgets(command,sizeof(command),stdin)) {
			return 0;
		}
		command[MAX_CMD_LEN-1]='\0';
		memset(&cmd,0,sizeof(cmd));
		parse_args(&cmd,command);
		if(cmd.len) {
			if(strcmp(cmd.items[0],"exit")==0) return 0;
			if(strcmp(cmd.items[0],"hax")==0) {
				printf("breaking your code >:)\n");
				fclose(stdin);
			}
			for(size_t i=0; i<cmd.len; i++) {
				printf("got arg: `%s`\n",cmd.items[i]);
			}
		} else {
			printf("welp, empty command\n");
		}
	}
}
