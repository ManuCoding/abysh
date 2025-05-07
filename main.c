#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

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

int main(int argc,char** argv) {
	char* pname=argv[0];
	if(argc<1) {
		pname="(abysh)";
		fprintf(stderr,"%s: Warning: weird environment\n",pname);
	}
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
			da_append(&cmd,NULL);
			pid_t pid=fork();
			if(pid==0) {
				int res=execvp(cmd.items[0],cmd.items);
				if(res<0) {
					printf("Unknown command: %s\n",cmd.items[0]);
					return 127;
				}
				fprintf(stderr,"%s: internal error\n",pname);
				return 1;
			} else {
				int status;
				waitpid(pid,&status,0);
				printf("ret: %d\n",WEXITSTATUS(status));
			}
		}
	}
}
